#include "uproxy/load_balancer.h"

#include <algorithm>

namespace uproxy {

WeightedRoundRobin::WeightedRoundRobin(std::vector<UpstreamConfig> configs) {
    endpoints_.reserve(configs.size());
    current_weights_.reserve(configs.size());
    for (auto& cfg : configs) {
        auto endpoint = std::make_shared<UpstreamEndpoint>();
        endpoint->addr = std::move(cfg.addr);
        endpoint->port = cfg.port;
        endpoint->weight = std::max<uint32_t>(1, cfg.weight);
        endpoint->name = cfg.name.empty() ? endpoint->addr + ":" + std::to_string(endpoint->port)
                                          : std::move(cfg.name);
        endpoint->healthy.store(cfg.healthy);
        endpoints_.push_back(std::move(endpoint));
        current_weights_.push_back(0);
    }
}

std::shared_ptr<UpstreamEndpoint> WeightedRoundRobin::next() {
    std::lock_guard<std::mutex> lock(mu_);
    int64_t total = 0;
    int best = -1;
    for (size_t i = 0; i < endpoints_.size(); ++i) {
        if (!endpoints_[i]->healthy.load()) {
            continue;
        }
        current_weights_[i] += static_cast<int64_t>(endpoints_[i]->weight);
        total += static_cast<int64_t>(endpoints_[i]->weight);
        if (best < 0 || current_weights_[i] > current_weights_[static_cast<size_t>(best)]) {
            best = static_cast<int>(i);
        }
    }
    if (best < 0 || total == 0) {
        return nullptr;
    }
    current_weights_[static_cast<size_t>(best)] -= total;
    endpoints_[static_cast<size_t>(best)]->total_requests.fetch_add(1);
    return endpoints_[static_cast<size_t>(best)];
}

void WeightedRoundRobin::mark_unhealthy(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& ep : endpoints_) {
        if (ep->name == name) {
            ep->healthy.store(false);
        }
    }
}

void WeightedRoundRobin::mark_healthy(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& ep : endpoints_) {
        if (ep->name == name) {
            ep->healthy.store(true);
        }
    }
}

std::vector<WeightedRoundRobin::Stats> WeightedRoundRobin::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Stats> out;
    out.reserve(endpoints_.size());
    for (const auto& ep : endpoints_) {
        out.push_back({ep->name, ep->healthy.load(), ep->active_requests.load(),
                       ep->total_requests.load(), ep->failed_requests.load()});
    }
    return out;
}

const std::vector<std::shared_ptr<UpstreamEndpoint>>& WeightedRoundRobin::endpoints() const {
    return endpoints_;
}

} // namespace uproxy
