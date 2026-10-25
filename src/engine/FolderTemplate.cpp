// SPDX-License-Identifier: GPL-3.0-or-later
#include "FolderTemplate.h"

#include "SysInfo.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace pd {

const std::vector<std::string>& templateTokens() {
    static const std::vector<std::string> t{"title", "date", "year", "month", "time", "device", "input", "profile"};
    return t;
}

namespace {

// Calls `tok` for each {token} and `lit` for each literal run. False on a malformed template.
template <typename Tok, typename Lit>
bool walk(const std::string& s, Tok tok, Lit lit, std::string& err) {
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '{') {
            const size_t e = s.find('}', i + 1);
            if (e == std::string::npos) {
                err = "unmatched \"{\" in the folder";
                return false;
            }
            const std::string name = s.substr(i + 1, e - i - 1);
            if (!tok(name)) {
                err = "unknown token {" + name + "}";
                return false;
            }
            i = e + 1;
            continue;
        }
        if (c == '}') {
            err = "unmatched \"}\" in the folder";
            return false;
        }
        size_t e = s.find_first_of("{}", i);
        if (e == std::string::npos) e = s.size();
        lit(s.substr(i, e - i));
        i = e;
    }
    return true;
}

bool known(const std::string& name) {
    const auto& t = templateTokens();
    return std::find(t.begin(), t.end(), name) != t.end();
}

std::string timeField(int64_t ns, const char* fmt) {
    const time_t t = time_t(ns / 1000000000LL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof buf, fmt, &tmv);
    return buf;
}

}  // namespace

bool validateFolderTemplate(const std::string& tmpl, std::string& err) {
    if (tmpl.empty()) {
        err = "the folder is empty";
        return false;
    }
    return walk(tmpl, [](const std::string& n) { return known(n); }, [](const std::string&) {}, err);
}

bool templateUsesToken(const std::string& tmpl, const std::string& token) {
    return tmpl.find("{" + token + "}") != std::string::npos;
}

std::string cleanTemplateValue(const std::string& v) {
    std::string s;
    s.reserve(v.size());
    for (unsigned char c : v) s += (c == '/' || c < 0x20 || c == 0x7f) ? '_' : char(c);
    auto trim = [](std::string& x) {
        size_t a = 0, b = x.size();
        while (a < b && (x[a] == ' ' || x[a] == '.')) ++a;
        while (b > a && (x[b - 1] == ' ' || x[b - 1] == '.')) --b;
        x = x.substr(a, b - a);
    };
    trim(s);
    if (s.size() > 100) {
        size_t cut = 100;
        // Do not split a UTF-8 sequence.
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
        trim(s);
    }
    return s;
}

bool resolveFolderTemplate(const std::string& tmpl, const TemplateValues& v, std::string& out, std::string& err) {
    std::string r;
    auto value = [&](const std::string& name) -> std::string {
        std::string x;
        if (name == "title") {
            x = cleanTemplateValue(v.title);
            return x.empty() ? "Untitled" : x;
        }
        if (name == "date") return timeField(v.startRealNs, "%Y-%m-%d");
        if (name == "year") return timeField(v.startRealNs, "%Y");
        if (name == "month") return timeField(v.startRealNs, "%m");
        if (name == "time") return timeField(v.startRealNs, "%H%M%S");
        if (name == "device") x = cleanTemplateValue(v.device);
        else if (name == "input") x = cleanTemplateValue(v.input);
        else if (name == "profile") x = cleanTemplateValue(v.profile);
        return x.empty() ? "Unknown" : x;
    };
    const bool ok = walk(
        tmpl,
        [&](const std::string& n) {
            if (!known(n)) return false;
            r += value(n);
            return true;
        },
        [&](const std::string& lit) { r += lit; }, err);
    if (!ok) return false;
    out = expandHome(r);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return true;
}

std::string templateBase(const std::string& tmpl) {
    const size_t brace = tmpl.find('{');
    std::string head = brace == std::string::npos ? tmpl : tmpl.substr(0, brace);
    if (brace != std::string::npos) {
        const size_t slash = head.find_last_of('/');
        head = slash == std::string::npos ? std::string() : head.substr(0, slash + 1);
    }
    std::string out = expandHome(head.empty() ? "." : head);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::string nearestExistingDir(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    while (!p.empty()) {
        if (fs::is_directory(p, ec)) return p.string();
        const fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return "/";
}

std::vector<std::string> findCaptures(const std::string& base, size_t limit) {
    namespace fs = std::filesystem;
    std::vector<std::pair<fs::file_time_type, std::string>> found;
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return {};
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 6) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        const std::string name = it->path().filename().string();
        const std::string ext = it->path().extension().string();
        if (ext != ".mkv" && ext != ".avi") continue;
        if (name.find(".recovered.") != std::string::npos) continue;
        found.push_back({it->last_write_time(fe), it->path().string()});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (const auto& f : found) {
        if (out.size() >= limit) break;
        out.push_back(f.second);
    }
    return out;
}

}  // namespace pd
