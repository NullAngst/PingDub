// SPDX-License-Identifier: GPL-3.0-or-later
#include "SysInfo.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace pd {

uint64_t freeBytes(const std::string& dirIn) {
    // A folder that does not exist yet will be created on the drive of its
    // nearest existing parent, so measure that.
    std::string dir = dirIn;
    struct statvfs s;
    while (statvfs(dir.c_str(), &s) != 0) {
        const size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos || dir.size() <= 1) return 0;
        dir = slash == 0 ? "/" : dir.substr(0, slash);
    }
    return uint64_t(s.f_bavail) * uint64_t(s.f_frsize);
}

bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    size_t pos = 0;
    while (pos != std::string::npos) {
        pos = path.find('/', pos + 1);
        cur = path.substr(0, pos);
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/tmp";
}

std::string expandHome(const std::string& path) {
    if (path == "~") return homeDir();
    if (path.rfind("~/", 0) == 0) return homeDir() + path.substr(1);
    return path;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string readSmallFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
    return s;
}

std::vector<ProcessInfo> processesHolding(const std::string& devicePath) {
    std::vector<ProcessInfo> out;
    char real[PATH_MAX];
    std::string target = devicePath;
    if (realpath(devicePath.c_str(), real)) target = real;
    const int self = getpid();
    DIR* proc = opendir("/proc");
    if (!proc) return out;
    while (dirent* e = readdir(proc)) {
        const int pid = std::atoi(e->d_name);
        if (pid <= 0 || pid == self) continue;
        const std::string fdDir = std::string("/proc/") + e->d_name + "/fd";
        DIR* fds = opendir(fdDir.c_str());
        if (!fds) continue;
        bool hit = false;
        while (dirent* f = readdir(fds)) {
            if (f->d_name[0] == '.') continue;
            char link[PATH_MAX];
            const std::string p = fdDir + "/" + f->d_name;
            const ssize_t n = readlink(p.c_str(), link, sizeof link - 1);
            if (n <= 0) continue;
            link[n] = 0;
            if (target == link) {
                hit = true;
                break;
            }
        }
        closedir(fds);
        if (hit) out.push_back({pid, readSmallFile(std::string("/proc/") + e->d_name + "/comm")});
    }
    closedir(proc);
    return out;
}

std::vector<std::string> currentGroups() {
    std::vector<std::string> out;
    int n = getgroups(0, nullptr);
    if (n < 0) return out;
    std::vector<gid_t> gids(size_t(n) + 1);
    n = getgroups(n, gids.data());
    gids.resize(size_t(std::max(n, 0)));
    gids.push_back(getegid());
    for (gid_t g : gids) {
        if (const group* gr = getgrgid(g)) {
            std::string name = gr->gr_name;
            bool dup = false;
            for (auto& x : out) dup |= x == name;
            if (!dup) out.push_back(name);
        }
    }
    return out;
}

bool inGroup(const std::string& group) {
    for (const auto& g : currentGroups())
        if (g == group) return true;
    return false;
}

std::string errnoString(int e) { return std::strerror(e); }

}  // namespace pd
