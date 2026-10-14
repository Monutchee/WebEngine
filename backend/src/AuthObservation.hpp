#pragma once
#include "webengine/AuthObserver.hpp"
#include <mutex>
#include <utility>

namespace webengine {
class AuthObservation {
public:
    void set(AuthObserver observer) {
        std::lock_guard lock(mutex_);
        observer_ = std::move(observer);
    }
    // Observability cannot change the outcome of an authentication operation.
    void notify(const AuthEvent& event) const noexcept {
        try {
            AuthObserver observer;
            { std::lock_guard lock(mutex_); observer = observer_; }
            if (observer) observer(event);
        } catch (...) {}
    }
private:
    mutable std::mutex mutex_;
    AuthObserver observer_;
};
} // namespace webengine
