// SPDX-License-Identifier: GPL-3.0-or-later
// Small Linux helpers used by the engine and the diagnostics window.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pd {

uint64_t freeBytes(const std::string& dir);   // 0 if unknown
bool makeDirs(const std::string& path);
std::string homeDir();
std::string expandHome(const std::string& path);
bool fileExists(const std::string& path);
std::string readSmallFile(const std::string& path);  // trimmed, empty on error

struct ProcessInfo {
    int pid = 0;
    std::string name;
};
// Processes with `devicePath` open, found by scanning /proc/*/fd.
std::vector<ProcessInfo> processesHolding(const std::string& devicePath);

// Group names the current process belongs to.
std::vector<std::string> currentGroups();
bool inGroup(const std::string& group);

std::string errnoString(int e);

}  // namespace pd
