#include "uproxy/event_loop.h"

#if defined(__APPLE__) || defined(__FreeBSD__)

#include <cerrno>
#include <chrono>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>

namespace uproxy {

class KqueueLoop final : public EventLoop {
    Uniquefd kqfd_;
    struct Timer {
        uint64_t id{0};
        std::function<void()> cb;
    };
    std::unordered_map<uint64_t, Timer> timers_;
    uint64_t next_timer_id_{1};
    static constexpr int MAX_EVENTS = 256;

  public:
    KqueueLoop() : kqfd_(::kqueue()) {}

    Result<void> add(int fd, Event events, void* user_data) override {
        return apply(fd, events, user_data, EV_ADD | EV_ENABLE | EV_CLEAR);
    }

    Result<void> modify(int fd, Event events, void* user_data) override {
        return apply(fd, events, user_data, EV_ADD | EV_ENABLE | EV_CLEAR);
    }

    Result<void> remove(int fd) override {
        struct kevent changes[2];
        EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        if (::kevent(kqfd_.get(), changes, 2, nullptr, 0, nullptr) < 0 && errno != ENOENT) {
            return Result<void>::err(Error::from_errno("kevent delete"));
        }
        return Result<void>::ok();
    }

    Result<void> poll(int timeout_ms, EventCallback cb) override {
        struct kevent events[MAX_EVENTS];
        timespec timeout{};
        timespec* timeout_ptr = nullptr;
        if (timeout_ms >= 0) {
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;
            timeout_ptr = &timeout;
        }
        const int n = ::kevent(kqfd_.get(), nullptr, 0, events, MAX_EVENTS, timeout_ptr);
        if (n < 0) {
            if (errno == EINTR) {
                return Result<void>::ok();
            }
            return Result<void>::err(Error::from_errno("kevent poll"));
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].filter == EVFILT_TIMER) {
                const uint64_t id = static_cast<uint64_t>(events[i].ident);
                auto it = timers_.find(id);
                if (it != timers_.end()) {
                    auto fn = std::move(it->second.cb);
                    timers_.erase(it);
                    fn();
                }
                continue;
            }
            Event e = Event::None;
            if (events[i].filter == EVFILT_READ) {
                e = e | Event::Read;
            }
            if (events[i].filter == EVFILT_WRITE) {
                e = e | Event::Write;
            }
            if ((events[i].flags & EV_EOF) != 0) {
                e = e | Event::HangUp;
            }
            if ((events[i].flags & EV_ERROR) != 0) {
                e = e | Event::Error;
            }
            cb(FiredEvent{static_cast<int>(events[i].ident), e, events[i].udata});
        }
        return Result<void>::ok();
    }

    Result<uint64_t> add_timer(int delay_ms, std::function<void()> cb) override {
        const uint64_t id = next_timer_id_++;
        struct kevent ev;
        EV_SET(&ev, static_cast<uintptr_t>(id), EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0,
               delay_ms, nullptr);
        if (::kevent(kqfd_.get(), &ev, 1, nullptr, 0, nullptr) < 0) {
            return Result<uint64_t>::err(Error::from_errno("kevent timer add"));
        }
        timers_.emplace(id, Timer{id, std::move(cb)});
        return Result<uint64_t>::ok(id);
    }

    Result<void> cancel_timer(uint64_t id) override {
        struct kevent ev;
        EV_SET(&ev, static_cast<uintptr_t>(id), EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
        if (::kevent(kqfd_.get(), &ev, 1, nullptr, 0, nullptr) < 0 && errno != ENOENT) {
            return Result<void>::err(Error::from_errno("kevent timer delete"));
        }
        timers_.erase(id);
        return Result<void>::ok();
    }

    // Track which filters are registered per fd
    std::unordered_map<int, uint32_t> registered_;

  private:
    Result<void> apply(int fd, Event events, void* user_data, uint16_t flags) {
        uint32_t cur = registered_[fd];

        if (has(events, Event::Read)) {
            struct kevent ev;
            EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, flags, 0, 0, user_data);
            if (::kevent(kqfd_.get(), &ev, 1, nullptr, 0, nullptr) < 0) {
                return Result<void>::err(Error::from_errno("kevent add read"));
            }
            cur |= 1;
        } else if (cur & 1) {
            struct kevent ev;
            EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            (void)::kevent(kqfd_.get(), &ev, 1, nullptr, 0, nullptr);
            cur &= ~1u;
        }

        if (has(events, Event::Write)) {
            struct kevent ev;
            EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_WRITE, flags, 0, 0, user_data);
            if (::kevent(kqfd_.get(), &ev, 1, nullptr, 0, nullptr) < 0) {
                return Result<void>::err(Error::from_errno("kevent add write"));
            }
            cur |= 2;
        } else if (cur & 2) {
            struct kevent ev;
            EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            (void)::kevent(kqfd_.get(), &ev, 1, nullptr, 0, nullptr);
            cur &= ~2u;
        }

        registered_[fd] = cur;
        return Result<void>::ok();
    }
};

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<KqueueLoop>();
}

} // namespace uproxy

#endif
