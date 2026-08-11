#include "core/thread_pool.hpp"

#include <algorithm>
#include <utility>

namespace ghidraengine {

unsigned ThreadPool::default_thread_count() noexcept {
    const unsigned detected = std::thread::hardware_concurrency();
    return detected == 0 ? 4u : detected;
}

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) {
        threads = default_thread_count();
    }
    threads = std::max(1u, threads);

    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_ready_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        const std::lock_guard lock(mutex_);
        tasks_.push(std::move(task));
    }
    work_ready_.notify_one();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            work_ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
            ++active_;
        }

        task();

        {
            const std::lock_guard lock(mutex_);
            --active_;
        }
    }
}

}
