// The workload is a flat parallel_for over independent files, not a fork-join
// tree, so chunk distribution over one atomic counter beats work-stealing deques:
// it self-balances when per-item cost varies (40 MB RAW next to a 30 KB thumb) and
// touches one cache line per chunk.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace ghidraengine {

class ThreadPool {
public:
    // `threads` of 0 resolves to hardware_concurrency().
    explicit ThreadPool(unsigned threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> task);
    void wait_idle();

    [[nodiscard]] unsigned size() const noexcept { return static_cast<unsigned>(workers_.size()); }

    static unsigned default_thread_count() noexcept;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable idle_;
    std::size_t active_ = 0;
    bool stopping_ = false;
};

// Runs `body(index)` over [begin, end), with the calling thread participating.
// `token` is polled between chunks. Must not be called from inside a pool task:
// the caller would occupy a worker while waiting for tasks that need one.
template <typename Body>
void parallel_for(ThreadPool& pool, std::size_t begin, std::size_t end, Body&& body,
                  std::stop_token token = {}, std::size_t chunk_size = 0) {
    if (begin >= end) {
        return;
    }

    const std::size_t count = end - begin;
    const unsigned threads = pool.size() + 1; // workers plus the caller

    if (chunk_size == 0) {
        // Several chunks per thread so a slow one cannot idle a thread at the tail,
        // but large enough that the atomic fetch_add stays negligible.
        chunk_size = count / (static_cast<std::size_t>(threads) * 8);
        if (chunk_size == 0) {
            chunk_size = 1;
        }
    }

    std::atomic<std::size_t> cursor{begin};
    std::atomic<unsigned> remaining{threads};
    std::mutex done_mutex;
    std::condition_variable done_cv;

    auto run = [&] {
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
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            const std::lock_guard lock(done_mutex);
            done_cv.notify_one();
        }
    };

    for (unsigned i = 0; i + 1 < threads; ++i) {
        pool.submit(run);
    }
    run(); // the caller pulls its share

    std::unique_lock lock(done_mutex);
    done_cv.wait(lock, [&] { return remaining.load(std::memory_order_acquire) == 0; });
}

} // namespace ghidraengine
