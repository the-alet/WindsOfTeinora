#include "jobs.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <immintrin.h>
#include <mutex>
#include <thread>
#include <vector>

namespace taelia {

struct JobPool::Impl {
    std::vector<std::thread> workers;
    std::mutex m;
    std::condition_variable wake, done;
    std::atomic<uint64_t> gen{0};
    std::atomic<int> next{0}, busy{0};
    std::atomic<bool> quit{false};
    const std::function<void(int)>* fn = nullptr;
    int n = 0;

    void work() {
        const std::function<void(int)>* f = fn;
        const int count = n;
        for (int i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count;) (*f)(i);
    }

    void loop() {
        uint64_t seen = 0;
        for (;;) {
            // Spin a little first: the grids hand out work in quick bursts.
            for (int s = 0; s < 4000 && gen.load(std::memory_order_acquire) == seen && !quit.load(); ++s) _mm_pause();
            if (gen.load(std::memory_order_acquire) == seen) {
                std::unique_lock<std::mutex> lk(m);
                wake.wait(lk, [&] { return quit.load() || gen.load(std::memory_order_acquire) != seen; });
            }
            if (quit.load()) return;
            seen = gen.load(std::memory_order_acquire);
            work();
            if (busy.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lk(m);
                done.notify_one();
            }
        }
    }
};

JobPool::JobPool(int threads) : impl_(std::make_unique<Impl>()) {
    for (int i = 1; i < threads; ++i) impl_->workers.emplace_back([this] { impl_->loop(); });
}

JobPool::~JobPool() {
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->quit = true;
    }
    impl_->wake.notify_all();
    for (std::thread& t : impl_->workers) t.join();
}

int JobPool::threads() const { return int(impl_->workers.size()) + 1; }

int JobPool::defaultThreads() {
    const int hw = int(std::thread::hardware_concurrency());
    return std::clamp(hw - 2, 1, 12);
}

void JobPool::run(int n, const std::function<void(int)>& fn) {
    Impl& p = *impl_;
    if (p.workers.empty() || n <= 1) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(p.m);
        p.fn = &fn;
        p.n = n;
        p.next.store(0, std::memory_order_relaxed);
        p.busy.store(int(p.workers.size()), std::memory_order_relaxed);
        p.gen.fetch_add(1, std::memory_order_release);
    }
    p.wake.notify_all();
    p.work();
    for (int s = 0; s < 20000 && p.busy.load(std::memory_order_acquire) != 0; ++s) _mm_pause();
    if (p.busy.load(std::memory_order_acquire) != 0) {
        std::unique_lock<std::mutex> lk(p.m);
        p.done.wait(lk, [&] { return p.busy.load(std::memory_order_acquire) == 0; });
    }
}

}  // namespace taelia
