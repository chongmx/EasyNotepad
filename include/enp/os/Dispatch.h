// Main-thread task queue and background work.
#pragma once

#include <functional>

namespace enp::os::dispatch {

using Task = std::function<void()>;

// Thread-safe: queue a task for execution on the main thread.
void postToMain(Task task);
// Called (from any thread) after a task is posted, so a main loop that sleeps while idle can wake
// up: the GLFW app sets it to glfwPostEmptyEvent.
void setWakeCallback(std::function<void()> wake);

// Main thread only: run all queued tasks. Call once per frame.
void pump();

// Run `work` on a background thread when the platform has threads; otherwise it is
// deferred to the next pump() on the main thread (single-threaded wasm).
void runAsync(Task work);

} // namespace enp::os::dispatch
