#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/// Fixed-size thread pool.  Created once from `worker_threads` and reused
/// across all strips.  `run_batch` submits a set of tasks and blocks until
/// every one has returned.
class ThreadPool {
public:
    explicit ThreadPool(int num_threads) {
        for (int i = 0; i < num_threads; ++i)
            workers_.emplace_back(&ThreadPool::worker_loop, this);
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    int worker_count() const { return static_cast<int>(workers_.size()); }

    void run_batch(std::vector<std::function<void()>> tasks) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_ = static_cast<int>(tasks.size());
            for (auto& t : tasks) queue_.push(std::move(t));
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> lock(mu_);
        done_cv_.wait(lock, [this]{ return pending_ == 0; });
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this]{ return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            task();
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (--pending_ == 0) done_cv_.notify_one();
            }
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex                        mu_;
    std::condition_variable           cv_;
    std::condition_variable           done_cv_;
    int                               pending_ = 0;
    bool                              stop_    = false;
};
