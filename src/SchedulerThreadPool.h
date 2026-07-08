#ifndef SCHEDULER_THREAD_POOL_HEADER
#define SCHEDULER_THREAD_POOL_HEADER

#include "definition.h"
#include "LayerQueues.h"
#include "MPMCRingQueue.h"
#include "NewKmerTree.h"
#include "../src/SpinBackoff.h"

#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <utility>
#include <barrier>
#include <limits>
#include <chrono>
#include <mutex>


template <uint32_t N>
class SchedulerThreadPool final
{
    constexpr static uint32_t WORKER_QUEUE_CAPACITY = 4;
    constexpr static uint32_t INVALID_DEPTH = MAX_DEPTH;
    constexpr static uint32_t DRAIN_EMPTY_CONFIRM_ROUNDS = 8;
    constexpr static uint32_t MAX_PROCESS_TASKS = 128;
    static constexpr uint32_t FORCE_DEAL_WITH_LOCAL_STACK_ROUND = 32;

    // Scheduler algorithm constants
    static constexpr uint32_t SCHEDULE_INTERVAL_US = 2;
    static constexpr double PRESSURE_EMA_ALPHA = 0.25;
    static constexpr double HYSTERESIS_RATIO = 1.5;
    static constexpr uint32_t DRAIN_INTERVAL_US = 1;

    struct WorkerInfo
    {
        alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> depth{ INVALID_DEPTH };
    };

    const uint32_t thread_count_;
    alignas(CACHE_LINE_SIZE) std::atomic<bool> running_{ false };
    alignas(CACHE_LINE_SIZE) std::atomic<bool> stop_requested_{ false };
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> active_producer{ 0 };

    KmerTree<N>* tree_ptr_ = nullptr;
    LayerQueues<N>* layer_queues_ptr_ = nullptr;
    std::thread scheduler_thread_;
    std::vector<std::unique_ptr<std::thread>> worker_threads_ptr_;
    std::vector<MPMCRingQueue<uint32_t, WORKER_QUEUE_CAPACITY>> worker_queues_;

    std::array<std::atomic<int>, MAX_DEPTH> depth_worker_count{};
    std::vector<WorkerInfo> worker_infos;
    std::array<double, MAX_DEPTH> depth_ema_pressure_{};

    inline static thread_local SpinBackoff<> backoff;

public:
    explicit SchedulerThreadPool(uint32_t thread_count, uint32_t producer_count, KmerTree<N>* tree_ptr, LayerQueues<N>* layer_queues_ptr)
        : thread_count_(thread_count > 1 ? thread_count : 2), active_producer(producer_count), tree_ptr_(tree_ptr),
        layer_queues_ptr_(layer_queues_ptr), worker_queues_(thread_count - 1), worker_infos(thread_count - 1)
    {
        worker_threads_ptr_.reserve(thread_count - 1);
    }

    ~SchedulerThreadPool()
    {

    }

    void start()
    {
        running_.store(true, std::memory_order_release);
        uint32_t cur_depth = 0;
        for (uint32_t i = 0; i + 1 < thread_count_; i++)
        {
            worker_init(i, cur_depth);
            worker_threads_ptr_.push_back(std::make_unique<std::thread>(&SchedulerThreadPool::worker_thread_loop, this, i));

            depth_worker_count[cur_depth].fetch_add(1, std::memory_order_release);

            cur_depth++;
            cur_depth = cur_depth % MAX_DEPTH;
        }

        scheduler_thread_ = std::thread(&SchedulerThreadPool::scheduler_thread_loop, this);
    }

    void join()
    {
        stop_requested_.store(true, std::memory_order_release);
        if (scheduler_thread_.joinable())
        {
            scheduler_thread_.join();
        }
        for (auto& t : worker_threads_ptr_)
        {
            if (t->joinable())
            {
                t->join();
            }
        }
    }

    void mark_producer_done()
    {
        active_producer.fetch_sub(1, std::memory_order_release);
    }

private:

    bool all_producers_done() const
    {
        return active_producer.load(std::memory_order_acquire) == 0;
    }

    bool are_all_depth_queues_empty() const
    {
        return layer_queues_ptr_->size() == 0;
    }

    void drain_all(const uint32_t worker_id)
    {
        Task<N> task;
        uint32_t stable_empty_rounds = 0;
        uint32_t start_depth = worker_infos[worker_id].depth.load(std::memory_order_acquire);

        while (stable_empty_rounds < DRAIN_EMPTY_CONFIRM_ROUNDS)
        {
            for (uint32_t k = 0; k < MAX_DEPTH; k++)
            {
                uint32_t depth = (k + start_depth) % MAX_DEPTH;
                auto queue = layer_queues_ptr_->get_queue(static_cast<uint32_t>(depth));
                while (queue->try_dequeue(task))
                {
                    layer_queues_ptr_->decrease_size();
                    tree_ptr_->thread_add_kmer(task);
                }
                cpu_relax();
            }

            bool found_local_task_work = tree_ptr_->check_and_deal_with_local_stack();

            if (!found_local_task_work && are_all_depth_queues_empty())
            {
                stable_empty_rounds++;
            }
            else
            {
                stable_empty_rounds = 0;
            }
        }
    }

    uint32_t process_batch_at_depth(const uint32_t depth, const uint32_t max_process_tasks = MAX_PROCESS_TASKS)
    {
        Task<N> task;
        auto queue = layer_queues_ptr_->get_queue(depth);
        uint32_t processed = 0;
        while (processed < max_process_tasks && queue->try_dequeue(task))
        {
            tree_ptr_->thread_add_kmer(task);
            layer_queues_ptr_->decrease_size();
            processed++;
        }
        return processed;
    }

    // 所有 depth 切换全部由 try_swithch_depth 完成
    bool try_switch_depth(const uint32_t worker_id, const uint32_t depth)
    {
        uint32_t new_depth = INVALID_DEPTH;
        uint32_t processed = 0;
        if (worker_queues_[worker_id].try_dequeue(new_depth))
        {
            worker_infos[worker_id].depth.store(new_depth, std::memory_order_release);
            depth_worker_count[depth].fetch_sub(1, std::memory_order_release);
            depth_worker_count[new_depth].fetch_add(1, std::memory_order_release);
            backoff.reset();
            return true;
        }
        else
        {
            processed = process_batch_at_depth(depth, MAX_PROCESS_TASKS / 2);

            if (processed)
            {
                backoff.decay();
            }
            else
            {
                backoff.backoff();
            }
            return false;
        }

    }

    void work_at_depth(const uint32_t worker_id, const uint32_t depth)
    {
        uint32_t processed = process_batch_at_depth(depth);
    }

    void try_work(const uint32_t worker_id)
    {
        uint32_t work_depth = worker_infos[worker_id].depth.load(std::memory_order_acquire);

        work_at_depth(worker_id, work_depth);

        try_switch_depth(worker_id, work_depth);
    }

    void worker_init(const uint32_t worker_id, const uint32_t depth)
    {
        worker_infos[worker_id].depth.store(depth, std::memory_order_release);
    }

    void worker_thread_loop(const uint32_t worker_id)
    {
        uint32_t loop_round = 0;

        while (!stop_requested_.load(std::memory_order_acquire) || !all_producers_done())
        {
            try_work(worker_id);
            loop_round++;
            if (loop_round >= FORCE_DEAL_WITH_LOCAL_STACK_ROUND)
            {
                tree_ptr_->check_and_deal_with_local_stack();
                loop_round = 0;
            }
        }
        drain_all(worker_id);
    }

    void get_depth_worker_count_snapshot(std::array<uint32_t, MAX_DEPTH>& depth_worker_count_snapshot)
    {
        for (uint32_t depth = 0; depth < MAX_DEPTH; ++depth)
        {
            depth_worker_count_snapshot[depth] = depth_worker_count[depth].load(std::memory_order_acquire);
        }
    }

    void compute_ema_pressure(const std::array<uint32_t, MAX_DEPTH>& worker_snapshot)
    {
        for (uint32_t d = 0; d < MAX_DEPTH; ++d)
        {
            uint32_t qsize = layer_queues_ptr_->get_queue(d)->size();
            double raw = static_cast<double>(qsize) / (worker_snapshot[d] + 1);
            depth_ema_pressure_[d] = PRESSURE_EMA_ALPHA * raw + (1.0 - PRESSURE_EMA_ALPHA) * depth_ema_pressure_[d];
        }
    }

    void scheduler_thread_loop()
    {
        std::array<uint32_t, MAX_DEPTH> depth_worker_count_snapshot{};
        std::array<uint32_t, MAX_DEPTH> depth_dynamic_worker_lower_bound{};

        const uint32_t hard_worker_upper_bound = std::max<uint32_t>(1, (thread_count_ - 1) * 3 / 4);
        const uint32_t total_workers = thread_count_ - 1;

        depth_dynamic_worker_lower_bound.fill(1);

        while (!stop_requested_.load(std::memory_order_acquire) || !all_producers_done())
        {
            bool is_drain = all_producers_done();

            get_depth_worker_count_snapshot(depth_worker_count_snapshot);
            compute_ema_pressure(depth_worker_count_snapshot);

            // Minimum worker guarantee: ensure non-empty depths have at least 1 worker
            for (uint32_t d = 0; d < MAX_DEPTH; ++d)
            {
                uint32_t qsize = layer_queues_ptr_->get_queue(d)->size();
                uint32_t min_workers = depth_dynamic_worker_lower_bound[d];
                if (qsize > 0 && depth_worker_count_snapshot[d] < min_workers)
                {
                    uint32_t max_d = 0;
                    for (uint32_t dd = 1; dd < MAX_DEPTH; ++dd)
                    {
                        if (depth_worker_count_snapshot[dd] > depth_worker_count_snapshot[max_d])
                            max_d = dd;
                    }
                    if (depth_worker_count_snapshot[max_d] > min_workers)
                    {
                        for (uint32_t w = 0; w < total_workers; ++w)
                        {
                            uint32_t wd = worker_infos[w].depth.load(std::memory_order_acquire);
                            if (wd == max_d)
                            {
                                if (worker_queues_[w].try_enqueue(d))
                                    break;
                            }
                        }
                    }
                }
            }

            // Gradient-based migration with hysteresis

            uint32_t max_d = 0;
            uint32_t min_d = INVALID_DEPTH;
            double max_ema = depth_ema_pressure_[0];
            double min_ema = std::numeric_limits<double>::max();

            for (uint32_t d = 0; d < MAX_DEPTH; ++d)
            {
                if (depth_ema_pressure_[d] > max_ema)
                {
                    max_ema = depth_ema_pressure_[d];
                    max_d = d;
                }
                if (depth_worker_count_snapshot[d] > 0 && depth_ema_pressure_[d] < min_ema)
                {
                    min_ema = depth_ema_pressure_[d];
                    min_d = d;
                }
            }

            if (min_d != INVALID_DEPTH && max_ema > min_ema * HYSTERESIS_RATIO)
            {
                if (depth_worker_count_snapshot[max_d] < hard_worker_upper_bound)
                {
                    for (uint32_t w = 0; w < total_workers; ++w)
                    {
                        uint32_t wd = worker_infos[w].depth.load(std::memory_order_acquire);
                        if (wd == min_d)
                        {
                            if (worker_queues_[w].try_enqueue(max_d))
                                break;
                        }
                    }
                }
            }

            // Drain mode: aggressively move workers from empty depths to non-empty ones
            if (is_drain) [[unlikely]]
            {

                auto move_one_empty_to_work = [&]() {
                    for (uint32_t d = 0; d < MAX_DEPTH; ++d)
                    {
                        uint32_t qsize = layer_queues_ptr_->get_queue(d)->size();
                        if (qsize == 0 && depth_worker_count_snapshot[d] > 0)
                        {
                            for (uint32_t td = 0; td < MAX_DEPTH; ++td)
                            {
                                if (td == d) continue;

                                uint32_t tqsize = layer_queues_ptr_->get_queue(td)->size();
                                if (tqsize > 0 && depth_worker_count_snapshot[td] < hard_worker_upper_bound)
                                {
                                    for (uint32_t w = 0; w < total_workers; ++w)
                                    {
                                        uint32_t wd = worker_infos[w].depth.load(std::memory_order_acquire);
                                        if (wd == d)
                                        {
                                            if (worker_queues_[w].try_enqueue(td))
                                                return;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    };

                move_one_empty_to_work();
            }


            uint32_t interval = is_drain ? DRAIN_INTERVAL_US : SCHEDULE_INTERVAL_US;
            std::this_thread::sleep_for(std::chrono::microseconds(interval));
        }
    }
};

#endif // SCHEDULING_THREAD_POOL_HEADER
