#include "../OsInternal.h"

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

void runAsync(Task work)
{
#if defined(ENP_HAS_THREADS)
    std::thread(std::move(work)).detach();
#else
    postToMain(std::move(work));
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
