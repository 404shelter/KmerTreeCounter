#ifndef CLASSIFIER_THREAD_POOL_HEADER
#define CLASSIFIER_THREAD_POOL_HEADER

#include "definition.h"
#include "MPMCRingQueue.h"
#include "NewKmerTree.h"
#include "ConcurrentMemoryPool.h"
#include "RingMemoryPool.h"
#include "FastqClassifier.h"
#include "SchedulerThreadPool.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include <utility>
#include <chrono>

template <uint32_t N>
class ClassifierThreadPool
{

    int k_len;
    const uint32_t classifier_count;
    RingMemoryPool<PARSER_CLASSIFIER_RING_MEMORY_POOL_CAPACITY> *parser_classifier_ring_pool;
    SchedulerThreadPool<N> *task_thread_pool;
    KmerTree<N> *tree;
    std::vector<std::unique_ptr<std::thread>> threads_ptr;

public:
    explicit ClassifierThreadPool(const int in_k, KmerTree<N> *tree_ptr,
                                  RingMemoryPool<PARSER_CLASSIFIER_RING_MEMORY_POOL_CAPACITY> *in_parser_classifier_ring_pool_ptr,
                                  SchedulerThreadPool<N> *task_thread_pool_ptr,
                                  uint32_t in_classifier_count)
        : k_len(in_k),
          tree(tree_ptr),
          parser_classifier_ring_pool(in_parser_classifier_ring_pool_ptr),
          classifier_count(in_classifier_count),
          task_thread_pool(task_thread_pool_ptr)
    {
        threads_ptr.reserve(classifier_count);
    }

    void start()
    {
        for (uint32_t i = 0; i < classifier_count; ++i)
        {
            threads_ptr.push_back(std::make_unique<std::thread>([&]
                                                                {
				FastqClassifier<N> parser(k_len, parser_classifier_ring_pool, tree);
				parser.classify_and_push();
                task_thread_pool->mark_producer_done(); }));
        }
    }

    void join()
    {
        for (auto &t : threads_ptr)
        {
            if (t->joinable())
            {
                t->join();
            }
        }
    }

};

#endif