#pragma once

#include "uproxy/fd.h"
#include "uproxy/result.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace uproxy {

enum class Event : uint32_t {
    None = 0,
    Read = 1U << 0U,
    Write = 1U << 1U,
    HangUp = 1U << 2U,
    Error = 1U << 3U,
};

inline Event operator|(Event a, Event b) {
    return static_cast<Event>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline Event operator&(Event a, Event b) {
    return static_cast<Event>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool has(Event set, Event flag) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0U;
}

struct FiredEvent {
    int fd{-1};
    Event events{Event::None};
    void* user_data{nullptr};
};

using EventCallback = std::function<void(const FiredEvent&)>;

class EventLoop {
  public:
    virtual ~EventLoop() = default;
    virtual Result<void> add(int fd, Event events, void* user_data) = 0;
    virtual Result<void> modify(int fd, Event events, void* user_data) = 0;
    virtual Result<void> remove(int fd) = 0;
    virtual Result<void> poll(int timeout_ms, EventCallback cb) = 0;
    virtual Result<uint64_t> add_timer(int delay_ms, std::function<void()> cb) = 0;
    virtual Result<void> cancel_timer(uint64_t id) = 0;
    static std::unique_ptr<EventLoop> create();
};

} // namespace uproxy
