#include "uproxy/log.h"

namespace uproxy {

Logger* g_log = nullptr;

LogLevel parse_log_level(std::string_view level) {
    if (level == "trace") {
        return LogLevel::Trace;
    }
    if (level == "debug") {
        return LogLevel::Debug;
    }
    if (level == "warn") {
        return LogLevel::Warn;
    }
    if (level == "error") {
        return LogLevel::Error;
    }
    return LogLevel::Info;
}

std::string_view log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

Logger::Logger(LogLevel level, bool json, FILE* out)
    : min_level_(level), json_format_(json), out_(out) {
    if (out_ == nullptr) {
        out_ = stderr;
    }
}

std::string Logger::escape_json(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (const char c : input) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

} // namespace uproxy
