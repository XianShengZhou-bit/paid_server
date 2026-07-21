#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

#include "config.hpp"
#include "logger.hpp"

namespace payment_http {

class WorkerPool {
  public:
    // review
    static WorkerPool& instance() {
        static WorkerPool pool;
        return pool;
    }

    // review
    WorkerPool(const WorkerPool&) = delete;
    // review
    WorkerPool& operator=(const WorkerPool&) = delete;

    // review
    void init(int worker_count) {
        if (initialized_) {
            return;
        }
        if (worker_count < 4) {
            worker_count = 4;
        }
        worker_count_ = worker_count;
        workers_.reserve(static_cast<std::size_t>(worker_count));
        for (int i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
        initialized_ = true;
        LOG_INFO("HTTP 工作线程池初始化完成, worker_count={}", worker_count_);
    }

    // review
    int getWorkerCount() const {
        return worker_count_;
    }

    // review
    void initFromConfig(const payment_config::Config& config = payment_config::Config::instance()) {
        init(config.httpServer().worker_threads);
    }

    // review
    template <typename F> void submit(F&& task) {
        ensureInitialized();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            tasks_.emplace(std::forward<F>(task));
        }
        cv_.notify_one();
    }

  private:
    WorkerPool() = default;

    // review
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    // review
    void ensureInitialized() const {
        if (!initialized_) {
            throw std::runtime_error("payment_http::WorkerPool 未初始化");
        }
    }

    // review
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    int worker_count_{0};
    bool initialized_{false};
    bool stop_{false};
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace payment_http
