// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace pd {

inline int64_t monotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

inline int64_t realtimeNs() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// Offset to turn a CLOCK_REALTIME timestamp into CLOCK_MONOTONIC.
inline int64_t realtimeToMonotonicOffset() { return monotonicNs() - realtimeNs(); }

// ISO 8601 local time with seconds, for logs and tags.
std::string isoLocalTime(int64_t realtimeNanos);
// "YYYY-MM-DD_HHMMSS" for file names.
std::string fileStamp(int64_t realtimeNanos);

}  // namespace pd
