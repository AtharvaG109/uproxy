#include "uproxy/upstream_pool.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

namespace uproxy {

namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

UpstreamPool::UpstreamPool(std::shared_ptr<UpstreamEndpoint> ep, PoolConfig cfg)
    : endpoint_(std::move(ep)), cfg_(cfg) {}

Result<std::unique_ptr<PooledConn>> UpstreamPool::acquire(EventLoop& loop) {
    {
        std::unique_lock<std::mutex> lock(mu_);
        if (!idle_.empty()) {
            auto conn = std::move(idle_.front());
            idle_.pop_front();
            conn->in_use = true;
            return Result<std::unique_ptr<PooledConn>>::ok(std::move(conn));
        }
        if (total_ >= cfg_.max_conns_per_upstream) {
            const bool ok = cv_.wait_for(lock, std::chrono::milliseconds(cfg_.connect_timeout_ms),
                                         [&] { return !idle_.empty(); });
            if (!ok) {
                return Result<std::unique_ptr<PooledConn>>::err(
                    Error::from_code(ErrCode::Timeout, "upstream pool exhausted"));
            }
            auto conn = std::move(idle_.front());
            idle_.pop_front();
            conn->in_use = true;
            return Result<std::unique_ptr<PooledConn>>::ok(std::move(conn));
        }
        ++total_;
    }
    auto created = new_connection(loop);
    if (created.is_err()) {
        std::lock_guard<std::mutex> lock(mu_);
        --total_;
        endpoint_->failed_requests.fetch_add(1);
        return created;
    }
    return created;
}

void UpstreamPool::release(std::unique_ptr<PooledConn> conn) {
    if (conn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    conn->in_use = false;
    conn->last_used_at_ms = now_ms();
    if (!conn->fd.valid() || conn->request_count >= cfg_.max_keepalive) {
        --total_;
        cv_.notify_one();
        return;
    }
    idle_.push_back(std::move(conn));
    cv_.notify_one();
}

void UpstreamPool::evict_idle(uint64_t ts_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = idle_.begin();
    while (it != idle_.end()) {
        if (ts_ms >= (*it)->last_used_at_ms &&
            ts_ms - (*it)->last_used_at_ms > cfg_.idle_timeout_ms) {
            it = idle_.erase(it);
            --total_;
        } else {
            ++it;
        }
    }
}

void UpstreamPool::drain() {
    std::lock_guard<std::mutex> lock(mu_);
    idle_.clear();
    total_ = 0;
    cv_.notify_all();
}

size_t UpstreamPool::idle_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return idle_.size();
}

size_t UpstreamPool::total_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_;
}

Result<std::unique_ptr<PooledConn>> UpstreamPool::new_connection(EventLoop& loop) {
    (void)loop;
    Uniquefd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid()) {
        return Result<std::unique_ptr<PooledConn>>::err(Error::from_errno("socket upstream"));
    }
    auto nb = fd.set_nonblocking();
    if (nb.is_err()) {
        return Result<std::unique_ptr<PooledConn>>::err(nb.error());
    }
    (void)fd.set_nodelay();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint_->port);
    if (::inet_pton(AF_INET, endpoint_->addr.c_str(), &addr.sin_addr) != 1) {
        return Result<std::unique_ptr<PooledConn>>::err(
            Error::from_code(ErrCode::ConfigInvalid, "upstream address must be IPv4"));
    }
    const int rc = ::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        return Result<std::unique_ptr<PooledConn>>::err(Error::from_errno("connect upstream"));
    }
    auto conn = std::make_unique<PooledConn>();
    conn->fd = std::move(fd);
    conn->in_use = true;
    conn->created_at_ms = now_ms();
    conn->last_used_at_ms = conn->created_at_ms;
    return Result<std::unique_ptr<PooledConn>>::ok(std::move(conn));
}

PoolManager::PoolManager(PoolConfig cfg) : cfg_(cfg) {}

UpstreamPool& PoolManager::get(std::shared_ptr<UpstreamEndpoint> ep) {
    const std::string key = ep->name;
    auto it = pools_.find(key);
    if (it == pools_.end()) {
        it = pools_.emplace(key, std::make_unique<UpstreamPool>(std::move(ep), cfg_)).first;
    }
    return *it->second;
}

void PoolManager::tick(uint64_t ts_ms) {
    for (auto& [_, pool] : pools_) {
        pool->evict_idle(ts_ms);
    }
}

} // namespace uproxy
