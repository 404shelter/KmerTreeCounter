# ConcurrentMemoryPool.h 改动影响分析

> **分析对象**：`src/ConcurrentMemoryPool.h`（当前 main 分支）  
> **分析原则**：一切以当前代码为准，不修改代码。

---

## 1. 当前代码状态

经过后续调整，当前代码保留的改动：

| 保留的改动 | 说明 |
|-----------|------|
| 自包含缓存行大小 `CMP_CACHE_LINE_SIZE` | 避免依赖 `definition.h` 的 include 顺序 |
| `RemoteListHead` 缓存行填充 | 填充至 64 字节 |
| `Arena` 按缓存行对齐 | `alignas(CMP_CACHE_LINE_SIZE)` |
| CAS 退避 | `cpu_relax` + `SpinBackoff` |
| NUMA 距离排序 | 跨 Arena 窃取时按 `numa_distance()` 排序 |
| first-touch 修复 | 修复剩余线程分配到无 CPU 节点的 bug |
| 大页路径清理 | 移除重复的 `madvise(MADV_HUGEPAGE)` |

已被撤销的改动：

| 撤销的改动 | 说明 |
|-----------|------|
| `ThreadLocalCache` 整体对齐 | 已移除 `alignas` |
| 统计功能 | 已移除 `MemoryPoolStats`、`get_stats`、`print_stats` 及所有原子统计变量 |
| 异常处理 | 已改回 `std::exit(-1)` |

---

## 2. 对项目其他代码的影响

**公共 API 完全恢复为原始接口**，没有新增也没有破坏：

- `ConcurrentMemoryPool(size_t total_bytes)`
- `~ConcurrentMemoryPool()`
- `void* allocate()`
- `void* allocate_large(size_t bytes)`
- `void deallocate(void* ptr)`
- `void* allocate_before_init_arenas(uint64_t bytes)`
- `void init_arenas()`
- `void perform_first_touch(size_t num_threads = 0)`
- `int get_current_arena_index() const`
- `static ThreadLocalCache& get_thread_local_cache()`

**行为变更**：

| 变更点 | 原行为 | 新行为 | 影响 |
|--------|--------|--------|------|
| 跨 Arena 窃取顺序 | 轮询 `(local + 1) % num_arenas_` | 按 NUMA 距离排序 | 低：功能结果相同，仅顺序变化 |
| CAS 失败退让 | 忙等重试 | `cpu_relax` + 指数退避 | 低：减少高竞争 CPU 开销 |
| `mmap_total_memory` | 普通页路径重复 `madvise` | 仅一次 | 无 |
| 失败处理 | `std::exit(-1)` | `std::exit(-1)` | 无：已改回 |

**结论**：当前代码对项目其他代码的功能没有影响，行为上只有跨 Arena 窃取顺序和 CAS 退让策略的微调。

---

## 3. 并发安全性分析

### 3.1 基本并发模型

- **线程本地栈**：`thread_local ThreadLocalCache tls_cache_`，每个线程独立，无共享。
- **Arena 中央空闲链表**：由 `std::mutex` 保护。
- **Arena bump 指针**：由 CAS 原子推进。
- **远程释放列表**：每个线程维护自己的 `remote_lists`，满时批量归还到目标 Arena（lock 目标 Arena mutex）。

### 3.2 场景推演

#### 场景 A：多线程同时从同一 Arena allocate

- `central_free_list` 访问受 mutex 保护。
- `bump_cursor` 使用 CAS，多线程竞争时失败者重试，不会重复分配。
- `SpinBackoff` 的加入降低了 CAS 失败时的 CPU 开销。

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

### 4.1 `RemoteListHead` 的缓存行填充

```cpp
struct alignas(CMP_CACHE_LINE_SIZE) RemoteListHead
{
    FreeBlock* head;        // 8 bytes
    size_t count;           // 8 bytes
    char padding[48];       // 64 - 16 = 48

    RemoteListHead() : head(nullptr), count(0)
    {
        static_assert(sizeof(RemoteListHead) == CMP_CACHE_LINE_SIZE,
                      "RemoteListHead must occupy exactly one cache line");
    }
};
```

**上下文**：`RemoteListHead` 是 `ThreadLocalCache::remote_lists[MAX_NUMA_NODES]` 的元素，而 `ThreadLocalCache` 是 `thread_local`。

**分析**：

- `remote_lists` 属于单个线程，不存在跨线程访问。
- 因此，把 `RemoteListHead` 填充到 64 字节解决的是一个**不存在的问题**。

**结论**：**多余的优化**。不会出错，但每个 `ThreadLocalCache` 多占用 `16 × 48 = 768` 字节 TLS 空间。

### 4.2 `alignas(CMP_CACHE_LINE_SIZE) Arena`

```cpp
struct alignas(CMP_CACHE_LINE_SIZE) Arena { ... };
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

### 4.3 `ThreadLocalCache` 对齐

当前代码：

```cpp
struct ThreadLocalCache { ... };
```

`alignas` 已被移除。

**结论**：符合用户要求。由于 TLS 块通常不连续相邻，整体对齐收益有限，移除不影响正确性。

### 4.4 对齐优化总结

| 优化点 | 是否多余 | 是否出错 | 说明 |
|--------|----------|----------|------|
| `RemoteListHead` 填充 | 是 | 否 | 线程本地数据无需防伪共享 |
| `alignas` 对 `Arena` | 部分多余 | 否 | Arena 大小非 64 倍数，无法完全消除伪共享 |
| `alignas` 对 `ThreadLocalCache` | 已移除 | — | 按用户要求撤销 |

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

### 5.3 `pre_arena_offset_`

```cpp
alignas(CMP_CACHE_LINE_SIZE) std::atomic<size_t> pre_arena_offset_{0};
```

在 `allocate_before_init_arenas` 中由 `pre_arena_mutex_` 保护访问。

**分析**：

- mutex 已提供同步，atomic 的 memory order 不重要。
- 当前代码使用 operator T() 和 operator+=()，默认是 seq_cst，安全但偏保守。
- 由于 mutex 保护，即使使用普通 `size_t` 也不会出错。

**结论**：安全，但 `std::atomic` 在此场景下略显多余。

### 5.4 CAS 内存序总结

| 原子变量/操作 | 当前内存序 | 是否合适 | 说明 |
|---------------|------------|----------|------|
| `bump_cursor` load/CAS | `relaxed` | 是 | cursor 是唯一同步点 |
| `arenas_initialized_` store/load | `release/acquire` | 是 | 正确发布 Arena 初始化 |
| `pre_arena_offset_` | 默认 seq_cst（mutex 保护） | 是 | mutex 已提供同步 |

---

## 6. 结论

### 6.1 对项目其他代码的影响

- **公共 API 无变化**。
- 失败处理已改回 `std::exit(-1)`，行为与原始代码一致。
- 跨 Arena 窃取顺序和 CAS 退让策略有微调，不影响功能正确性。

### 6.2 并发安全性

- 当前改动没有引入新的数据竞争或死锁。
- TLS 生命周期问题（pool 先析构）原已存在。

### 6.3 内存对齐

- `RemoteListHead` 填充多余（线程本地数据）。
- `Arena` 的 `alignas(64)` 部分有效，但不能完全消除 Arena 间伪共享。
- `ThreadLocalCache` 整体对齐已按用户要求移除。

### 6.4 CAS 内存序

- 当前内存序选择合理，无需提升。
