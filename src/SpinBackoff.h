#ifndef SPIN_BACKOFF_HEADER
#define SPIN_BACKOFF_HEADER

#include "definition.h"

#include <chrono>


template <int MAX_BACKOFF = 256, int YIELD_THRESHOLD = 64, int SLEEP_THRESHOLD = 128>
class SpinBackoff {
public:
    void backoff()
    {
        if (count_ < YIELD_THRESHOLD)
        {
            // 动态自旋：执行 backoff_ 次 cpu_relax
            for (int i = 0; i < backoff_; i++)
            {
                cpu_relax();
            }
            // 指数增长，但限制上限
            backoff_ = std::min(backoff_ * 2, MAX_BACKOFF);
        }
        else if (count_ < SLEEP_THRESHOLD)
        {
            std::this_thread::yield();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++count_;
    }

    void decay()
    {
        count_ >>= 1;          // 阶段计数器指数衰减
        backoff_ = (backoff_ > 1) ? backoff_ / 2 : 1; // backoff 也指数衰减
    }

    void reset()
    {
        count_ = 0;
        backoff_ = 1;
    }

private:
    int count_ = 0;
    int backoff_ = 1;          // 初始只执行 1 次 pause
};

#endif