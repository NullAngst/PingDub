// SPDX-License-Identifier: GPL-3.0-or-later
// ALSA capture straight from the hw: device: the real device clock, its native
// rate and depth, no plug layer and no hidden resampling. Timestamps come from
// the driver on CLOCK_MONOTONIC, the same clock V4L2 uses.
#pragma once

#include "AudioBackend.h"

#include <string>
#include <vector>

typedef struct _snd_pcm snd_pcm_t;

namespace pd {

struct AlsaDevice {
    std::string pcm;         // "hw:CARD=xyz,DEV=0", "pipewire", "default"
    std::string name;        // human readable
    std::string cardId;
    int cardIndex = -1;
    std::string sysfsDevice; // resolved /sys path of the card's parent device
    bool fallback = false;   // pipewire/default: may resample
};

class AlsaBackend : public AudioBackend {
public:
    explicit AlsaBackend(const std::string& pcm);
    ~AlsaBackend() override;

    static std::vector<AlsaDevice> enumerate();
    // Card under the same USB/PCI parent as /dev/videoN, or "" if none.
    static std::string pairedPcmFor(const std::string& videoNode);
    // Sets known mixer quirks for the video driver (stk1160: capture source Line).
    // Returns what was done, empty if nothing applied.
    static std::string applyMixerQuirks(const std::string& videoDriver);

    std::string backendName() const override { return "ALSA"; }
    std::string deviceName() const override { return m_pcm; }
    bool open(int preferRate, AudioFormat& out, std::string& err) override;
    bool start(std::string& err) override;
    void stop() override;
    void close() override;
    int read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) override;
    std::vector<int> supportedRates() const override { return m_rates; }
    std::vector<MixerControl> mixer() override;
    bool setMixer(const std::string& name, int index, long value, std::string& err) override;
    std::string diagnostics() override;

private:
    std::string mixerDevice() const;
    std::string m_pcm;
    snd_pcm_t* m_h = nullptr;
    AudioFormat m_fmt;
    bool m_packed24 = false;  // S24_3LE
    bool m_low24 = false;     // S24_LE: 24 bits in the low part of 32
    unsigned long m_period = 480;
    std::vector<uint8_t> m_raw;
    std::vector<int> m_rates;
    bool m_pendingXrun = false;
    std::string m_formatDesc;
};

}  // namespace pd
