// Unit test for ThreadPool.
//
// Behaviors verified:
//   1. All N submitted tasks run exactly once.
//   2. Per-task results are written to the correct output slot.
//   3. Pool is reusable: a second run_batch after the first completes correctly.
//   4. A 2-thread pool can run two concurrently-blocking tasks simultaneously
//      (tests that the pool truly has ≥2 live worker threads).

#include "ThreadPool.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

// ── Cycle 1 & 2: all tasks run, per-task results correct ─────────────────────

static void test_all_tasks_run_and_results_correct() {
    ThreadPool pool(4);

    constexpr int N = 8;
    std::vector<int> results(N, -1);

    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < N; ++i) {
        tasks.push_back([i, &results]() { results[i] = i * i; });
    }
    pool.run_batch(std::move(tasks));

    for (int i = 0; i < N; ++i) {
        assert(results[i] == i * i);
    }
    std::puts("PASS: all 8 tasks ran and produced correct results");
}

// ── Cycle 3: pool reusable across successive batches ─────────────────────────

static void test_pool_reusable_across_batches() {
    ThreadPool pool(2);

    std::atomic<int> counter{0};

    auto make_tasks = [&](int n) {
        std::vector<std::function<void()>> tasks;
        for (int i = 0; i < n; ++i)
            tasks.push_back([&]() { counter.fetch_add(1); });
        return tasks;
    };

    pool.run_batch(make_tasks(5));
    assert(counter.load() == 5);

    pool.run_batch(make_tasks(3));
    assert(counter.load() == 8);

    std::puts("PASS: pool reusable — second batch ran after first completed");
}

// ── Cycle 4: ≥2 live workers (concurrent-blocking test) ──────────────────────
//
// Two tasks each signal "I am running" then wait for the other to signal.
// With only 1 worker, the second task never starts while the first waits →
// the first task hangs forever.  With ≥2 workers both tasks proceed.
//
// The wait uses a real condition variable (no busy-spin), so the test is not
// a timing artifact — it is structurally impossible to pass with 1 worker.

static void test_two_workers_run_concurrently() {
    ThreadPool pool(2);

    std::mutex          mu;
    std::condition_variable cv;
    int                 ready = 0;  // how many tasks have checked in

    auto task = [&]() {
        std::unique_lock<std::mutex> lock(mu);
        ++ready;
        cv.notify_all();
        // Wait until BOTH tasks have reached this point.
        cv.wait(lock, [&]{ return ready >= 2; });
    };

    pool.run_batch({task, task});
    // If we get here without deadlock, both tasks ran simultaneously.
    std::puts("PASS: 2-worker pool ran two mutually-blocking tasks simultaneously");
}

int main() {
    test_all_tasks_run_and_results_correct();
    test_pool_reusable_across_batches();
    test_two_workers_run_concurrently();
    std::puts("ALL PASS");
    return 0;
}
