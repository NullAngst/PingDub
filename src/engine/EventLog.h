// SPDX-License-Identifier: GPL-3.0-or-later
// The CSV sidecar (<name>.log.csv) that lists every timing event with its
// timeline position, and the JSON sidecar (<name>.json) with settings and totals.
#pragma once

#include "Types.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pd {

class EventLog {
public:
    ~EventLog();
    bool open(const std::string& path, const Rational& fps);
    void close();
    // slot < 0 means "not on the timeline" (before start, session-level events).
    void log(const std::string& event, int64_t slot, uint64_t count, const std::string& detail);
    size_t count() const;
    std::string path() const { return m_path; }

private:
    mutable std::mutex m_mutex;
    FILE* m_f = nullptr;
    std::string m_path;
    Rational m_fps{30000, 1001};
    size_t m_count = 0;
};

// Minimal JSON writer for the sidecar: nested objects of strings and numbers.
class JsonWriter {
public:
    JsonWriter& begin(const std::string& key = {});
    JsonWriter& end();
    JsonWriter& str(const std::string& key, const std::string& v);
    JsonWriter& num(const std::string& key, double v);
    JsonWriter& integer(const std::string& key, long long v);
    JsonWriter& boolean(const std::string& key, bool v);
    JsonWriter& strList(const std::string& key, const std::vector<std::string>& v);
    // Arrays: beginArray, then begin()/end() per object element or intItem, then endArray.
    JsonWriter& beginArray(const std::string& key = {});
    JsonWriter& endArray();
    JsonWriter& intItem(long long v);
    std::string text() const { return m_out + "\n"; }
    static std::string escape(const std::string& s);

private:
    void sep();
    std::string m_out;
    std::vector<bool> m_first;
};

bool writeTextFile(const std::string& path, const std::string& text);

}  // namespace pd
