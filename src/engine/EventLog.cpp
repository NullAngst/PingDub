// SPDX-License-Identifier: GPL-3.0-or-later
#include "EventLog.h"

#include "Clock.h"

#include <cmath>
#include <cstring>
#include <ctime>

namespace pd {

std::string isoLocalTime(int64_t ns) {
    const time_t t = time_t(ns / 1000000000LL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &tmv);
    return buf;
}

std::string fileStamp(int64_t ns) {
    const time_t t = time_t(ns / 1000000000LL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d_%H%M%S", &tmv);
    return buf;
}

static std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else if (c == '\n') o += ' ';
        else o += c;
    }
    return o + "\"";
}

static std::string timecode(int64_t slot, const Rational& fps) {
    if (slot < 0) return "";
    const double sec = double(slot) * fps.den / fps.num;
    const int h = int(sec / 3600), m = int(std::fmod(sec, 3600) / 60);
    const double s = std::fmod(sec, 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%06.3f", h, m, s);
    return buf;
}

EventLog::~EventLog() { close(); }

bool EventLog::open(const std::string& path, const Rational& fps) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_f = std::fopen(path.c_str(), "w");
    if (!m_f) return false;
    m_path = path;
    m_fps = fps;
    m_count = 0;
    std::fprintf(m_f, "wallclock,timeline_frame,timeline_time,event,count,detail\n");
    std::fflush(m_f);
    return true;
}

void EventLog::close() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_f) std::fclose(m_f);
    m_f = nullptr;
}

void EventLog::log(const std::string& event, int64_t slot, uint64_t count, const std::string& detail) {
    std::lock_guard<std::mutex> lk(m_mutex);
    ++m_count;
    if (!m_f) return;
    std::fprintf(m_f, "%s,%s,%s,%s,%llu,%s\n", isoLocalTime(realtimeNs()).c_str(),
                 slot >= 0 ? std::to_string(slot).c_str() : "", timecode(slot, m_fps).c_str(), event.c_str(),
                 (unsigned long long)count, csvField(detail).c_str());
    std::fflush(m_f);
}

size_t EventLog::count() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_count;
}

// ---------------------------------------------------------------------------

std::string JsonWriter::escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    return o;
}

void JsonWriter::sep() {
    if (m_first.empty()) return;
    if (!m_first.back()) m_out += ",";
    m_first.back() = false;
    m_out += "\n" + std::string(m_first.size() * 2, ' ');
}

JsonWriter& JsonWriter::begin(const std::string& key) {
    sep();
    if (!key.empty()) m_out += "\"" + escape(key) + "\": ";
    m_out += "{";
    m_first.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end() {
    const bool empty = m_first.back();
    m_first.pop_back();
    if (!empty) m_out += "\n" + std::string(m_first.size() * 2, ' ');
    m_out += "}";
    return *this;
}

JsonWriter& JsonWriter::str(const std::string& key, const std::string& v) {
    sep();
    m_out += "\"" + escape(key) + "\": \"" + escape(v) + "\"";
    return *this;
}

JsonWriter& JsonWriter::num(const std::string& key, double v) {
    sep();
    char b[64];
    if (std::isfinite(v)) std::snprintf(b, sizeof b, "%.6g", v);
    else std::snprintf(b, sizeof b, "null");
    m_out += "\"" + escape(key) + "\": " + b;
    return *this;
}

JsonWriter& JsonWriter::integer(const std::string& key, long long v) {
    sep();
    m_out += "\"" + escape(key) + "\": " + std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::boolean(const std::string& key, bool v) {
    sep();
    m_out += "\"" + escape(key) + "\": " + (v ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::strList(const std::string& key, const std::vector<std::string>& v) {
    sep();
    m_out += "\"" + escape(key) + "\": [";
    for (size_t i = 0; i < v.size(); ++i) m_out += (i ? ", \"" : "\"") + escape(v[i]) + "\"";
    m_out += "]";
    return *this;
}

JsonWriter& JsonWriter::beginArray(const std::string& key) {
    sep();
    if (!key.empty()) m_out += "\"" + escape(key) + "\": ";
    m_out += "[";
    m_first.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    const bool empty = m_first.back();
    m_first.pop_back();
    if (!empty) m_out += "\n" + std::string(m_first.size() * 2, ' ');
    m_out += "]";
    return *this;
}

JsonWriter& JsonWriter::intItem(long long v) {
    sep();
    m_out += std::to_string(v);
    return *this;
}

bool writeTextFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace pd
