// Main-thread task queue and background work.
#pragma once

#include <functional>
#include <memory>

namespace enp::os::dispatch {

using Task = std::function<void()>;

// Thread-safe: queue a task for execution on the main thread.
void postToMain(Task task);
// Called (from any thread) after a task is posted, so a main loop that sleeps while idle can wake
// up: the GLFW app sets it to glfwPostEmptyEvent.
void setWakeCallback(std::function<void()> wake);

// Main thread only: run all queued tasks. Call once per frame.
void pump();
// Tasks are waiting for pump() (a minimized window's loop runs a frame for them).
bool hasPending();

// Run `work` on a background thread when the platform has threads; otherwise it is
// deferred to the next pump() on the main thread (single-threaded wasm).
// The threads are a pool kept for the life of the process: a task runs on an idle one, and a new
// thread starts only when every kept thread is busy. The process has as many threads as the most
// work it ever ran at once, never one more per task: a new thread costs memory in libraries that
// see every thread start (the NVIDIA OpenGL driver keeps about 70 KB per thread its DllMain saw,
// up to 37 MB), so a thread per task grew the app for as long as it ran.
void runAsync(Task work);
// Background threads started so far (all kept; tests and the developer readout).
int poolThreads();

// Tasks run on the pool, waited for together (the reading and writing ends of a process's pipes).
// Destroying the group waits too.
class Group {
public:
    Group();
    ~Group();
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    void run(Task task);
    void wait();

private:
    struct State;
    std::shared_ptr<State> state_; // shared with the running tasks, which signal it
};

// One background thread kept for its owner's lifetime, running its tasks one at a time in order.
// Work that recurs (an index rebuilt for every folder) runs on the same thread every time, so it
// uses the same stack and the same malloc arena: what that arena keeps after the work is the same
// memory each time, instead of whichever arena a new thread happens to get (docs/MEMORY.md rule
// 10). The thread starts with the first task. Destroying the worker waits for the task running
// then (it should be cancelled first) and drops those still queued. Without threads (wasm) tasks
// run on the main thread at the next pump().
class Worker {
public:
    Worker();
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    void post(Task task);
    bool started() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace enp::os::dispatch
