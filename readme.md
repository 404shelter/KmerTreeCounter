# tree_v4 — High-Performance K-mer Counter

A header-only C++20, template-driven, lock-free k-mer counter for FASTQ files. Uses x86-64 SIMD (AVX2/SSE4.2), NUMA-aware memory allocation with huge pages, and an adaptive multi-threaded pipeline.

## Pipeline Architecture

```
FASTQ file
    ↓
[FastqReader] (1 thread)
    reads FASTQ in chunks, parses Header→Sequence→Plus→Quality records,
    pushes raw data blocks
    ↓ RingMemoryPool (bipartite lock-free ring buffer)
[FastqParser × N] (N threads via ParserThreadPool)
    extracts canonical k-mers, batches by root prefix (first 4 bases, 256 buckets)
    checks each k-mer against per-root ConcurrentDoubleBloomFilter
    ├─ First occurrence → bloom filter insert → export directly via export_ring_pool
    └─ Repeated → accumulates in per-thread local root nodes → periodically flushes to KmerTree
    ↓ (on overflow: LayerQueues depth 0)
[KmerTree — 4-level radix tree]
    ├─ Root layer (256 nodes, indexed by first 4 bases)
    ├─ Level 1   (16 children/node, indexed by next 2 bases)
    ├─ Level 2   (16 children/node, indexed by next 2 bases)
    └─ Level 3   (16 children/node, leaf, 2 bases) → ConcurrentCountingHashMap per node
    ↓ LayerQueues (for routing) / export_ring_pool
[SchedulerThreadPool] (1 scheduler + M workers)
    ├─ Scheduler: monitors queue pressure across 4 depth queues,
    │   adaptively assigns worker threads to balance load
    └─ Workers: dequeue tasks from assigned depth, route k-mers to child nodes,
        lazily create CountingHashMaps at leaf level
    ↓ export_ring_pool
[ExportWriter] (1 thread)
    consumes first-occurrence k-mers from parser, groups by root prefix (256 buckets),
    writes `low_{prefix}.bin` files
    ↓
[Final Drain] (parallel phase, post-pipeline)
    traverses entire tree, drains remaining k-mers from node buffers into
    global hash map or FinalDrainWriter (`root_{id}.bin` files)
```

## Component Details

| File | Role |
|------|------|
| `main.cpp` | Entry point, CLI argument parsing, pipeline orchestration, timing measurement |
| `definition.h` | All compile-time constants (pool sizes, queue capacities, k-mer params) and shared type definitions (`Task`, `KmerBatch`, `ExportBlock`, etc.) |
| `kmer.h` | `kmer<N>` — bit-packed 2-bit/base k-mer stored in N `uint64_t` words; `kmer_block<N>` — 4KB-aligned fixed-capacity batch of k-mers |
| `GetKmer.h` | `GetKmer<N>` — sliding-window k-mer extraction from raw DNA stream. Maintains both forward and reverse complement simultaneously using bit manipulation |
| `FastqReader.h` | `FastqReader<N>` — single-threaded FASTQ state-machine parser (Header → Sequence → Plus → Quality). Uses `posix_fadvise` for sequential readahead, handles cross-block k-mer overlap |
| `RingMemoryPool.h` | `RingMemoryPool<C>` — bipartite lock-free ring buffer with two MPMC queues (producer→consumer + consumer→producer), enabling fixed-size block reuse |
| `RingLockQueue.h` | `RingLockQueue<T,C>` — condition-variable-based blocking ring queue (available but not used in main pipeline) |
| `FastqParser.h` | `FastqParser<N>` — canonical k-mer extractor. For each root prefix bucket, checks the per-root `ConcurrentDoubleBloomFilter`: first-occurrence k-mers are exported immediately via `export_ring_pool`; repeated k-mers accumulate in per-thread local root nodes and flush to `KmerTree` via `main_add_kmer_block_with_local_root_nodes()` |
| `ParserThreadPool.h` | `ParserThreadPool<N>` — manages N `FastqParser` threads, each consuming from the shared `RingMemoryPool` |
| `NewKmerTree.h` | `KmerTree<N>` — the core 4-level radix tree. Provides `main_add_kmer*()` for parser insertion, `thread_add_kmer()` for worker routing, `insert_kmer_in_task_to_node_hash_map()` for leaf counting, `final_drain_parallel()` for cleanup, and per-root `ConcurrentDoubleBloomFilter` instances for first-occurrence detection |
| `LayerQueues.h` | `LayerQueues<N>` — array of 4 `MPMCRingQueue<Task>` instances (one per tree depth 0–3) plus a final-drain queue, with atomic size tracking for the scheduler |
| `SchedulerThreadPool.h` | `SchedulerThreadPool<N>` — adaptive scheduling engine. Scheduler thread computes score-based pressure metrics to dynamically assign worker threads to queue depths; workers dequeue and process tasks |
| `SpinLock.h` | `SpinLock` — TATAS (test-and-test-and-set) spinlock with exponential backoff, yield-after-spin, and optional spin-loop counting for testing |
| `ConcurrentMemoryPool.h` | `ConcurrentMemoryPool` — NUMA-aware 4KB-block allocator. Per-node arenas, thread-local caching (local free stack + remote lists), 2MB huge page support with transparent huge page fallback |
| `MPMCRingQueue.h` | `MPMCRingQueue<T,C>` — lock-free multi-producer multi-consumer ring queue with a 4-state slot machine (EMPTY → STORING → STORED → LOADING) |
| `BloomFilter.h` | `ConcurrentDoubleBloomFilter<N>` — thread-safe double-layer bloom filter using atomic `fetch_or` for concurrent insertion and duplicate detection |
| `ConcurrentCountingHashMap.h` | `ConcurrentCountingHashMap<N>` — SIMD-accelerated (AVX2/SSE4.2) concurrent hash map with 4KB page-aligned NodeBlocks, lock-free probing with lock-based chain extension |
| `CountingHashTable.h` | `CountingHashTable<N,B,T>` — single-threaded open-addressing hash table with SIMD-accelerated control-byte matching, used as a thread-local accumulator before flushing to the global map |
| `ConcurrentMap.h` | `ConcurrentMap<N>` — lock-free concurrent hash map using linked-list buckets with CAS-based insertion, stores final high-frequency k-mer counts |
| `ExportWriter.h` | `ExportWriter<N>` — single-threaded writer consuming from the export ring pool, groups first-occurrence k-mers by root prefix (first 4 bases, 256 buckets), appends to `low_{prefix}.bin` files |
| `FinalDrainWriter.h` | `FinalDrainWriter` — per-root file writer for the final drain phase, writes sorted k-mer records to `root_{id}.bin` |
| `ExportReader.h` | `ExportReader<N>` — reads back exported `low_{prefix}.bin` files (for post-processing or analysis) |
| `HashFunction.h` | `hash_func<N>()` — simple hash utility for k-mers |
| `SplitMix.h` | `SplitMix64` — thread-safe split-mix pseudo-random number generator |
| `FixedStack.h` | `FixedStack<T,N>` — fixed-capacity stack (used for thread-local task stacks) |
| `FixedMinHeap.h` | `FixedMinHeap<T,C>` — fixed-capacity min-heap (available utility) |

## Key Design Highlights

- **Lock-free data flow**: MPMC ring queues for all cross-thread communication; atomic bloom filter and CAS-based hash map insertion; no global pipeline locks.
- **NUMA-aware memory**: Per-NUMA-node arenas with first-touch initialization and thread-local caching; automatic remote-list batching for cross-node frees.
- **Cache-line alignment**: All hot structures (`node`, `Task`, `MPMCRingQueue` head/tail, `Arena`, `SpinLock` counters) padded to 64-byte cache lines to prevent false sharing.
- **Adaptive scheduling**: Scheduler computes per-depth pressure scores (fill ratio, burst, trend, upstream pressure) and dynamically reassigns worker threads to balance load across tree depths.
- **Early duplicate detection with two-path export**: Parsers check each k-mer against a per-root `ConcurrentDoubleBloomFilter`. First occurrences are immediately exported as singletons; repeated k-mers flow into the radix tree and are counted in leaf-level `ConcurrentCountingHashMap` instances.
- **Thread-local aggregation**: Parsers accumulate k-mers in local root nodes (reducing tree lock contention); workers use local hash tables and local task stacks (reducing queue pressure).

## Build & Usage

### Dependencies
- C++20 compiler (GCC 11+ / Clang 14+)
- pthread, libnuma, libaio

### Build
```bash
cmake -B build
cmake --build build
```

### Run
```bash
./Tree <fastq_file> <k_len> <n_thread> <memory_limit_gb> [map_capacity] [min_count] [max_count] [parser_threads]
```

### Parameters
| Parameter | Description |
|-----------|-------------|
| `fastq_file` | Input FASTQ file path |
| `k_len` | K-mer length (≤ 128) |
| `n_thread` | Total thread count (≥ 3) |
| `memory_limit_gb` | Memory budget in GB |
| `map_capacity` | Hash map bucket capacity (default: 16384) |
| `min_count` | Minimum count threshold for export (default: 1) |
| `max_count` | Maximum count threshold for export (default: unlimited) |
| `parser_threads` | Override parser thread count |
