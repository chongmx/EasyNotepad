#include "../OsInternal.h"

#include "enp/os/Memory.h"

#include <deque>
#include <mutex>
#include <utility>

#if defined(ENP_HAS_THREADS)
#   include <condition_variable>
#   include <thread>
#endif

namespace enp::os::dispatch {

namespace {
std::mutex g_mutex;
std::deque<Task> g_queue;
std::function<void()> g_wake;
} // namespace

void postToMain(Task task)
{
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push_back(std::move(task));
        wake = g_wake;
    }
    if (wake)
        wake();
}

void setWakeCallback(std::function<void()> wake)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wake = std::move(wake);
}

void pump()
{
    // Swap out the queue so tasks may post new tasks without deadlocking;
    // those run on the next pump().
    std::deque<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tasks.swap(g_queue);
    }
    for (Task& task : tasks)
        if (task)
            task();
}

bool hasPending()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_queue.empty();
}

#if defined(ENP_HAS_THREADS)
namespace {

struct Pool {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Task> tasks;
    int idle = 0;     // threads waiting for a task
    int starting = 0; // threads started, not yet waiting
    int threads = 0;
};

// Never destroyed: its threads wait on it until the process ends.
Pool& pool()
{
    static Pool* p = new Pool;
    return *p;
}

void poolThread()
{
    Pool& p = pool();
    std::unique_lock<std::mutex> lock(p.mutex);
    --p.starting;
    for (;;) {
        ++p.idle;
        p.wake.wait(lock, [&p] { return !p.tasks.empty(); });
        --p.idle;
        Task task = std::move(p.tasks.front());
        p.tasks.pop_front();
        lock.unlock();
        task();
        task = nullptr; // what it captured goes here, on this thread, before the next one
        lock.lock();
    }
}

} // namespace
#endif

void runAsync(Task work)
{
#if defined(ENP_HAS_THREADS)
    // The pool's own room (the queue, its threads) is its own, whoever posts the first task.
    mem::Scope scope("dispatch");
    Pool& p = pool();
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.tasks.push_back(std::move(work));
        if (int(p.tasks.size()) > p.idle + p.starting) { // every kept thread is busy
            ++p.starting;
            ++p.threads;
            start = true;
        }
    }
    if (start)
        std::thread(poolThread).detach();
    else
        p.wake.notify_one();
#else
    postToMain(std::move(work));
#endif
}

int poolThreads()
{
#if defined(ENP_HAS_THREADS)
    std::lock_guard<std::mutex> lock(pool().mutex);
    return pool().threads;
#else
    return 0;
#endif
}

// ---- Group -----------------------------------------------------------------------------------------

struct Group::State {
    std::mutex mutex;
#if defined(ENP_HAS_THREADS)
    std::condition_variable done;
#endif
    int pending = 0;
};

Group::Group() : state_(std::make_shared<State>()) {}

Group::~Group()
{
    wait();
}

void Group::run(Task task)
{
#if defined(ENP_HAS_THREADS)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->pending;
    }
    runAsync([state = state_, task = std::move(task)]() mutable {
        task();
        task = nullptr;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (--state->pending == 0)
            state->done.notify_all();
    });
#else
    task(); // no threads: at once
#endif
}

void Group::wait()
{
#if defined(ENP_HAS_THREADS)
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->done.wait(lock, [this] { return state_->pending == 0; });
#endif
}

// ---- Worker ----------------------------------------------------------------------------------------

struct Worker::State {
#if defined(ENP_HAS_THREADS)
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Task> tasks;
    bool stop = false;
    std::thread thread;

    void run()
    {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stop || !tasks.empty(); });
                if (stop)
                    return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task(); // destroyed here, on this thread, before the next one
        }
    }
#endif
};

Worker::Worker() : state_(std::make_unique<State>()) {}

Worker::~Worker()
{
#if defined(ENP_HAS_THREADS)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop = true;
        std::deque<Task>().swap(state_->tasks); // queued work is dropped
    }
    state_->wake.notify_all();
    if (state_->thread.joinable())
        state_->thread.join();
#endif
}

void Worker::post(Task task)
{
#if defined(ENP_HAS_THREADS)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->tasks.push_back(std::move(task));
        if (!state_->thread.joinable())
            state_->thread = std::thread([state = state_.get()] { state->run(); });
    }
    state_->wake.notify_one();
#else
    postToMain(std::move(task));
#endif
}

bool Worker::started() const
{
#if defined(ENP_HAS_THREADS)
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->thread.joinable();
#else
    return false;
#endif
}

} // namespace enp::os::dispatch
