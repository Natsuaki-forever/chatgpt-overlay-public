#pragma once
#include "core.hpp"
#include <cstdio>

inline void record_native_issue(const fs::path& directory, const std::string& code,
                                const char* state, int region = 0) noexcept {
    try {
        fs::create_directories(directory);
        auto path = directory / L"native.jsonl";
        std::error_code error;
        if (fs::file_size(path, error) >= 128 * 1024 && !error) {
            auto previous = directory / L"native.previous.jsonl";
            if (!MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        }
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        char time[32];
        std::snprintf(time, sizeof(time), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                      unsigned(utc.wYear), unsigned(utc.wMonth), unsigned(utc.wDay),
                      unsigned(utc.wHour), unsigned(utc.wMinute), unsigned(utc.wSecond));
        json event = {{"time", time}, {"source", "native"}, {"code", code}, {"state", state}};
        if (region > 0) event["region"] = region;
        std::ofstream(path, std::ios::app | std::ios::binary) << event.dump() << '\n';
    } catch (...) {
        // Reporting must not interfere with the overlay or its configuration.
    }
}

inline void update_native_issue(const fs::path& directory, std::string& active,
                                const std::string& code, int region = 0) {
    if (active == code) return;
    if (code.empty()) record_native_issue(directory, active, "recovered", region);
    else record_native_issue(directory, code, "failed", region);
    active = code;
}
