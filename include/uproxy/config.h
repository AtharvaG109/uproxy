#pragma once

#include "uproxy/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uproxy {

struct UpstreamConfig {
    std::string name;
    std::string addr;
    uint16_t port{0};
    uint32_t weight{1};
    bool healthy{true};
    enum class Proto { HTTP1, H2C } proto{Proto::HTTP1};
};

struct TLSConfig {
    std::string cert_file;
    std::string key_file;
    std::string min_version{"tls1.2"};
    std::vector<std::string> alpn{"h2", "http/1.1"};
    bool enabled{true};
};

struct PoolConfig {
    uint32_t max_conns_per_upstream{64};
    uint32_t connect_timeout_ms{5000};
    uint32_t idle_timeout_ms{60000};
    uint32_t max_keepalive{1000};
};

struct LogConfig {
    std::string level{"info"};
    std::string format{"text"};
    std::string file;
};

struct ProxyConfig {
    std::string listen_addr{"0.0.0.0"};
    uint16_t listen_port{8080};
    uint16_t tls_port{8443};
    uint32_t worker_threads{0};
    TLSConfig tls;
    PoolConfig pool;
    LogConfig log;
    std::vector<UpstreamConfig> upstreams;
};

[[nodiscard]] Result<ProxyConfig> config_from_file(std::string_view path);
[[nodiscard]] Result<ProxyConfig> config_apply_cli(ProxyConfig base, int argc, char** argv);
[[nodiscard]] Result<ProxyConfig> config_validate(ProxyConfig cfg);

} // namespace uproxy
