#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FlatConcurrentHashMap.h"

namespace
{
constexpr uint64_t ROOT_BUCKET_COUNT = 1ULL << (2 * ROOT_BASES);
constexpr uint64_t BYTES_PER_GIB      = 1024ULL * 1024ULL * 1024ULL;
constexpr char     BASE_CHARS[]       = {'A', 'C', 'G', 'T'};

struct Options
{
    std::string tmp_dir;
    bool        is_precise      = false;
    uint32_t    k_len           = 0;
    uint32_t    max_threads     = 0;
    uint64_t    max_memory_bytes = 0;
    std::string output_file;
    uint32_t    min_freq        = 1;
    uint32_t    max_freq        = std::numeric_limits<uint32_t>::max();
};

struct RootFileInfo
{
    std::string filename;
    uint64_t    record_count = 0;
    uint64_t    file_size    = 0;
};

class ProgressPrinter
{
public:
    explicit ProgressPrinter(uint64_t total) : total_(total) {}

    void start()
    {
        if (total_ == 0) last_pct_.store(100, std::memory_order_relaxed);
        std::cout << "progress 0%        " << std::flush;
    }

    // Thread-safe: called concurrently from multiple workers.
    void add(uint64_t bytes)
    {
        if (total_ == 0 || bytes == 0) return;
        uint64_t done = done_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        uint64_t pct = (done >= total_) ? 100 : (done * 100) / total_;
        if (pct > 99) pct = 99;

        uint64_t prev = last_pct_.load(std::memory_order_relaxed);
        while (pct > prev) {
            if (last_pct_.compare_exchange_weak(prev, pct,
                    std::memory_order_relaxed, std::memory_order_relaxed))
            {
                std::lock_guard<std::mutex> lk(cout_mtx_);
                std::cout << "\rprogress " << std::setw(3) << pct << "%     " << std::flush;
                break;
            }
        }
    }

    void finish()
    {
        bool expected = false;
        if (finished_.compare_exchange_strong(expected, true)) {
            std::cout << "\rprogress 100%     " << std::endl;
        }
    }

private:
    uint64_t                total_{0};
    std::atomic<uint64_t>   done_{0};
    std::atomic<uint64_t>   last_pct_{0};
    std::atomic<bool>       finished_{false};
    std::mutex              cout_mtx_;
};

// k-mer → string  (with correct canonical decode)
static char complement_char(char c)
{
    switch (c) {
    case 'A': return 'T';
    case 'C': return 'G';
    case 'G': return 'C';
    case 'T': return 'A';
    default:  return c;
    }
}

static std::string reverse_complement(const std::string& s)
{
    std::string rc = s;
    std::reverse(rc.begin(), rc.end());
    for (char& ch : rc) ch = complement_char(ch);
    return rc;
}

template <uint32_t N>
static std::string kmer_to_string(const kmer<N>& key, uint32_t k_len)
{
    std::string result(k_len, 'A');
    const uint32_t full_words = k_len / BASES_PER_U64T;
    const uint32_t tail_bases = k_len % BASES_PER_U64T;

    for (uint32_t i = 0; i < k_len; ++i) {
        uint32_t word_idx, bit_shift;
        if (tail_bases > 0 && i < tail_bases) {
            word_idx  = full_words;
            bit_shift = 64 - 2 * tail_bases + 2 * i;
        } else {
            uint32_t off = i - (tail_bases > 0 ? tail_bases : 0);
            word_idx  = full_words - 1 - (off / BASES_PER_U64T);
            bit_shift = 2 * (off % BASES_PER_U64T);
        }
        uint8_t base = (key.data[word_idx] >> bit_shift) & 0x3ULL;
        result[i] = BASE_CHARS[base];
    }

    const std::string rc = reverse_complement(result);
    return result < rc ? result : rc;
}

// Sort-key: canonical k-mer → integer with oldest base at MSB.
// k ≤ 32 → uint64_t   k ≤ 64 → __uint128_t   k ≤ 128 → Key128
using uint128_t = unsigned __int128;

struct Key128 {
    uint64_t hi, lo;
    Key128() : hi(0), lo(0) {}
    Key128(uint64_t l) : hi(0), lo(l) {}
    Key128(uint64_t h, uint64_t l) : hi(h), lo(l) {}
    Key128 operator<<(int shift) const {
        if (shift == 0) return *this;
        if (shift < 64) return { (hi << shift) | (lo >> (64 - shift)), lo << shift };
        return { lo << (shift - 64), 0 };
    }
    Key128 operator>>(int shift) const {
        if (shift == 0) return *this;
        if (shift < 64) return { hi >> shift, (lo >> shift) | (hi << (64 - shift)) };
        return { 0, hi >> (shift - 64) };
    }
    Key128 operator|(uint64_t v) const { return { hi, lo | v }; }
    uint64_t operator&(uint64_t mask) const { return lo & mask; }
    Key128& operator>>=(int shift) { *this = *this >> shift; return *this; }
};

template <uint32_t N> struct SortKeyType;
template <> struct SortKeyType<1> { using type = uint64_t;  static constexpr uint32_t bits = 64;  };
template <> struct SortKeyType<2> { using type = uint128_t; static constexpr uint32_t bits = 128; };
template <> struct SortKeyType<4> { using type = Key128;    static constexpr uint32_t bits = 256; };

// Extract the 2-bit code for base i (0 = oldest) from a kmer.
template <uint32_t N>
static inline uint8_t kmer_base_at(const kmer<N>& key, uint32_t i,
                                   uint32_t full_words, uint32_t tail_bases)
{
    uint32_t word_idx, bit_shift;
    if (tail_bases > 0 && i < tail_bases) {
        word_idx  = full_words;
        bit_shift = 64 - 2 * tail_bases + 2 * i;
    } else {
        uint32_t off = i - (tail_bases > 0 ? tail_bases : 0);
        word_idx  = full_words - 1 - (off / BASES_PER_U64T);
        bit_shift = 2 * (off % BASES_PER_U64T);
    }
    return (key.data[word_idx] >> bit_shift) & 0x3;
}

// Build sort key: oldest base at MSB, newest at LSB.
template <typename KeyT, uint32_t N>
static KeyT make_sort_key(const kmer<N>& kmer, uint32_t k_len)
{
    const uint32_t fw = k_len / BASES_PER_U64T;
    const uint32_t tb = k_len % BASES_PER_U64T;
    KeyT key = 0;
    for (uint32_t i = 0; i < k_len; ++i)
        key = (key << 2) | kmer_base_at<N>(kmer, i, fw, tb);
    return key;
}

// Convert sort key back to string (oldest base already at MSB of key).
template <typename KeyT>
static std::string key_to_string(KeyT key, uint32_t k_len)
{
    std::string result(k_len, 'A');
    for (uint32_t i = k_len; i-- > 0; ) {
        result[i] = BASE_CHARS[key & 0x3];
        key >>= 2;
    }
    return result;
}

// 11-bit radix = 2048 buckets
constexpr uint32_t SORT_RADIX_BITS = 11;
constexpr uint32_t SORT_BUCKETS     = 1U << SORT_RADIX_BITS;
constexpr uint32_t SORT_RADIX_MASK  = SORT_BUCKETS - 1;

static inline uint32_t extract_digit(uint64_t key, uint32_t shift) {
    return (key >> shift) & SORT_RADIX_MASK;
}
static inline uint32_t extract_digit(uint128_t key, uint32_t shift) {
    return static_cast<uint32_t>((key >> shift) & SORT_RADIX_MASK);
}
static inline uint32_t extract_digit(Key128 key, uint32_t shift) {
    return static_cast<uint32_t>((key >> shift) & SORT_RADIX_MASK);
}

// SortEntry: compact {key, count} for radix sort
template <typename KeyT>
struct SortEntry {
    KeyT     key;
    uint32_t count;
};

// Parallel LSD radix sort — sorts SortEntry[] by key in-place
template <typename KeyT>
static void parallel_lsd_radix_sort(std::vector<SortEntry<KeyT>>& entries,
                                    uint32_t total_bits, uint32_t worker_count)
{
    uint64_t n = entries.size();
    if (n <= 1) return;

    std::vector<SortEntry<KeyT>> temp(n);
    uint32_t passes = (total_bits + SORT_RADIX_BITS - 1) / SORT_RADIX_BITS;
    SortEntry<KeyT>* src = entries.data();
    SortEntry<KeyT>* dst = temp.data();

    for (uint32_t pass = 0; pass < passes; ++pass) {
        uint32_t shift = pass * SORT_RADIX_BITS;

        // ── parallel histogram ──
        std::vector<std::vector<uint64_t>> local_hists(worker_count,
            std::vector<uint64_t>(SORT_BUCKETS, 0));

        uint64_t per_thread = (n + worker_count - 1) / worker_count;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (uint32_t tid = 0; tid < worker_count; ++tid) {
            workers.emplace_back([&, tid]() {
                uint64_t start = tid * per_thread;
                uint64_t end   = std::min<uint64_t>(start + per_thread, n);
                if (start >= end) return;
                auto& hist = local_hists[tid];
                for (uint64_t i = start; i < end; ++i)
                    hist[extract_digit(src[i].key, shift)]++;
            });
        }
        for (auto& w : workers) w.join();

        // global prefix sum
        uint64_t global_hist[SORT_BUCKETS];
        uint64_t sum = 0;
        for (uint32_t b = 0; b < SORT_BUCKETS; ++b) {
            uint64_t cnt = 0;
            for (uint32_t t = 0; t < worker_count; ++t)
                cnt += local_hists[t][b];
            global_hist[b] = sum;
            sum += cnt;
        }

        // per-thread write cursors
        for (uint32_t b = 0; b < SORT_BUCKETS; ++b) {
            uint64_t off = global_hist[b];
            for (uint32_t t = 0; t < worker_count; ++t) {
                uint64_t cnt = local_hists[t][b];
                local_hists[t][b] = off;
                off += cnt;
            }
        }

        // ── parallel scatter ──
        workers.clear();
        for (uint32_t tid = 0; tid < worker_count; ++tid) {
            workers.emplace_back([&, tid]() {
                uint64_t start = tid * per_thread;
                uint64_t end   = std::min<uint64_t>(start + per_thread, n);
                if (start >= end) return;
                auto& cur = local_hists[tid];
                for (uint64_t i = start; i < end; ++i) {
                    uint32_t b = extract_digit(src[i].key, shift);
                    dst[cur[b]++] = src[i];
                }
            });
        }
        for (auto& w : workers) w.join();

        std::swap(src, dst);
    }

    // Result is in src; copy back to entries if src != entries.data()
    if (src == temp.data())
        entries.swap(temp);
}

// Utilities
[[nodiscard]] static std::string with_trailing_slash(std::string path)
{
    if (path.empty()) return "./";
    char tail = path.back();
    if (tail != '/' && tail != '\\') path.push_back('/');
    return path;
}

[[nodiscard]] static uint32_t parse_u32(const char* text, const char* name)
{
    size_t parsed = 0;
    unsigned long long value = std::stoull(text, &parsed);
    if (parsed != std::strlen(text) || value > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "invalid " << name << ": " << text << '\n';
        exit(-1);
    }
    return static_cast<uint32_t>(value);
}

[[nodiscard]] static uint64_t parse_memory_gib(const char* text)
{
    size_t parsed = 0;
    long double value = std::stold(text, &parsed);
    if (parsed != std::strlen(text) || value <= 0.0L) {
        std::cerr << "invalid max_memory_gb: " << text << '\n';
        exit(-1);
    }
    long double bytes = value * static_cast<long double>(BYTES_PER_GIB);
    if (bytes > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        std::cerr << "max_memory_gb too large\n";
        exit(-1);
    }
    return static_cast<uint64_t>(bytes);
}

[[nodiscard]] static Options parse_options(int argc, char* argv[])
{
    if (argc < 7 || argc > 9) {
        std::cerr << "Usage: dump_tool <precise/approximate> <tmp_dir> <k_len>"
                  << " <max_threads> <max_memory_gb> <output_file>"
                  << " [min_freq=1] [max_freq=max]\n";
        exit(-1);
    }
    Options o;
    o.tmp_dir    = with_trailing_slash(argv[2]);
    o.k_len      = parse_u32(argv[3], "k_len");
    o.max_threads = parse_u32(argv[4], "max_threads");
    o.max_memory_bytes = parse_memory_gib(argv[5]);
    o.output_file = argv[6];

    if (std::strcmp(argv[1], "precise") == 0)      o.is_precise = true;
    else if (std::strcmp(argv[1], "approximate") == 0) o.is_precise = false;
    else { std::cerr << "mode must be 'precise' or 'approximate'\n"; exit(-1); }

    if (argc >= 8) o.min_freq = parse_u32(argv[7], "min_freq");
    if (argc >= 9) o.max_freq = parse_u32(argv[8], "max_freq");
    if (o.k_len == 0 || o.k_len > MAX_K)   { std::cerr << "invalid k_len\n"; exit(-1); }
    if (o.max_threads == 0)                 { std::cerr << "invalid max_threads\n"; exit(-1); }
    if (o.max_freq < o.min_freq)            { std::cerr << "max_freq < min_freq\n"; exit(-1); }
    return o;
}

[[nodiscard]] static std::string root_filename(const std::string& dir, uint64_t id)
{
    return dir + "root_" + std::to_string(id) + ".bin";
}

static bool in_range(uint64_t f, uint32_t lo, uint32_t hi) noexcept
{
    return f >= lo && f <= hi;
}

// Packed k-mer byte size
template <uint32_t N>
[[nodiscard]] static uint64_t packed_kmer_bytes_for_k(uint32_t k_len)
{
    uint64_t full_cnt  = k_len / BASES_PER_U64T;
    uint64_t tail_bits = 2ULL * (k_len % BASES_PER_U64T);
    uint64_t tail_bytes = (tail_bits + 7ULL) / 8ULL;
    uint64_t pkb = full_cnt * sizeof(uint64_t) + tail_bytes;
    if (pkb == 0 || k_len > N * BASES_PER_U64T) {
        std::cerr << "invalid k for packed bytes: " << k_len << '\n';
        exit(-1);
    }
    return pkb;
}

// Phase 0 — collect root file metadata
template <uint32_t N>
[[nodiscard]] static std::vector<RootFileInfo>
collect_root_files(const std::string& tmp_dir, uint64_t& total_records)
{
    std::vector<RootFileInfo> files;
    files.reserve(ROOT_BUCKET_COUNT);
    total_records = 0;

    for (uint64_t id = 0; id < ROOT_BUCKET_COUNT; ++id) {
        std::string fn = root_filename(tmp_dir, id);
        int fd = ::open(fn.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) continue;
            std::cerr << "open failed: " << fn << ": " << std::strerror(errno) << '\n';
            exit(-1);
        }
        struct stat st{};
        if (::fstat(fd, &st) != 0) { ::close(fd); std::cerr << "stat failed\n"; exit(-1); }
        ::close(fd);

        uint64_t sz = static_cast<uint64_t>(st.st_size);
        if (sz % sizeof(ExportRecord<N>) != 0) {
            std::cerr << "bad root file size: " << fn << '\n';
            exit(-1);
        }
        uint64_t n = sz / sizeof(ExportRecord<N>);
        total_records += n;
        if (n > 0) files.push_back({std::move(fn), n, sz});
    }
    return files;
}

// Phase 1 — parallel hash map construction from root files
template <uint32_t N>
static void build_hash_map(const std::vector<RootFileInfo>& files,
                           FlatConcurrentHashMap<N>&        map,
                           uint32_t                         worker_count,
                           ProgressPrinter*                 progress)
{
    std::atomic<uint64_t> next_file{0};

    auto worker = [&]() {
        std::vector<ExportRecord<N>> buf(65536);
        for (;;) {
            uint64_t idx = next_file.fetch_add(1, std::memory_order_relaxed);
            if (idx >= files.size()) break;

            const RootFileInfo& fi = files[idx];
            int fd = ::open(fi.filename.c_str(), O_RDONLY);
            if (fd < 0) { std::cerr << "open failed: " << fi.filename << '\n'; exit(-1); }

            uint64_t remaining = fi.record_count;
            uint64_t offset    = 0;
            while (remaining > 0) {
                uint64_t batch = std::min<uint64_t>(remaining, 65536);
                size_t   bytes = static_cast<size_t>(batch * sizeof(ExportRecord<N>));
                ssize_t  n     = ::pread(fd, buf.data(), bytes,
                                         static_cast<off_t>(offset * sizeof(ExportRecord<N>)));
                if (n != static_cast<ssize_t>(bytes)) {
                    std::cerr << "short read: " << fi.filename << '\n'; exit(-1);
                }
                for (uint64_t i = 0; i < batch; ++i)
                    map.insert_unique(buf[i].key, buf[i].count);
                remaining -= batch;
                offset    += batch;
                progress->add(bytes);
            }
            ::close(fd);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (uint32_t i = 0; i < worker_count; ++i)
        workers.emplace_back(worker);
    for (auto& w : workers) w.join();
}

// Unpack a packed k-mer (low.bin format) → kmer<N>
template <uint32_t N>
inline void unpack_packed(const char*   record,
                          kmer<N>&      out,
                          uint64_t      full_data_count,
                          uint64_t      tail_bytes)
{
    uint64_t full_bytes = full_data_count * sizeof(uint64_t);
    if (full_bytes > 0)
        std::memcpy(out.data.data(), record, static_cast<size_t>(full_bytes));

    if (tail_bytes > 0) {
        uint64_t tail = 0;
        std::memcpy(reinterpret_cast<char*>(&tail) + (sizeof(uint64_t) - tail_bytes),
                    record + full_bytes, static_cast<size_t>(tail_bytes));
        out.data[full_data_count] = tail;
        for (uint64_t j = full_data_count + 1; j < N; ++j) out.data[j] = 0;
    } else {
        for (uint64_t j = full_data_count; j < N; ++j) out.data[j] = 0;
    }
}

// Phase 2 — parallel low-frequency processing
template <uint32_t N>
static uint64_t process_low_freq(const std::string&              low_path,
                                 FlatConcurrentHashMap<N>&       map,
                                 std::vector<ExportRecord<N>>&   results,
                                 uint32_t                        k_len,
                                 uint32_t                        min_freq,
                                 uint32_t                        max_freq,
                                 uint32_t                        worker_count,
                                 uint64_t                        packed_kmer_bytes,
                                 ProgressPrinter*                progress)
{
    int fd = ::open(low_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "open low.bin failed: " << std::strerror(errno) << '\n';
        exit(-1);
    }
    struct stat st{};
    ::fstat(fd, &st);
    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint64_t total_kmers = file_size / packed_kmer_bytes;

    const char* data = static_cast<const char*>(
        ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
    ::close(fd);
    if (data == MAP_FAILED) {
        std::cerr << "mmap low.bin failed\n"; exit(-1);
    }

    const uint64_t full_data_count = k_len / BASES_PER_U64T;
    const uint64_t tail_bits       = 2ULL * (k_len % BASES_PER_U64T);
    const uint64_t tail_bytes      = (tail_bits + 7ULL) / 8ULL;

    std::vector<std::vector<ExportRecord<N>>> thread_results(worker_count);
    for (auto& v : thread_results)
        v.reserve(total_kmers / worker_count + 1024);

    uint64_t kmers_per_worker = (total_kmers + worker_count - 1) / worker_count;

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (uint32_t tid = 0; tid < worker_count; ++tid) {
        workers.emplace_back([&, tid]() {
            uint64_t start = tid * kmers_per_worker;
            uint64_t end   = std::min<uint64_t>(start + kmers_per_worker, total_kmers);
            if (start >= end) return;

            auto& local = thread_results[tid];
            using Lookup = typename FlatConcurrentHashMap<N>::PreparedLookup;
            kmer<N> key{};

            for (uint64_t i = start; i < end; ++i) {
                unpack_packed<N>(data + i * packed_kmer_bytes, key,
                                 full_data_count, tail_bytes);

                uint32_t count = 0;
                uint64_t slot  = UINT64_MAX;
                Lookup   lkup  = map.prepare_lookup(key);
                map.prefetch(lkup);

                if (map.find_prepared_slot(key, lkup, count, slot)) {
                    uint64_t merged = static_cast<uint64_t>(count) + 1ULL;
                    if (in_range(merged, min_freq, max_freq))
                        local.push_back(ExportRecord<N>{key, static_cast<uint32_t>(merged)});
                    *map.mutable_count_at(slot) = 0;
                } else if (in_range(1, min_freq, max_freq)) {
                    local.push_back(ExportRecord<N>{key, 1});
                }
            }
            progress->add((end - start) * packed_kmer_bytes);
        });
    }
    for (auto& w : workers) w.join();

    uint64_t total_results = 0;
    for (auto& v : thread_results) total_results += v.size();
    results.reserve(results.size() + total_results);
    for (auto& v : thread_results) {
        results.insert(results.end(), v.begin(), v.end());
        v.clear();
    }

    ::munmap(const_cast<char*>(data), file_size);
    return total_results;
}


// Phase 3 — collect hash entries not zeroed by the low-freq pass
template <uint32_t N>
static void collect_remaining(FlatConcurrentHashMap<N>&       map,
                              std::vector<ExportRecord<N>>&   results,
                              uint32_t                        min_freq,
                              uint32_t                        max_freq)
{
    map.for_each_entry([&](const kmer<N>& key, uint32_t count) {
        if (count > 0 && in_range(count, min_freq, max_freq))
            results.push_back(ExportRecord<N>{key, count});
    });
}


// Phase 4 — compute sort keys, LSD radix sort, write output
template <uint32_t N>
static void sort_and_write(std::vector<ExportRecord<N>>& records,
                           const std::string&            output_file,
                           uint32_t                      k_len,
                           uint32_t                      worker_count)
{
    using KeyT = typename SortKeyType<N>::type;
    constexpr uint32_t key_bits = SortKeyType<N>::bits;
    uint32_t sort_bits = std::min<uint32_t>(key_bits, k_len * 2);

    // ── Build SortEntry array (compute key once per entry) ──
    uint64_t n = records.size();
    std::vector<SortEntry<KeyT>> entries(n);
    const uint32_t fw = k_len / BASES_PER_U64T;
    const uint32_t tb = k_len % BASES_PER_U64T;
    for (uint64_t i = 0; i < n; ++i) {
        entries[i].key   = make_sort_key<KeyT, N>(records[i].key, k_len);
        entries[i].count = records[i].count;
    }

    // ── Parallel LSD radix sort by key ──
    parallel_lsd_radix_sort<KeyT>(entries, sort_bits, worker_count);

    // ── Write sorted output ──
    std::ofstream out(output_file, std::ios::out | std::ios::trunc);
    if (!out) { std::cerr << "failed to open output: " << output_file << '\n'; exit(1); }

    std::string line;
    line.reserve(static_cast<size_t>(k_len) + 32);
    for (uint64_t i = 0; i < n; ++i) {
        line = key_to_string<KeyT>(entries[i].key, k_len);
        out << line << '\t' << entries[i].count << '\n';
    }
}

// precise mode
template <uint32_t N>
static int run_precise(const Options& opts)
{
    temp_dir = opts.tmp_dir;
    uint32_t worker_count = std::max<uint32_t>(1, opts.max_threads);

    uint64_t total_root_records = 0;
    auto root_files = collect_root_files<N>(opts.tmp_dir, total_root_records);
    uint64_t high_bytes = 0;
    for (auto& rf : root_files) high_bytes += rf.file_size;

    uint64_t pkb      = packed_kmer_bytes_for_k<N>(opts.k_len);
    uint64_t low_bytes = 0;
    {
        std::string lp = opts.tmp_dir + "low.bin";
        int fd = ::open(lp.c_str(), O_RDONLY);
        if (fd < 0) { std::cerr << "open low.bin failed\n"; exit(-1); }
        struct stat st{};
        ::fstat(fd, &st); ::close(fd);
        low_bytes = static_cast<uint64_t>(st.st_size);
        if (low_bytes % pkb != 0) {
            std::cerr << "low.bin size not multiple of packed kmer\n"; exit(-1);
        }
    }

    ProgressPrinter progress(high_bytes + low_bytes);
    progress.start();

    uint64_t est_mem = FlatConcurrentHashMap<N>::required_mmap_bytes(total_root_records);
    if (est_mem > opts.max_memory_bytes) {
        std::cerr << "insufficient-memory: need ~" << est_mem
                  << " have " << opts.max_memory_bytes << '\n';
        return 2;
    }

    // Phase 1: build hash map from root files
    FlatConcurrentHashMap<N> hash_map(total_root_records, worker_count);
    build_hash_map<N>(root_files, hash_map, worker_count, &progress);
    hash_map.seal();

    // Phase 2: process low.bin
    std::vector<ExportRecord<N>> results;
    process_low_freq<N>(opts.tmp_dir + "low.bin", hash_map, results,
                        opts.k_len, opts.min_freq, opts.max_freq,
                        worker_count, pkb, &progress);

    // Phase 3: collect remaining hash entries
    collect_remaining<N>(hash_map, results, opts.min_freq, opts.max_freq);

    // Phase 4: sort & write
    sort_and_write<N>(results, opts.output_file, opts.k_len, worker_count);
    progress.finish();

    return 0;
}

// approximate mode  (root files → filter → sort → write)
template <uint32_t N>
static int run_approximate(const Options& opts)
{
    temp_dir = opts.tmp_dir;
    uint32_t worker_count = std::max<uint32_t>(1, opts.max_threads);

    uint64_t total_records = 0;
    auto root_files = collect_root_files<N>(opts.tmp_dir, total_records);
    uint64_t total_bytes = 0;
    for (auto& rf : root_files) total_bytes += rf.file_size;

    ProgressPrinter progress(total_bytes);
    progress.start();

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<ExportRecord<N>>> thread_results(worker_count);
    for (auto& v : thread_results)
        v.reserve(total_records / worker_count + 1024);

    std::atomic<uint64_t> next_file{0};

    auto worker = [&](uint32_t tid) {
        std::vector<ExportRecord<N>> buf(65536);
        auto& local = thread_results[tid];
        for (;;) {
            uint64_t idx = next_file.fetch_add(1, std::memory_order_relaxed);
            if (idx >= root_files.size()) break;

            const RootFileInfo& fi = root_files[idx];
            int fd = ::open(fi.filename.c_str(), O_RDONLY);
            if (fd < 0) { std::cerr << "open failed: " << fi.filename << '\n'; exit(-1); }

            uint64_t remaining = fi.record_count;
            uint64_t offset    = 0;
            while (remaining > 0) {
                uint64_t batch = std::min<uint64_t>(remaining, 65536);
                size_t   bytes = static_cast<size_t>(batch * sizeof(ExportRecord<N>));
                ssize_t  n     = ::pread(fd, buf.data(), bytes,
                                         static_cast<off_t>(offset * sizeof(ExportRecord<N>)));
                if (n != static_cast<ssize_t>(bytes)) {
                    std::cerr << "short read: " << fi.filename << '\n'; exit(-1);
                }
                for (uint64_t i = 0; i < batch; ++i)
                    if (in_range(buf[i].count, opts.min_freq, opts.max_freq))
                        local.push_back(buf[i]);
                remaining -= batch;
                offset    += batch;
                progress.add(bytes);
            }
            ::close(fd);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (uint32_t i = 0; i < worker_count; ++i)
        workers.emplace_back(worker, i);
    for (auto& w : workers) w.join();

    std::vector<ExportRecord<N>> results;
    uint64_t total_out = 0;
    for (auto& v : thread_results) total_out += v.size();
    results.reserve(total_out);
    for (auto& v : thread_results) {
        results.insert(results.end(), v.begin(), v.end());
        v.clear();
    }

    sort_and_write<N>(results, opts.output_file, opts.k_len, worker_count);
    progress.finish();
    return 0;
}

// Dispatch by k-mer length
static int precise_dispatch(const Options& opts)
{
    if (opts.k_len <= 32)          return run_precise<1>(opts);
    else if (opts.k_len <= 64)     return run_precise<2>(opts);
    else if (opts.k_len <= MAX_K)  return run_precise<4>(opts);
    std::cerr << "unsupported k_len: " << opts.k_len << '\n';
    exit(-1);
}

static int approximate_dispatch(const Options& opts)
{
    if (opts.k_len <= 32)          return run_approximate<1>(opts);
    else if (opts.k_len <= 64)     return run_approximate<2>(opts);
    else if (opts.k_len <= MAX_K)  return run_approximate<4>(opts);
    std::cerr << "unsupported k_len: " << opts.k_len << '\n';
    exit(-1);
}

} 

int main(int argc, char* argv[])
{
    Options opts = parse_options(argc, argv);
    int x =  opts.is_precise ? precise_dispatch(opts) : approximate_dispatch(opts);;
}
