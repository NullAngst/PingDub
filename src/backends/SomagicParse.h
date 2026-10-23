// SPDX-License-Identifier: GPL-3.0-or-later
// Stream parser for Somagic SMI2021 EasyCaps (port of the somagic-easycap
// userspace tools' protocol code, GPL-2.0-or-later). The device sends
// isochronous packets cut into 0x400-byte blocks: [aa aa 00 00] blocks carry
// the SAA7113's BT.656 stream, [aa aa 00 01] blocks carry PCM audio.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace pd {

class SomagicParser {
public:
    // Interleaved UYVY frame, 720 x (2 * linesPerField), field 0 on even lines.
    std::function<void(const uint8_t* frame, size_t size)> onFrame;
    // Audio bytes as sent: 32-bit little-endian stereo at 48 kHz, whole frames only.
    std::function<void(const uint8_t* pcm, size_t bytes)> onAudio;

    void reset(bool lines525);
    void feedPacket(const uint8_t* data, size_t len);
    int linesPerField() const { return m_linesPerField; }
    size_t frameBytes() const { return size_t(kWidth) * 2 * size_t(m_linesPerField) * 2; }
    uint64_t frames() const { return m_frames; }
    uint64_t badBlocks() const { return m_badBlocks; }
    uint64_t audioBytes() const { return m_audioBytes; }

    static constexpr int kWidth = 720;
    static constexpr size_t kBlock = 0x400;
    static constexpr int kAudioFrameBytes = 8;

private:
    void videoByte(uint8_t c);
    void put(uint8_t c);

    enum State { HSYNC, SYNCZ1, SYNCZ2, SYNCAV };
    State m_state = HSYNC;
    int m_line = 0;
    int m_col = 0;
    int m_field = 0;
    int m_blank = 0;
    bool m_seenFieldStart = false;
    int m_linesPerField = 240;
    std::vector<uint8_t> m_frame;
    std::vector<uint8_t> m_audio;
    uint64_t m_frames = 0;
    uint64_t m_badBlocks = 0;
    uint64_t m_audioBytes = 0;
};

// Somagic firmware lives inside the Windows driver (SmiUsbGrabber3C.sys) and
// cannot be redistributed. These find and check it there.
struct SomagicFirmware {
    std::vector<uint8_t> data;
    int product = -1;  // index into the known firmware table
};
bool somagicExtractFirmware(const std::vector<uint8_t>& driverFile, SomagicFirmware& out);
bool somagicFirmwareValid(const std::vector<uint8_t>& fw);
uint32_t crc32Ieee(const uint8_t* data, size_t len);

}  // namespace pd
