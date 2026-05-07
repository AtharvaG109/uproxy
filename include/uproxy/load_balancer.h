#pragma once

#include "uproxy/config.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace uproxy {

struct UpstreamEndpoint {
    std::string addr;
    uint16_t port{0};
    uint32_t weight{1};
    std::string name;
    std::atomic<bool> healthy{true};
    std::atomic<uint64_t> active_requests{0};
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> failed_requests{0};
};

class WeightedRoundRobin {
    std::vector<std::shared_ptr<UpstreamEndpoint>> endpoints_;
    std::vector<int64_t> current_weights_;
    mutable std::mutex mu_;

  public:
    explicit WeightedRoundRobin(std::vector<UpstreamConfig> configs);
    [[nodiscard]] std::shared_ptr<UpstreamEndpoint> next();
    void mark_unhealthy(const std::string& name);
    void mark_healthy(const std::string& name);

    struct Stats {
        std::string name;
        bool healthy{false};
        uint64_t active{0};
        uint64_t total{0};
        uint64_t failed{0};
    };
    [[nodiscard]] std::vector<Stats> stats() const;
    [[nodiscard]] const std::vector<std::shared_ptr<UpstreamEndpoint>>& endpoints() const;
};

} // namespace uproxy
