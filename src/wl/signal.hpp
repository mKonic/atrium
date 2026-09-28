#pragma once
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace atrium::wl {

// A C++ signal for the protocol layer's own objects (a surface's commit, a
// seat's focus). connect() returns a Connection that disconnects when it is
// destroyed, so a listener that goes can never be called; a slot may destroy
// its own connection, or the signal's owner, while being called.
template <class... Args>
class Signal {
    struct Slot {
        std::function<void(Args...)> fn;
        bool live = true;
    };

public:
    class Connection {
    public:
        Connection() = default;
        Connection(Connection&&) noexcept = default;
        Connection& operator=(Connection&& other) noexcept {
            disconnect();
            slot_ = std::move(other.slot_);
            return *this;
        }
        ~Connection() { disconnect(); }

        void disconnect() {
            if (auto s = slot_.lock())
                s->live = false;
            slot_.reset();
        }
        bool connected() const {
            auto s = slot_.lock();
            return s && s->live;
        }

    private:
        friend class Signal;
        explicit Connection(std::weak_ptr<Slot> slot) : slot_(std::move(slot)) {}
        std::weak_ptr<Slot> slot_;
    };

    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    [[nodiscard]] Connection connect(std::function<void(Args...)> fn) {
        auto slot = std::make_shared<Slot>(Slot{std::move(fn)});
        slots_->push_back(slot);
        return Connection(slot);
    }

    // Slots connected during the emit are not called by it.
    void emit(Args... args) {
        auto slots = slots_;  // keeps the list alive if the owner dies mid-emit
        const size_t n = slots->size();
        for (size_t i = 0; i < n && i < slots->size(); ++i) {
            std::shared_ptr<Slot> s = (*slots)[i];
            if (s->live)
                s->fn(args...);
        }
        std::erase_if(*slots, [](const std::shared_ptr<Slot>& s) { return !s->live; });
    }

    bool empty() const {
        return std::ranges::none_of(*slots_, [](const auto& s) { return s->live; });
    }

private:
    std::shared_ptr<std::vector<std::shared_ptr<Slot>>> slots_ =
        std::make_shared<std::vector<std::shared_ptr<Slot>>>();
};

} // namespace atrium::wl
