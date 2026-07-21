// 测试 ConcurrentMemoryPool 的 spill（磁盘溢出）功能
// 1. 匿名内存耗尽后继续分配成功（spill 接管，不退出）
// 2. anon + spill 块数据完整、指针唯一
// 3. allocate_large 跨 spill 分配成功
// 4. 释放的 spill 块被复用（文件不再增长）
// 5. 多线程混合 allocate/deallocate 越过 spill 边界
// 6. 析构后临时文件被清理

#include "../src/ConcurrentMemoryPool.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "CHECK FAILED: " #cond " @line " << __LINE__ << std::endl; \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static off_t file_size_of(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return -1;
    return st.st_size;
}

// 小池 8MB → 2048 个 4KB 块，分配量显著超过它以强制 spill
constexpr size_t POOL_BYTES = 8ULL * 1024 * 1024;
constexpr size_t TOTAL_ALLOCS = 3600;                   // ≈14MB，远超池容量
// 池只有 2048 块匿名块；留足余量后，之后的块必为 spill 块
constexpr size_t SPILL_PTRS_BEGIN = 2200;

int main()
{
    const std::string spill_path = temp_dir + "file_mapping.tmp";
    unlink(spill_path.c_str()); // 清理可能的历史残留
    mkdir(temp_dir.c_str(), 0755);

    //==========================================================================
    // 单线程：基本 spill / 数据完整 / 指针唯一 / allocate_large / 释放复用
    //==========================================================================
    {
        ConcurrentMemoryPool pool(POOL_BYTES);
        pool.init_arenas();

        // ---- Phase 1: 分配超过池容量，未退出即证明 spill 接管 ----
        std::vector<char*> ptrs;
        ptrs.reserve(TOTAL_ALLOCS);
        for (size_t i = 0; i < TOTAL_ALLOCS; ++i)
        {
            ptrs.push_back(static_cast<char*>(pool.allocate()));
        }
        CHECK(access(spill_path.c_str(), F_OK) == 0); // spill 文件已懒创建
        CHECK(file_size_of(spill_path) > 0);          // 且已提交空间

        // ---- Phase 2: 数据完整性（anon + spill 全部块）----
        for (size_t i = 0; i < ptrs.size(); ++i)
        {
            *reinterpret_cast<uint64_t*>(ptrs[i]) = 0xDEAD0000ULL ^ i;
            ptrs[i][BLOCK_SIZE - 1] = static_cast<char>(i & 0xFF);
        }
        bool data_ok = true;
        for (size_t i = 0; i < ptrs.size(); ++i)
        {
            if (*reinterpret_cast<uint64_t*>(ptrs[i]) != (0xDEAD0000ULL ^ i) ||
                ptrs[i][BLOCK_SIZE - 1] != static_cast<char>(i & 0xFF))
            {
                data_ok = false;
                break;
            }
        }
        CHECK(data_ok);

        // ---- Phase 3: 指针唯一性（无重复块、无重叠）----
        {
            std::vector<char*> sorted(ptrs);
            std::sort(sorted.begin(), sorted.end());
            bool unique_ok = true;
            for (size_t i = 1; i < sorted.size(); ++i)
            {
                if (sorted[i] == sorted[i - 1])
                {
                    unique_ok = false;
                    break;
                }
            }
            CHECK(unique_ok);
        }

        // ---- Phase 4: allocate_large 跨 spill（32MB 远大于剩余匿名空间）----
        constexpr size_t LARGE_BYTES = 32ULL * 1024 * 1024;
        char* big = static_cast<char*>(pool.allocate_large(LARGE_BYTES));
        CHECK(big != nullptr);
        big[0] = 0x11;
        big[LARGE_BYTES / 2] = 0x22;
        big[LARGE_BYTES - 1] = 0x33;
        CHECK(big[0] == 0x11 && big[LARGE_BYTES / 2] == 0x22 && big[LARGE_BYTES - 1] == 0x33);

        // ---- Phase 5: 释放 spill 块后复用，文件不再增长 ----
        std::vector<char*> spill_ptrs(ptrs.begin() + SPILL_PTRS_BEGIN, ptrs.end());
        for (char* p : spill_ptrs)
        {
            pool.deallocate(p);
        }
        const off_t size_before = file_size_of(spill_path);
        CHECK(size_before > 0);

        // 复用验证：重新分配略少于刚释放的数量，应全部命中 free list / 已提交空间
        constexpr size_t REUSE_ALLOCS = TOTAL_ALLOCS - SPILL_PTRS_BEGIN - 64;
        std::vector<char*> reused;
        reused.reserve(REUSE_ALLOCS);
        for (size_t i = 0; i < REUSE_ALLOCS; ++i)
        {
            reused.push_back(static_cast<char*>(pool.allocate()));
        }
        const off_t size_after = file_size_of(spill_path);
        CHECK(size_after == size_before); // 复用未触发新 grow

        // 复用块同样要唯一且可写
        {
            std::vector<char*> sorted(reused);
            std::sort(sorted.begin(), sorted.end());
            bool unique_ok = true;
            for (size_t i = 1; i < sorted.size(); ++i)
            {
                if (sorted[i] == sorted[i - 1])
                {
                    unique_ok = false;
                    break;
                }
            }
            CHECK(unique_ok);
        }
        for (char* p : reused)
        {
            pool.deallocate(p);
        }
        // anon 块仍在 ptrs 中持有（SPILL_PTRS_BEGIN 之前的部分），随池析构
    }

    // ---- Phase 6: 析构后临时文件被清理 ----
    CHECK(access(spill_path.c_str(), F_OK) != 0);

    //==========================================================================
    // 多线程：混合 allocate/deallocate 越过 spill 边界
    //==========================================================================
    {
        ConcurrentMemoryPool pool(POOL_BYTES);
        pool.init_arenas();

        constexpr int NUM_THREADS = 8;
        constexpr size_t ALLOCS_PER_THREAD = 448; // 共 3584 块 ≈14MB > 池容量

        std::vector<std::vector<char*>> per_thread(NUM_THREADS);
        std::vector<std::thread> threads;
        threads.reserve(NUM_THREADS);

        for (int t = 0; t < NUM_THREADS; ++t)
        {
            threads.emplace_back([&pool, &per_thread, t]() {
                auto& local = per_thread[t];
                local.reserve(ALLOCS_PER_THREAD);
                for (size_t i = 0; i < ALLOCS_PER_THREAD; ++i)
                {
                    char* p = static_cast<char*>(pool.allocate());
                    // 写入 (tid, seq) 模式
                    uint64_t* w = reinterpret_cast<uint64_t*>(p);
                    w[0] = static_cast<uint64_t>(t);
                    w[1] = static_cast<uint64_t>(i);
                    local.push_back(p);

                    // 穿插释放：每 8 个释放 1 个（仍在写线程内持有其余块）
                    if (local.size() >= 8 && (i % 8) == 7)
                    {
                        pool.deallocate(local[local.size() / 2]);
                        local.erase(local.begin() + static_cast<std::ptrdiff_t>(local.size() / 2));
                    }
                }
            });
        }
        for (auto& th : threads)
        {
            th.join();
        }

        CHECK(access(spill_path.c_str(), F_OK) == 0);

        // 汇总：指针唯一性 + 数据完整性
        std::vector<char*> all;
        for (int t = 0; t < NUM_THREADS; ++t)
        {
            all.insert(all.end(), per_thread[t].begin(), per_thread[t].end());
        }
        {
            std::vector<char*> sorted(all);
            std::sort(sorted.begin(), sorted.end());
            bool unique_ok = true;
            for (size_t i = 1; i < sorted.size(); ++i)
            {
                if (sorted[i] == sorted[i - 1])
                {
                    unique_ok = false;
                    break;
                }
            }
            CHECK(unique_ok);
        }
        bool data_ok = true;
        for (int t = 0; t < NUM_THREADS && data_ok; ++t)
        {
            for (char* p : per_thread[t])
            {
                const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
                if (w[0] != static_cast<uint64_t>(t))
                {
                    data_ok = false;
                    break;
                }
            }
        }
        CHECK(data_ok);

        // 主线程统一释放（跨线程释放路径：anon→本地栈，spill→spill_remote）
        for (char* p : all)
        {
            pool.deallocate(p);
        }

        // 释放后仍能分配（复用生效）
        std::vector<char*> again;
        for (size_t i = 0; i < 1024; ++i)
        {
            again.push_back(static_cast<char*>(pool.allocate()));
        }
        for (char* p : again)
        {
            pool.deallocate(p);
        }
    }

    // 第二个池析构后文件同样被清理
    CHECK(access(spill_path.c_str(), F_OK) != 0);

    if (g_failures == 0)
    {
        std::cout << "All memory pool spill tests passed." << std::endl;
        return 0;
    }
    std::cerr << g_failures << " checks failed." << std::endl;
    return 1;
}
