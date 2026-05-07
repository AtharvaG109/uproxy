#pragma once

#include "uproxy/event_loop.h"
#include "uproxy/http2.h"
#include "uproxy/load_balancer.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace uproxy {

struct PooledConn {
    Uniquefd fd;
    std::unique_ptr<H2Conn> h2;
    bool in_use{false};
    uint64_t request_count{0};
    uint64_t created_at_ms{0};
    uint64_t last_used_at_ms{0};
};

class UpstreamPool {
    std::shared_ptr<UpstreamEndpoint> endpoint_;
    PoolConfig cfg_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<PooledConn>> idle_;
    uint32_t total_{0};

  public:
    UpstreamPool(std::shared_ptr<UpstreamEndpoint> ep, PoolConfig cfg);
    [[nodiscard]] Result<std::unique_ptr<PooledConn>> acquire(EventLoop& loop);
    void release(std::unique_ptr<PooledConn> conn);
    void evict_idle(uint64_t now_ms);
    void drain();
    [[nodiscard]] size_t idle_count() const;
    [[nodiscard]] size_t total_count() const;

  private:
    [[nodiscard]] Result<std::unique_ptr<PooledConn>> new_connection(EventLoop& loop);
};

class PoolManager {
    std::unordered_map<std::string, std::unique_ptr<UpstreamPool>> pools_;
    PoolConfig cfg_;

  public:
    explicit PoolManager(PoolConfig cfg);
    UpstreamPool& get(std::shared_ptr<UpstreamEndpoint> ep);
    void tick(uint64_t now_ms);
};

} // namespace uproxy
