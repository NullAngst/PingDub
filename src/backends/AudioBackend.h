// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/Types.h"

#include <string>
#include <vector>

namespace pd {

struct MixerControl {
    enum class Kind { Volume, Switch, Enum };
    std::string name;
    int index = 0;
    Kind kind = Kind::Volume;
    long minimum = 0, maximum = 0, value = 0;
    std::vector<std::string> items;  // Enum
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual std::string backendName() const = 0;
    virtual std::string deviceName() const = 0;
    // Negotiates the native format: 48 kHz first, deepest sample format,
    // device channel count. `preferRate` 0 means 48000.
    virtual bool open(int preferRate, AudioFormat& out, std::string& err) = 0;
    virtual bool start(std::string& err) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    // Reads about one period. Returns frames (>0), 0 on timeout, -1 on error.
    // `xrun` is set when an overrun happened and was recovered before this chunk.
    virtual int read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) = 0;
    virtual std::vector<int> supportedRates() const { return {}; }
    virtual std::vector<MixerControl> mixer() { return {}; }
    virtual bool setMixer(const std::string& name, int index, long value, std::string& err) {
        err = "no mixer";
        return false;
    }
    virtual std::string diagnostics() { return {}; }
};

}  // namespace pd
