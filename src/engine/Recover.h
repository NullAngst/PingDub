// SPDX-License-Identifier: GPL-3.0-or-later
// Rebuilds the index of a capture that was cut off (crash, kill -9, power
// loss) by remuxing every readable packet into a new file. Nothing is decoded
// or re-encoded, so the video and audio stay bit-identical.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace pd {

struct RecoverResult {
    bool ok = false;
    std::string outPath;
    std::string message;
    uint64_t packets = 0;
    double seconds = 0;
};

// Writes "<name>.recovered.mkv" (or .avi) next to the input unless outPath is set.
// progress(bytesRead, totalBytes) may be called often; return false to cancel.
RecoverResult recoverCapture(const std::string& inPath, const std::string& outPath = {},
                             std::function<bool(uint64_t, uint64_t)> progress = {});

}  // namespace pd
