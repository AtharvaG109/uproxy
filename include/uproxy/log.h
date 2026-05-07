#pragma once

#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace uproxy {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

LogLevel parse_log_level(std::string_view level);
std::string_view log_level_name(LogLevel level);

class Logger {
    LogLevel min_level_;
    bool json_format_;
    FILE* out_;
    std::mutex mu_;

    static std::string escape_json(std::string_view input);

    template <typename T> static std::string value_to_string(const T& value) {
        std::ostringstream os;
        os << value;
        return os.str();
    }

    static std::string value_to_string(std::string_view value) {
        return std::string(value);
    }
    static std::string value_to_string(const std::string& value) {
        return value;
    }
    static std::string value_to_string(const char* value) {
        return value == nullptr ? "" : value;
    }

  public:
    Logger(LogLevel level, bool json, FILE* out);

    template <typename... KV> void log(LogLevel level, std::string_view msg, KV&&... kv) {
        if (static_cast<int>(level) < static_cast<int>(min_level_)) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (json_format_) {
            std::fprintf(out_, "{\"level\":\"%.*s\",\"msg\":\"%s\"",
                         static_cast<int>(log_level_name(level).size()),
                         log_level_name(level).data(), escape_json(msg).c_str());
            write_json_pairs(std::forward<KV>(kv)...);
            std::fprintf(out_, "}\n");
        } else {
            std::fprintf(out_, "%.*s %.*s", static_cast<int>(log_level_name(level).size()),
                         log_level_name(level).data(), static_cast<int>(msg.size()), msg.data());
            write_text_pairs(std::forward<KV>(kv)...);
            std::fprintf(out_, "\n");
        }
        std::fflush(out_);
    }

    template <typename... KV> void info(std::string_view msg, KV&&... kv) {
        log(LogLevel::Info, msg, std::forward<KV>(kv)...);
    }
    template <typename... KV> void warn(std::string_view msg, KV&&... kv) {
        log(LogLevel::Warn, msg, std::forward<KV>(kv)...);
    }
    template <typename... KV> void error(std::string_view msg, KV&&... kv) {
        log(LogLevel::Error, msg, std::forward<KV>(kv)...);
    }
    template <typename... KV> void debug(std::string_view msg, KV&&... kv) {
        log(LogLevel::Debug, msg, std::forward<KV>(kv)...);
    }

  private:
    void write_json_pairs() {}
    void write_text_pairs() {}

    template <typename K, typename V, typename... Rest>
    void write_json_pairs(K&& key, V&& value, Rest&&... rest) {
        const auto rendered = value_to_string(std::forward<V>(value));
        std::fprintf(out_, ",\"%s\":\"%s\"",
                     escape_json(value_to_string(std::forward<K>(key))).c_str(),
                     escape_json(rendered).c_str());
        write_json_pairs(std::forward<Rest>(rest)...);
    }

    template <typename K, typename V, typename... Rest>
    void write_text_pairs(K&& key, V&& value, Rest&&... rest) {
        std::fprintf(out_, " %s=%s", value_to_string(std::forward<K>(key)).c_str(),
                     value_to_string(std::forward<V>(value)).c_str());
        write_text_pairs(std::forward<Rest>(rest)...);
    }
};

extern Logger* g_log;

} // namespace uproxy

#define LOG_INFO(msg, ...)                                                                         \
    do {                                                                                           \
        if (::uproxy::g_log != nullptr) {                                                          \
            ::uproxy::g_log->info((msg)__VA_OPT__(, ) __VA_ARGS__);                                \
        }                                                                                          \
    } while (false)
#define LOG_WARN(msg, ...)                                                                         \
    do {                                                                                           \
        if (::uproxy::g_log != nullptr) {                                                          \
            ::uproxy::g_log->warn((msg)__VA_OPT__(, ) __VA_ARGS__);                                \
        }                                                                                          \
    } while (false)
#define LOG_ERROR(msg, ...)                                                                        \
    do {                                                                                           \
        if (::uproxy::g_log != nullptr) {                                                          \
            ::uproxy::g_log->error((msg)__VA_OPT__(, ) __VA_ARGS__);                               \
        }                                                                                          \
    } while (false)
