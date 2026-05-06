#include "uproxy/config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace uproxy {

namespace {

std::string trim(std::string_view input) {
    size_t begin = 0;
    size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return std::string(input.substr(begin, end - begin));
}

bool parse_bool(std::string_view value) {
    return trim(value) == "true";
}

std::string parse_string(std::string_view value) {
    auto v = trim(value);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

Result<uint32_t> parse_u32(std::string_view value, std::string_view field) {
    const std::string text = trim(value);
    uint32_t out = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc() || ptr != last) {
        return Result<uint32_t>::err(Error::from_code(ErrCode::ConfigInvalid, field));
    }
    return Result<uint32_t>::ok(out);
}

Result<uint16_t> parse_port(std::string_view value, std::string_view field) {
    auto parsed = parse_u32(value, field);
    if (parsed.is_err()) {
        return Result<uint16_t>::err(parsed.error());
    }
    if (parsed.value() == 0 || parsed.value() > 65535) {
        return Result<uint16_t>::err(Error::from_code(ErrCode::ConfigInvalid, field));
    }
    return Result<uint16_t>::ok(static_cast<uint16_t>(parsed.value()));
}

bool file_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

Result<UpstreamConfig> parse_upstream_cli(std::string_view spec) {
    UpstreamConfig cfg;
    const std::string s(spec);
    const size_t colon = s.find(':');
    if (colon == std::string::npos) {
        return Result<UpstreamConfig>::err(
            Error::from_code(ErrCode::ConfigInvalid, "upstream requires addr:port"));
    }
    cfg.addr = s.substr(0, colon);
    size_t next = s.find(':', colon + 1);
    const std::string port =
        s.substr(colon + 1, next == std::string::npos ? std::string::npos : next - colon - 1);
    auto parsed_port = parse_port(port, "invalid upstream port");
    if (parsed_port.is_err()) {
        return Result<UpstreamConfig>::err(parsed_port.error());
    }
    cfg.port = parsed_port.value();
    cfg.name = cfg.addr + ":" + std::to_string(cfg.port);
    if (next != std::string::npos) {
        const std::string suffix = s.substr(next + 1);
        constexpr std::string_view prefix = "weight=";
        if (suffix.rfind(prefix, 0) != 0) {
            return Result<UpstreamConfig>::err(
                Error::from_code(ErrCode::ConfigInvalid, "unsupported upstream option"));
        }
        auto weight = parse_u32(suffix.substr(prefix.size()), "invalid upstream weight");
        if (weight.is_err() || weight.value() == 0) {
            return Result<UpstreamConfig>::err(
                Error::from_code(ErrCode::ConfigInvalid, "invalid upstream weight"));
        }
        cfg.weight = weight.value();
    }
    return Result<UpstreamConfig>::ok(std::move(cfg));
}

} // namespace

Result<ProxyConfig> config_from_file(std::string_view path) {
    ProxyConfig cfg;
    std::ifstream in{std::string(path)};
    if (!in.good()) {
        if (path == "./uproxy.toml" || path == "uproxy.toml") {
            return Result<ProxyConfig>::ok(std::move(cfg));
        }
        return Result<ProxyConfig>::err(
            Error::from_code(ErrCode::ConfigSyntax, "config not found"));
    }

    enum class Section { Root, Proxy, TLS, Pool, Log, Upstream };
    Section section = Section::Root;
    UpstreamConfig* current_upstream = nullptr;
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        const std::string t = trim(line);
        if (t.empty()) {
            continue;
        }
        if (t == "[proxy]") {
            section = Section::Proxy;
            continue;
        }
        if (t == "[tls]") {
            section = Section::TLS;
            continue;
        }
        if (t == "[pool]") {
            section = Section::Pool;
            continue;
        }
        if (t == "[log]") {
            section = Section::Log;
            continue;
        }
        if (t == "[[upstream]]") {
            cfg.upstreams.emplace_back();
            current_upstream = &cfg.upstreams.back();
            section = Section::Upstream;
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            return Result<ProxyConfig>::err(Error::from_code(
                ErrCode::ConfigSyntax, "syntax error on line " + std::to_string(line_no)));
        }
        const std::string key = trim(std::string_view(t).substr(0, eq));
        const std::string value = trim(std::string_view(t).substr(eq + 1));

        auto parse_assign_u32 = [&](uint32_t& out) -> Result<void> {
            auto parsed = parse_u32(value, key);
            if (parsed.is_err()) {
                return Result<void>::err(parsed.error());
            }
            out = parsed.value();
            return Result<void>::ok();
        };

        if (section == Section::Proxy) {
            if (key == "listen_addr") {
                cfg.listen_addr = parse_string(value);
            } else if (key == "listen_port") {
                auto port = parse_port(value, key);
                if (port.is_err()) {
                    return Result<ProxyConfig>::err(port.error());
                }
                cfg.listen_port = port.value();
            } else if (key == "tls_port") {
                auto port = parse_port(value, key);
                if (port.is_err()) {
                    return Result<ProxyConfig>::err(port.error());
                }
                cfg.tls_port = port.value();
            } else if (key == "worker_threads") {
                auto r = parse_assign_u32(cfg.worker_threads);
                if (r.is_err()) {
                    return Result<ProxyConfig>::err(r.error());
                }
            }
        } else if (section == Section::TLS) {
            if (key == "cert_file") {
                cfg.tls.cert_file = parse_string(value);
            } else if (key == "key_file") {
                cfg.tls.key_file = parse_string(value);
            } else if (key == "min_version") {
                cfg.tls.min_version = parse_string(value);
            } else if (key == "alpn") {
                cfg.tls.alpn.clear();
                if (value.find("h2") != std::string::npos) {
                    cfg.tls.alpn.push_back("h2");
                }
                if (value.find("http/1.1") != std::string::npos) {
                    cfg.tls.alpn.push_back("http/1.1");
                }
            }
        } else if (section == Section::Pool) {
            if (key == "max_connections_per_upstream") {
                auto r = parse_assign_u32(cfg.pool.max_conns_per_upstream);
                if (r.is_err()) {
                    return Result<ProxyConfig>::err(r.error());
                }
            } else if (key == "connect_timeout_ms") {
                auto r = parse_assign_u32(cfg.pool.connect_timeout_ms);
                if (r.is_err()) {
                    return Result<ProxyConfig>::err(r.error());
                }
            } else if (key == "idle_timeout_ms") {
                auto r = parse_assign_u32(cfg.pool.idle_timeout_ms);
                if (r.is_err()) {
                    return Result<ProxyConfig>::err(r.error());
                }
            } else if (key == "max_keepalive") {
                auto r = parse_assign_u32(cfg.pool.max_keepalive);
                if (r.is_err()) {
                    return Result<ProxyConfig>::err(r.error());
                }
            }
        } else if (section == Section::Log) {
            if (key == "level") {
                cfg.log.level = parse_string(value);
            } else if (key == "format") {
                cfg.log.format = parse_string(value);
            } else if (key == "file") {
                cfg.log.file = parse_string(value);
            }
        } else if (section == Section::Upstream && current_upstream != nullptr) {
            if (key == "name") {
                current_upstream->name = parse_string(value);
            } else if (key == "addr") {
                current_upstream->addr = parse_string(value);
            } else if (key == "port") {
                auto port = parse_port(value, key);
                if (port.is_err()) {
                    return Result<ProxyConfig>::err(port.error());
                }
                current_upstream->port = port.value();
            } else if (key == "weight") {
                auto weight = parse_u32(value, key);
                if (weight.is_err()) {
                    return Result<ProxyConfig>::err(weight.error());
                }
                current_upstream->weight = weight.value();
            } else if (key == "healthy") {
                current_upstream->healthy = parse_bool(value);
            } else if (key == "protocol" && parse_string(value) == "h2c") {
                current_upstream->proto = UpstreamConfig::Proto::H2C;
            }
        }
    }
    return Result<ProxyConfig>::ok(std::move(cfg));
}

Result<ProxyConfig> config_apply_cli(ProxyConfig base, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        auto need_value = [&](std::string_view name) -> Result<std::string_view> {
            if (i + 1 >= argc) {
                return Result<std::string_view>::err(Error::from_code(
                    ErrCode::ConfigInvalid, std::string(name) + " requires value"));
            }
            ++i;
            return Result<std::string_view>::ok(argv[i]);
        };
        if (arg == "--config") {
            auto value = need_value(arg);
            if (value.is_err()) {
                return Result<ProxyConfig>::err(value.error());
            }
            auto loaded = config_from_file(value.value());
            if (loaded.is_err()) {
                return loaded;
            }
            base = std::move(loaded).value();
        } else if (arg == "--listen-addr") {
            auto value = need_value(arg);
            if (value.is_err()) {
                return Result<ProxyConfig>::err(value.error());
            }
            base.listen_addr = std::string(value.value());
        } else if (arg == "--listen-port") {
            auto value = need_value(arg);
            if (value.is_err()) {
                return Result<ProxyConfig>::err(value.error());
            }
            auto port = parse_port(value.value(), "listen port");
            if (port.is_err()) {
                return Result<ProxyConfig>::err(port.error());
            }
            base.listen_port = port.value();
        } else if (arg == "--tls-port") {
            auto value = need_value(arg);
            if (value.is_err()) {
                return Result<ProxyConfig>::err(value.error());
            }
            auto port = parse_port(value.value(), "tls port");
            if (port.is_err()) {
                return Result<ProxyConfig>::err(port.error());
            }
            base.tls_port = port.value();
        } else if (arg == "--upstream") {
            auto value = need_value(arg);
            if (value.is_err()) {
                return Result<ProxyConfig>::err(value.error());
            }
            auto upstream = parse_upstream_cli(value.value());
            if (upstream.is_err()) {
                return Result<ProxyConfig>::err(upstream.error());
            }
            base.upstreams.push_back(std::move(upstream).value());
        } else if (arg == "--no-tls") {
            base.tls.enabled = false;
        } else if (arg == "--log-level") {
            auto value = need_value(arg);
            if (value.is_err()) {
                return Result<ProxyConfig>::err(value.error());
            }
            base.log.level = std::string(value.value());
        } else if (arg == "--workers") {
            auto value = need_value(arg);
            if (value.is_err()) {
                return Result<ProxyConfig>::err(value.error());
            }
            auto workers = parse_u32(value.value(), "workers");
            if (workers.is_err()) {
                return Result<ProxyConfig>::err(workers.error());
            }
            base.worker_threads = workers.value();
        }
    }
    return Result<ProxyConfig>::ok(std::move(base));
}

Result<ProxyConfig> config_validate(ProxyConfig cfg) {
    if (cfg.upstreams.empty()) {
        return Result<ProxyConfig>::err(Error::from_code(ErrCode::ConfigInvalid, "zero upstreams"));
    }
    if (cfg.log.level != "trace" && cfg.log.level != "debug" && cfg.log.level != "info" &&
        cfg.log.level != "warn" && cfg.log.level != "error") {
        return Result<ProxyConfig>::err(
            Error::from_code(ErrCode::ConfigInvalid, "invalid log level"));
    }
    if (cfg.log.format != "json" && cfg.log.format != "text") {
        return Result<ProxyConfig>::err(
            Error::from_code(ErrCode::ConfigInvalid, "invalid log format"));
    }
    for (const auto& upstream : cfg.upstreams) {
        if (upstream.addr.empty() || upstream.port == 0 || upstream.weight == 0) {
            return Result<ProxyConfig>::err(
                Error::from_code(ErrCode::ConfigInvalid, "invalid upstream"));
        }
    }
    if (cfg.tls.enabled && (!file_exists(cfg.tls.cert_file) || !file_exists(cfg.tls.key_file))) {
        return Result<ProxyConfig>::err(
            Error::from_code(ErrCode::ConfigInvalid, "missing TLS certificate or key"));
    }
    if (cfg.tls.min_version != "tls1.2" && cfg.tls.min_version != "tls1.3") {
        return Result<ProxyConfig>::err(
            Error::from_code(ErrCode::ConfigInvalid, "invalid TLS minimum version"));
    }
    return Result<ProxyConfig>::ok(std::move(cfg));
}

} // namespace uproxy
