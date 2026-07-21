// review
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include "config.hpp"
#include "thread_pool.hpp"

/*
测试：payment_http::WorkerPool
1. 未 init 时 submit 抛异常
2. init 后 submit 能异步执行任务
3. init(2) 等过小 worker 数会被抬升到至少 4
4. initFromConfig / httpServer() 读取 .env
5. 重复 init 幂等
*/

// review
TEST(PaymentHttpWorkerPoolTest, LifecycleSubmitAndConfig) {
    auto& pool = payment_http::WorkerPool::instance();

    bool already_initialized = false;
    try {
        pool.submit([]() {});
        already_initialized = true;
    } catch (const std::runtime_error&) {
        already_initialized = false;
    }
    if (!already_initialized) {
        EXPECT_THROW(pool.submit([]() {}), std::runtime_error);
    }

    pool.init(2);
    pool.init(99);

    EXPECT_EQ(pool.getWorkerCount(), 4);

    std::promise<int> promise;
    std::future<int> future = promise.get_future();
    pool.submit([&promise]() { promise.set_value(42); });
    EXPECT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), 42);

    std::atomic<int> counter{0};
    for (int i = 0; i < 8; ++i) {
        pool.submit([&counter]() { counter.fetch_add(1); });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (counter.load() < 8 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(counter.load(), 8);

    const auto http_cfg = payment_config::Config::instance().httpServer();
    EXPECT_EQ(http_cfg.worker_threads, 20);
    EXPECT_EQ(http_cfg.listen_backlog, 128);
    EXPECT_EQ(http_cfg.max_concurrent_connections, 64);
    EXPECT_EQ(http_cfg.read_timeout_seconds, 10);

    pool.initFromConfig();
}
