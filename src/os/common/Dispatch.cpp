#include "../OsInternal.h"

#include <deque>
#include <mutex>
#include <utility>

#if defined(ENP_HAS_THREADS)
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

void runAsync(Task work)
{
#if defined(ENP_HAS_THREADS)
    std::thread(std::move(work)).detach();
#else
    postToMain(std::move(work));
#endif
}

} // namespace enp::os::dispatch
