# ConcurrentMemoryPool.h 改动影响分析

> **分析对象**：`src/ConcurrentMemoryPool.h`（分支 `feature/use-project-headers`）  
> **分析原则**：一切以当前代码为准，不修改代码。

---

## 1. 当前代码状态

本分支基于之前调整后的代码，进一步做了以下改动：

| 改动 | 说明 |
|------|------|
| 加入 `definition.h` | 使用其中的 `CACHE_LINE_SIZE` 和 `cpu_relax`（`cpu_relax` 实际来自 `definition.h` 所 include 的 `SpinLock.h`） |
| 加入 `SpinBackoff.h` | 使用其中的模板类 `SpinBackoff` |
| 移除自包含的 `CMP_CACHE_LINE_SIZE` | 统一使用项目公共常量 `CACHE_LINE_SIZE` |
| 移除自定义的 `cpu_relax()` | 使用 `SpinLock.h` 提供的版本 |
| 移除自定义的 `SpinBackoff` 类 | 使用项目公共的 `SpinBackoff` |
| `perform_first_touch` 优化 | 每页只清零第一个字节；使用 `volatile char*` 确保 `-O3` 不会优化掉 |

---

## 2. 对项目其他代码的影响

### 2.1 include 影响

当前代码：

```cpp
#include "definition.h"
#include "SpinBackoff.h"
```

- `definition.h` 中定义了 `TEST_MODE` 宏，并 include 了大量 STL 头文件（`<atomic>`、`<mutex>`、`<barrier>`、`<unordered_map>` 等）。
- 因此，所有 include `ConcurrentMemoryPool.h` 的翻译单元都会间接得到 `TEST_MODE` 宏和这些 STL 头文件。
- 如果项目其他代码对 `TEST_MODE` 敏感（例如条件编译），这可能会改变其行为。

### 2.2 公共 API

公共 API 与原始代码保持一致，没有新增或删除接口。

### 2.3 行为变更

| 变更点 | 说明 | 影响 |
|--------|------|------|
| 跨 Arena 窃取顺序 | 按 NUMA 距离排序 | 低 |
| CAS 失败退让 | 使用项目 `SpinBackoff` | 低 |
| `perform_first_touch` | 每页只 touch 第一个字节 | 中：first-touch 语义仍然成立，但写入量减少，速度更快 |
| 失败处理 | `std::exit(-1)` | 无 |

### 2.4 `TEST_MODE` 宏的潜在影响

`definition.h` 开头：

```cpp
#define TEST_MODE
```

这个宏 unconditionally 定义。如果项目中某些代码使用 `#ifdef TEST_MODE` 或 `#ifndef TEST_MODE` 做条件编译，那么所有 include `ConcurrentMemoryPool.h` 的地方都会处于 `TEST_MODE` 状态。

**建议**：如果 `TEST_MODE` 不应该在生产代码中全局生效，需要检查其使用范围。

---

## 3. 并发安全性分析

### 3.1 基本并发模型

- **线程本地栈**：`thread_local ThreadLocalCache tls_cache_`，每个线程独立。
- **Arena 中央空闲链表**：由 `std::mutex` 保护。
- **Arena bump 指针**：由 CAS 原子推进。
- **远程释放列表**：每个线程维护自己的 `remote_lists`，满时批量归还到目标 Arena（lock 目标 Arena mutex）。

### 3.2 场景推演

#### 场景 A：多线程同时从同一 Arena allocate

- `central_free_list` 访问受 mutex 保护。
- `bump_cursor` 使用 CAS，多线程竞争时失败者重试，不会重复分配。
- 使用项目 `SpinBackoff`，失败后做 `cpu_relax`/yield/sleep 退让。

**结论：安全。**

#### 场景 B：一线程 allocate，另一线程 deallocate 到同一 Arena

- allocate 路径会 lock `arena.mutex`（从 central 取块）或仅 CAS bump_cursor。
- deallocate 到本地栈无锁；本地栈满时 lock `arena.mutex` 批量归还。
- 两者不会同时持有同一 mutex。

**结论：安全。**

#### 场景 C：线程 T1 释放属于 Arena 1 的块，T1 的 local_arena 是 Arena 0

- T1 把块放入自己的 `remote_lists[1]`，无锁。
- 当 `rl.count >= REMOTE_LIST_CAPACITY` 时，T1 lock `arenas_[1].mutex` 批量归还。
- 同时 T2 若从 Arena 1 allocate，会 lock `arenas_[1].mutex`，互斥。

**结论：安全。**

#### 场景 D：TLS 析构与 pool 析构的生命周期顺序

```cpp
inline ThreadLocalCache::~ThreadLocalCache()
{
    if (!pool)
        return;

    while (local_free_stack)
    {
        FreeBlock* block = local_free_stack;
        ...
        int arena_idx = pool->find_arena_index(block);   // 若 pool 已释放，UB
        ...
    }
    ...
}
```

- `~ConcurrentMemoryPool()` 只清理**当前线程**的 `tls_cache_`。
- 但 `ThreadLocalCache` 是 `thread_local`，每个使用过的线程退出时都会析构自己的 TLS。
- 如果 `ConcurrentMemoryPool` 对象在这些线程之前被析构，则 `~ThreadLocalCache` 会访问已释放的 `pool` 对象，导致未定义行为。

**这是原代码就存在的生命周期风险，当前改动未修复。**

### 3.3 并发分析总结

| 场景 | 安全性 | 说明 |
|------|--------|------|
| 多线程本地 allocate | ✅ 安全 | thread_local，无共享 |
| 同一 Arena central 分配 | ✅ 安全 | mutex 保护 |
| 同一 Arena bump CAS | ✅ 安全 | CAS 推进，SpinBackoff 降低开销 |
| allocate + deallocate 同 Arena | ✅ 安全 | mutex 互斥 |
| 远程释放批量归还 | ✅ 安全 | 只 lock 目标 Arena |
| TLS 析构晚于 pool 析构 | ⚠️ 有风险 | 原已存在，未修复 |

---

## 4. 内存对齐优化评估

### 4.1 `CACHE_LINE_SIZE` 来源

当前代码通过 `definition.h` 使用 `CACHE_LINE_SIZE = 64`：

```cpp
#include "definition.h"
...
struct alignas(CACHE_LINE_SIZE) Arena { ... };
```

这比自定义 `CMP_CACHE_LINE_SIZE` 更好，因为统一使用项目公共常量。

### 4.2 `RemoteListHead` 的缓存行填充

```cpp
struct alignas(CACHE_LINE_SIZE) RemoteListHead
{
    FreeBlock* head;        // 8 bytes
    size_t count;           // 8 bytes
    char padding[CACHE_LINE_SIZE - sizeof(FreeBlock*) - sizeof(size_t)];

    RemoteListHead() : head(nullptr), count(0)
    {
        static_assert(sizeof(RemoteListHead) == CACHE_LINE_SIZE,
                      "RemoteListHead must occupy exactly one cache line");
    }
};
```

**上下文**：`RemoteListHead` 是 `ThreadLocalCache::remote_lists[MAX_NUMA_NODES]` 的元素，而 `ThreadLocalCache` 是 `thread_local`。

**分析**：

- `remote_lists` 属于单个线程，不存在跨线程访问。
- 因此，把 `RemoteListHead` 填充到 64 字节解决的是一个**不存在的问题**。

**结论**：**多余的优化**。不会出错，但每个 `ThreadLocalCache` 多占用 `16 × 48 = 768` 字节 TLS 空间。

### 4.3 `alignas(CACHE_LINE_SIZE) Arena`

```cpp
struct alignas(CACHE_LINE_SIZE) Arena { ... };
```

**Arena 实际大小估算**（x86_64 Linux）：

| 成员 | 大小 |
|------|------|
| `void* start_addr` | 8 |
| `void* end_addr` | 8 |
| `std::atomic<char*> bump_cursor` | 8 |
| `FreeBlock* central_free_list` | 8 |
| `std::mutex mutex` | 约 40（glibc pthread_mutex_t） |
| **合计** | **约 72 字节** |

**分析**：

- `alignas(64)` 只保证每个 `Arena` 从 64 字节边界开始。
- 但 Arena 大小不是 64 的倍数，相邻 Arena 仍可能共享缓存行。

**结论**：**部分有效**，能减少但不能完全消除 Arena 间伪共享。不会出错。

### 4.4 对齐优化总结

| 优化点 | 是否多余 | 是否出错 | 说明 |
|--------|----------|----------|------|
| `RemoteListHead` 填充 | 是 | 否 | 线程本地数据无需防伪共享 |
| `alignas` 对 `Arena` | 部分多余 | 否 | Arena 大小非 64 倍数，无法完全消除伪共享 |
| 使用 `CACHE_LINE_SIZE` | 否 | 否 | 统一项目公共常量 |

---

## 5. CAS 内存序分析

### 5.1 `bump_cursor` 的 CAS

```cpp
char* cursor = arena.bump_cursor.load(std::memory_order_relaxed);
...
if (arena.bump_cursor.compare_exchange_weak(cursor, next,
    std::memory_order_relaxed,
    std::memory_order_relaxed))
{
    return cursor;
}
```

**分析**：

- `bump_cursor` 是唯一的同步点。成功 CAS 后，当前线程获得 `[cursor, next)` 这段内存的独占权。
- 这段内存在此之前未被分配，其他线程不会访问。
- 失败者只是重试读取新的 cursor 值。
- `relaxed` 足够保证 cursor 值本身的一致性。

**结论**：**不需要更高内存序**。

### 5.2 `arenas_initialized_`

```cpp
// init_arenas 末尾
arenas_initialized_.store(true, std::memory_order_release);

// allocate 开头
if (!arenas_initialized_.load(std::memory_order_acquire)) { ... }
```

**分析**：

- `release` 保证 Arena 初始化写入对后续 `acquire` 线程可见。
- `acquire` 保证读取到 `true` 后，能看到 release 之前的所有写入。
- 这是正确的发布-订阅模式。

**结论**：**内存序合适**。

### 5.3 CAS 内存序总结

| 原子变量/操作 | 当前内存序 | 是否合适 | 说明 |
|---------------|------------|----------|------|
| `bump_cursor` load/CAS | `relaxed` | 是 | cursor 是唯一同步点 |
| `arenas_initialized_` store/load | `release/acquire` | 是 | 正确发布 Arena 初始化 |

---

## 6. `perform_first_touch` 分析

当前代码：

```cpp
char* start = static_cast<char*>(arenas_[arena_idx].start_addr);
for (size_t p = start_page; p < end_page; ++p)
{
    // 只触摸每页的第一个字节，使用 volatile 防止 O3 优化掉
    volatile char* vp = start + p * page_size_;
    *vp = 0;
}
```

### 6.1 功能正确性

- 按 `page_size_`（实际可能是 4KB 或 2MB）步进。
- 每页只写入第一个字节，足以触发缺页异常，让内核在该 NUMA 节点上分配物理页。
- 使用 `volatile char*` 确保编译器在 `-O3` 下不会将写入优化掉。

### 6.2 与原来全页清零的对比

| 方案 | 优点 | 缺点 |
|------|------|------|
| 原方案：每页每个字节清零 | 彻底 first-touch | 写入量大，耗时 |
| 当前方案：每页第一个字节清零 | 速度快，仍能触发页面分配 | 若后续访问同一页的其他字节，可能触发额外的 minor fault（但通常 negligible） |

### 6.3 结论

当前方案在功能上能达到 first-touch 目的，且性能更优。`volatile` 的使用正确防止了 `-O3` 优化。

---

## 7. `SpinBackoff.h` 的使用

当前代码：

```cpp
SpinBackoff backoff;
for (;;)
{
    ...
    if (arena.bump_cursor.compare_exchange_weak(cursor, next,
        std::memory_order_relaxed,
        std::memory_order_relaxed))
    {
        return cursor;
    }
    backoff.backoff();
}
```

### 7.1 接口匹配

项目 `SpinBackoff.h` 提供的接口是：

```cpp
template <int MAX_BACKOFF = 256, int YIELD_THRESHOLD = 64, int SLEEP_THRESHOLD = 128>
class SpinBackoff {
public:
    void backoff();
    void decay();
    void reset();
};
```

当前代码使用默认模板参数，调用 `backoff.backoff()`，接口匹配正确。

### 7.2 行为对比

| 特性 | 原自定义 SpinBackoff | 项目 SpinBackoff |
|------|---------------------|------------------|
| 初始退让 | 1 次 `cpu_relax` | 1 次 `cpu_relax` |
| 增长方式 | 指数增长，上限 16 | 指数增长，上限 256 |
| yield 阈值 | 无 | count >= 64 时 `std::this_thread::yield()` |
| sleep 阈值 | 无 | count >= 128 时 sleep 1ms |

项目 `SpinBackoff` 在高竞争下会 yield/sleep，比原自定义版本更保守，但 CPU 占用更低。

---

## 8. 结论

### 8.1 对项目其他代码的影响

- **公共 API 无变化**。
- **主要新增影响**：`definition.h` 的 `TEST_MODE` 宏会被传播到所有 include `ConcurrentMemoryPool.h` 的翻译单元。
- 行为上只有跨 Arena 窃取顺序、CAS 退让策略、`perform_first_touch` 写入量有微调。

### 8.2 并发安全性

- 当前改动没有引入新的数据竞争或死锁。
- TLS 生命周期问题（pool 先析构）原已存在。

### 8.3 内存对齐

- 使用项目公共 `CACHE_LINE_SIZE` 是正确的。
- `RemoteListHead` 填充仍然多余（线程本地数据）。
- `Arena` 的 `alignas(64)` 部分有效。

### 8.4 CAS 内存序

- 当前内存序选择合理，无需提升。

### 8.5 `perform_first_touch`

- 每页 touch 第一个字节足以触发页面分配，使用 `volatile` 防止 `-O3` 优化，实现正确。
