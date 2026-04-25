#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

namespace cppdbg {

// Serialises calls from the DAP thread onto the engine thread so every
// DbgEng / COM call happens on the same STA apartment.
class CommandQueue {
public:
    using Task = std::function<void()>;

    // Post a task and obtain a future for its result.
    template <class F>
    auto post(F&& fn) -> std::future<decltype(fn())> {
        using R = decltype(fn());
        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        pushTask([promise, fn = std::forward<F>(fn)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    fn();
                    promise->set_value();
                } else {
                    promise->set_value(fn());
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        return future;
    }

    // Block up to `timeoutMs` waiting for the next task. Returns false
    // on timeout. A zero timeout means "don't block".
    bool tryPop(Task& out, unsigned timeoutMs) {
        std::unique_lock<std::mutex> lock(mu_);
        if (timeoutMs == 0) {
            if (queue_.empty()) return false;
        } else {
            cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [&] { return !queue_.empty() || stopped_; });
            if (queue_.empty()) return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mu_);
        stopped_ = true;
        cv_.notify_all();
    }

private:
    void pushTask(Task task) {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push(std::move(task));
        cv_.notify_one();
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<Task> queue_;
    bool stopped_ = false;
};

}  // namespace cppdbg
