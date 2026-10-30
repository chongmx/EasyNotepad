// Minimal MVVM binding primitives: observable properties, events and commands.
#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace enp::viewmodel {

// RAII handle returned by subscribe(); unsubscribes when destroyed. The observed object
// must outlive the subscription (views subscribe to view-models that outlive them).
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
    Subscription(Subscription&& other) noexcept : cancel_(std::exchange(other.cancel_, nullptr)) {}
    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this != &other) {
            reset();
            cancel_ = std::exchange(other.cancel_, nullptr);
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() { reset(); }

    void reset()
    {
        if (cancel_)
            std::exchange(cancel_, nullptr)();
    }

private:
    std::function<void()> cancel_;
};

namespace detail {

template <typename... Args>
class ListenerList {
public:
    using Fn = std::function<void(const Args&...)>;

    [[nodiscard]] Subscription add(Fn fn)
    {
        const int id = nextId_++;
        listeners_->push_back({id, std::move(fn)});
        std::weak_ptr<std::vector<Entry>> weak = listeners_;
        return Subscription([weak, id]() {
            if (auto list = weak.lock())
                for (auto it = list->begin(); it != list->end(); ++it)
                    if (it->id == id) {
                        list->erase(it);
                        break;
                    }
        });
    }

    void notify(const Args&... args) const
    {
        // Copy so listeners may (un)subscribe while being notified.
        std::vector<Entry> snapshot = *listeners_;
        for (const Entry& e : snapshot)
            e.fn(args...);
    }

private:
    struct Entry {
        int id;
        Fn fn;
    };
    std::shared_ptr<std::vector<Entry>> listeners_ = std::make_shared<std::vector<Entry>>();
    int nextId_ = 0;
};

} // namespace detail

// A value with change notification. Views read with get(); the view-model (or a two-way
// binding in the view) writes with set(), which notifies subscribers only on change.
template <typename T>
class Property {
public:
    Property() = default;
    explicit Property(T initial) : value_(std::move(initial)) {}
    Property(const Property&) = delete;
    Property& operator=(const Property&) = delete;

    const T& get() const { return value_; }

    void set(T value)
    {
        if (value_ == value)
            return;
        value_ = std::move(value);
        listeners_.notify(value_);
    }

    [[nodiscard]] Subscription subscribe(std::function<void(const T&)> listener)
    {
        return listeners_.add(std::move(listener));
    }

private:
    T value_{};
    detail::ListenerList<T> listeners_;
};

// A fire-and-forget notification from the view-model to the view
// (e.g. "scroll to this anchor"), for requests that are not state.
template <typename... Args>
class Event {
public:
    [[nodiscard]] Subscription subscribe(std::function<void(const Args&...)> handler)
    {
        return listeners_.add(std::move(handler));
    }
    void emit(const Args&... args) const { listeners_.notify(args...); }

private:
    detail::ListenerList<Args...> listeners_;
};

// An action the view can invoke, with an availability predicate used to enable/disable UI.
class Command {
public:
    Command() = default;
    Command(std::function<void()> execute, std::function<bool()> canExecute = {})
        : execute_(std::move(execute)), canExecute_(std::move(canExecute))
    {
    }

    bool canExecute() const { return execute_ && (!canExecute_ || canExecute_()); }
    void execute() const
    {
        if (canExecute())
            execute_();
    }

private:
    std::function<void()> execute_;
    std::function<bool()> canExecute_;
};

} // namespace enp::viewmodel
