#include "uproxy/event_loop.h"

#if defined(__linux__)

#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <unordered_map>

namespace uproxy {

class EpollLoop final : public EventLoop {
    Uniquefd epfd_;
    struct Timer {
        uint64_t id{0};
        Uniquefd tfd;
        std::function<void()> cb;
    };
    std::unordered_map<int, uint64_t> timer_fd_to_id_;
    std::unordered_map<uint64_t, Timer> timers_;
    uint64_t next_timer_id_{1};
    static constexpr int MAX_EVENTS = 256;

  public:
    EpollLoop() : epfd_(::epoll_create1(EPOLL_CLOEXEC)) {}

    Result<void> add(int fd, Event events, void* user_data) override {
        epoll_event ev{};
        ev.events = to_epoll(events);
        ev.data.ptr = user_data;
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
            return Result<void>::err(Error::from_errno("epoll add"));
        }
        return Result<void>::ok();
    }

    Result<void> modify(int fd, Event events, void* user_data) override {
        epoll_event ev{};
        ev.events = to_epoll(events);
        ev.data.ptr = user_data;
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
            return Result<void>::err(Error::from_errno("epoll mod"));
        }
        return Result<void>::ok();
    }

    Result<void> remove(int fd) override {
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT) {
            return Result<void>::err(Error::from_errno("epoll del"));
        }
        return Result<void>::ok();
    }

    Result<void> poll(int timeout_ms, EventCallback cb) override {
        epoll_event events[MAX_EVENTS];
        const int n = ::epoll_wait(epfd_.get(), events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                return Result<void>::ok();
            }
            return Result<void>::err(Error::from_errno("epoll wait"));
        }
        for (int i = 0; i < n; ++i) {
            const int fd = static_cast<int>(reinterpret_cast<intptr_t>(events[i].data.ptr));
            auto timer_it = timer_fd_to_id_.find(fd);
            if (timer_it != timer_fd_to_id_.end()) {
                uint64_t expirations = 0;
                (void)::read(fd, &expirations, sizeof(expirations));
                auto it = timers_.find(timer_it->second);
                if (it != timers_.end()) {
                    auto fn = std::move(it->second.cb);
                    timer_fd_to_id_.erase(fd);
                    timers_.erase(it);
                    fn();
                }
                continue;
            }
            cb(FiredEvent{fd, from_epoll(events[i].events), events[i].data.ptr});
        }
        return Result<void>::ok();
    }

    Result<uint64_t> add_timer(int delay_ms, std::function<void()> cb) override {
        Uniquefd tfd(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
        if (!tfd.valid()) {
            return Result<uint64_t>::err(Error::from_errno("timerfd_create"));
        }
        itimerspec spec{};
        spec.it_value.tv_sec = delay_ms / 1000;
        spec.it_value.tv_nsec = (delay_ms % 1000) * 1000000;
        if (::timerfd_settime(tfd.get(), 0, &spec, nullptr) < 0) {
            return Result<uint64_t>::err(Error::from_errno("timerfd_settime"));
        }
        const uint64_t id = next_timer_id_++;
        const int fd = tfd.get();
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
            return Result<uint64_t>::err(Error::from_errno("epoll add timer"));
        }
        timer_fd_to_id_[fd] = id;
        timers_.emplace(id, Timer{id, std::move(tfd), std::move(cb)});
        return Result<uint64_t>::ok(id);
    }

    Result<void> cancel_timer(uint64_t id) override {
        auto it = timers_.find(id);
        if (it == timers_.end()) {
            return Result<void>::ok();
        }
        const int fd = it->second.tfd.get();
        (void)::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
        timer_fd_to_id_.erase(fd);
        timers_.erase(it);
        return Result<void>::ok();
    }

  private:
    uint32_t to_epoll(Event e) const noexcept {
        uint32_t out = EPOLLET;
        if (has(e, Event::Read)) {
            out |= EPOLLIN | EPOLLRDHUP;
        }
        if (has(e, Event::Write)) {
            out |= EPOLLOUT;
        }
        return out;
    }

    Event from_epoll(uint32_t ep) const noexcept {
        Event out = Event::None;
        if ((ep & EPOLLIN) != 0U) {
            out = out | Event::Read;
        }
        if ((ep & EPOLLOUT) != 0U) {
            out = out | Event::Write;
        }
        if ((ep & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
            out = out | Event::HangUp;
        }
        if ((ep & EPOLLERR) != 0U) {
            out = out | Event::Error;
        }
        return out;
    }
};

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<EpollLoop>();
}

} // namespace uproxy

#endif
