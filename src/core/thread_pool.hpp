#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace ghidraengine {

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> task);

    [[nodiscard]] unsigned size() const noexcept { return static_cast<unsigned>(workers_.size()); }

    static unsigned default_thread_count() noexcept;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::size_t active_ = 0;
    bool stopping_ = false;
};

template <typename Body>
void parallel_for(ThreadPool& pool, std::size_t begin, std::size_t end, Body&& body,
                  std::stop_token token = {}, std::size_t chunk_size = 0) {
    if (begin >= end) {
        return;
    }

    const std::size_t count = end - begin;
    const unsigned threads = pool.size() + 1;

    if (chunk_size == 0) {
        chunk_size = count / (static_cast<std::size_t>(threads) * 8);
        if (chunk_size == 0) {
            chunk_size = 1;
        }
    }

    std::atomic<std::size_t> cursor{begin};
    std::atomic<unsigned> remaining{threads};
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::mutex failure_mutex;
    std::exception_ptr failure;

    auto run = [&] {
        // The decrement below must run on every path: a throw that skipped it would
        // leave the caller waiting on a count that never reaches zero.
        try {
            for (;;) {
                if (token.stop_possible() && token.stop_requested()) {
                    break;
                }
                const std::size_t start = cursor.fetch_add(chunk_size, std::memory_order_relaxed);
                if (start >= end) {
                    break;
                }
                const std::size_t stop = std::min(start + chunk_size, end);
                for (std::size_t i = start; i < stop; ++i) {
                    body(i);
                }
            }
        } catch (...) {
            const std::lock_guard lock(failure_mutex);
            if (!failure) {
                failure = std::current_exception();
            }
            cursor.store(end, std::memory_order_relaxed);
        }
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            const std::lock_guard lock(done_mutex);
            done_cv.notify_one();
        }
    };

    for (unsigned i = 0; i + 1 < threads; ++i) {
        pool.submit(run);
    }
    run();

    {
        std::unique_lock lock(done_mutex);
        done_cv.wait(lock, [&] { return remaining.load(std::memory_order_acquire) == 0; });
    }

    if (failure) {
        std::rethrow_exception(failure);
    }
}

}
