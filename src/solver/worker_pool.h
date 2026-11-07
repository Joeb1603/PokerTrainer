#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace poker {

// A fixed set of std::jthread workers, created once and reused for every parallel
// region: run(tasks, f) calls f(task, worker) for task = 0 .. tasks - 1, on the
// calling thread (worker 0) and the pool's threads (workers 1 .. size() - 1), and
// returns once every call has finished. f is called through a const reference.
// Tasks are handed out in index order to
// whichever worker is free, so a task must not depend on which worker runs it
// beyond using that worker's own scratch: the caller keeps results bit-identical
// for any thread count by having each task write only its own storage and
// combining them afterwards in task order.
//
// An exception thrown by a task stops the hand-out of further tasks; run() waits for
// the tasks already running and rethrows the exception of the lowest-numbered task
// that threw on the calling thread. The pool stays usable afterwards.
//
// One region at a time: run() must not be called concurrently or from inside a task.
// The destructor stops and joins the threads.
class WorkerPool {
public:
    // `threads` workers including the caller: threads - 1 background threads (none for
    // 0 or 1).
    explicit WorkerPool(int threads);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int size() const { return static_cast<int>(threads_.size()) + 1; }  // Workers, the caller included

    template <typename F>
    void run(int tasks, F&& f) {
        using Fn = std::remove_reference_t<F>;
        dispatch(tasks, [](const void* context, int task, int worker) { (*static_cast<const Fn*>(context))(task, worker); },
                 static_cast<const void*>(&f));
    }

private:
    using TaskFn = void (*)(const void*, int, int);

    void dispatch(int tasks, TaskFn fn, const void* context);
    void drain(int worker);
    void loop(std::stop_token stop, int worker);

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable_any done_;
    std::uint64_t generation_ = 0;  // Bumped by each region
    int busy_ = 0;                  // Background workers still in the current region
    std::atomic<int> next_{0};      // The next task to hand out
    int tasks_ = 0;
    TaskFn fn_ = nullptr;
    const void* context_ = nullptr;
    std::exception_ptr error_;
    int error_task_ = 0;
    std::vector<std::jthread> threads_;  // Last: destroyed (stopped and joined) first
};

}  // namespace poker
