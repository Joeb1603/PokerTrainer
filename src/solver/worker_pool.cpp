#include "solver/worker_pool.h"

namespace poker {

WorkerPool::WorkerPool(int threads) {
    const int background = threads > 1 ? threads - 1 : 0;
    threads_.reserve(static_cast<std::size_t>(background));
    for (int w = 1; w <= background; ++w) {
        threads_.emplace_back([this, w](std::stop_token stop) { loop(stop, w); });
    }
}

WorkerPool::~WorkerPool() {
    // Stop first, so no thread is left waiting on a condition variable whose
    // members are destroyed after threads_ (the jthreads would stop anyway).
    for (std::jthread& thread : threads_) thread.request_stop();
    threads_.clear();
}

void WorkerPool::dispatch(int tasks, TaskFn fn, const void* context) {
    if (tasks <= 0) return;
    {
        const std::lock_guard lock(mutex_);
        fn_ = fn;
        context_ = context;
        tasks_ = tasks;
        next_.store(0, std::memory_order_relaxed);
        error_ = nullptr;
        error_task_ = tasks;
        busy_ = static_cast<int>(threads_.size());
        ++generation_;
    }
    wake_.notify_all();
    drain(0);
    std::exception_ptr error;
    {
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return busy_ == 0; });
        error = error_;
        error_ = nullptr;
        fn_ = nullptr;
        context_ = nullptr;
    }
    if (error) std::rethrow_exception(error);
}

void WorkerPool::drain(int worker) {
    for (;;) {
        const int task = next_.fetch_add(1, std::memory_order_relaxed);
        if (task >= tasks_) return;
        try {
            fn_(context_, task, worker);
        } catch (...) {
            const std::lock_guard lock(mutex_);
            if (!error_ || task < error_task_) {
                error_ = std::current_exception();
                error_task_ = task;
            }
            next_.store(tasks_, std::memory_order_relaxed);  // Hand out nothing more
        }
    }
}

void WorkerPool::loop(std::stop_token stop, int worker) {
    std::uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this, seen] { return generation_ != seen; })) return;
            seen = generation_;
        }
        drain(worker);
        {
            const std::lock_guard lock(mutex_);
            if (--busy_ == 0) done_.notify_all();
        }
    }
}

}  // namespace poker
