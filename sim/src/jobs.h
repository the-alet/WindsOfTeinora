// A fixed pool of threads for the simulation's grids. Each index runs exactly once on some thread; the work for an
// index must not depend on which thread runs it or in what order, so results never depend on the thread count.
// No float atomics anywhere: reductions are done by the caller afterwards, in index order.
#pragma once
#include <functional>
#include <memory>

namespace taelia {

class JobPool {
public:
    // `threads` counts the caller too; 1 runs everything inline.
    explicit JobPool(int threads);
    ~JobPool();
    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    int threads() const;
    // Runs fn(i) for every i in [0, n) and returns when all are done.
    void run(int n, const std::function<void(int)>& fn);

    static int defaultThreads();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace taelia
