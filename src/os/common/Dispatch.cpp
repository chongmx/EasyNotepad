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
} // namespace

void postToMain(Task task)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.push_back(std::move(task));
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
