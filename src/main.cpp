#include "uproxy/config.h"
#include "uproxy/log.h"
#include "uproxy/proxy.h"

#include <csignal>
#include <cstdio>

int main(int argc, char** argv) {
    using namespace uproxy;

    ProxyConfig base;
    auto cfg_result = config_apply_cli(std::move(base), argc, argv).and_then([](ProxyConfig cfg) {
        return config_validate(std::move(cfg));
    });
    if (cfg_result.is_err()) {
        std::fprintf(stderr, "Config error: %s\n", cfg_result.error().to_string().c_str());
        return 1;
    }
    ProxyConfig cfg = std::move(cfg_result).value();

    FILE* log_out = stderr;
    if (!cfg.log.file.empty()) {
        log_out = std::fopen(cfg.log.file.c_str(), "a");
        if (log_out == nullptr) {
            std::fprintf(stderr, "Log error: cannot open %s\n", cfg.log.file.c_str());
            return 1;
        }
    }
    Logger logger(parse_log_level(cfg.log.level), cfg.log.format == "json", log_out);
    g_log = &logger;

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, [](int) { ProxyServer::g_shutdown.store(true); });
    std::signal(SIGTERM, [](int) { ProxyServer::g_shutdown.store(true); });

    LOG_INFO("uproxy starting", "listen", cfg.listen_port, "upstreams", cfg.upstreams.size());
    ProxyServer server(std::move(cfg));
    auto run = server.run();
    if (run.is_err()) {
        LOG_ERROR("fatal error", "msg", run.error().to_string());
        return 1;
    }
    LOG_INFO("uproxy stopped cleanly");
    if (log_out != stderr) {
        std::fclose(log_out);
    }
    return 0;
}
