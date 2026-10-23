// SPDX-License-Identifier: GPL-3.0-or-later
// Protocol details ported from somagic-easycap (Tony Brown, Michal Demin,
// Jeffry Johnston, GPL-2.0-or-later).
#include "SomagicParse.h"

#include <cstring>

namespace pd {

void SomagicParser::reset(bool lines525) {
    m_linesPerField = lines525 ? 240 : 288;
    m_frame.assign(size_t(kWidth) * 2 * size_t(m_linesPerField + 2) * 2, 0);
    m_state = HSYNC;
    m_line = m_col = m_field = m_blank = 0;
    m_seenFieldStart = false;
    m_audio.clear();
}

void SomagicParser::put(uint8_t c) {
    if (m_line >= m_linesPerField || m_col >= kWidth * 2) {
        if (m_col < kWidth * 2) ++m_col;
        return;
    }
    const size_t pos = size_t(2 * m_line + m_field) * size_t(kWidth * 2) + size_t(m_col);
    m_frame[pos] = c;
    ++m_col;
}

// BT.656 timing reference codes: ff 00 00 XY. In XY, bit 6 is F (field),
// bit 5 is V (vertical blanking), bit 4 is H (0 = SAV, 1 = EAV).
void SomagicParser::videoByte(uint8_t c) {
    switch (m_state) {
    case HSYNC:
        if (c == 0xff) m_state = SYNCZ1;
        else put(c);
        break;
    case SYNCZ1:
        if (c == 0x00) {
            m_state = SYNCZ2;
        } else {
            m_state = HSYNC;
            put(0xff);
            put(c);
        }
        break;
    case SYNCZ2:
        if (c == 0x00) {
            m_state = SYNCAV;
        } else {
            m_state = HSYNC;
            put(0xff);
            put(0x00);
            put(c);
        }
        break;
    case SYNCAV: {
        m_state = HSYNC;
        if (c == 0x00) return;  // SDID of sliced data, not a timing code
        if (c & 0x10) {
            // EAV: end of an active line
            if (!m_blank) {
                ++m_line;
                m_col = 0;
                if (m_line > m_linesPerField + 1) m_line = m_linesPerField + 1;
            }
        } else {
            // SAV
            const int field = (c & 0x40) ? 1 : 0;
            const int blank = (c & 0x20) ? 1 : 0;
            const bool fieldEdge = field != m_field;
            const bool blankEdge = blank != m_blank;
            m_field = field;
            m_blank = blank;
            if (m_field == 0 && fieldEdge) {
                // A new frame starts: the previous one is complete. The very
                // first is partial and is dropped.
                if (m_seenFieldStart && onFrame) onFrame(m_frame.data(), frameBytes());
                if (m_seenFieldStart) ++m_frames;
                m_seenFieldStart = true;
            }
            if (m_blank == 0 && blankEdge) {
                m_line = 0;
                m_col = 0;
            }
        }
        break;
    }
    }
}

void SomagicParser::feedPacket(const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos + kBlock <= len) {
        const uint8_t* b = data + pos;
        if (b[0] == 0xaa && b[1] == 0xaa && b[2] == 0x00 && b[3] == 0x00) {
            for (size_t k = 4; k < kBlock; ++k) videoByte(b[k]);
        } else if (b[0] == 0xaa && b[1] == 0xaa && b[2] == 0x00 && b[3] == 0x01) {
            m_audio.insert(m_audio.end(), b + 4, b + kBlock);
            m_audioBytes += kBlock - 4;
            const size_t whole = (m_audio.size() / kAudioFrameBytes) * kAudioFrameBytes;
            if (whole && onAudio) onAudio(m_audio.data(), whole);
            m_audio.erase(m_audio.begin(), m_audio.begin() + long(whole));
        } else {
            ++m_badBlocks;
        }
        pos += kBlock;
    }
}

// --- firmware -----------------------------------------------------------------

uint32_t crc32Ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

namespace {
struct FwInfo {
    size_t length;
    uint8_t magic[4];
    uint32_t crc;  // as gcrypt prints it, big-endian bytes
};
const FwInfo kFirmware[] = {
    {7502, {0x0c, 0x94, 0xce, 0x00}, 0x3489f77bu},
    {6634, {0x0c, 0x94, 0xcc, 0x00}, 0x1ffedebbu},
    {6634, {0x0c, 0x94, 0xcc, 0x00}, 0x601d375fu},
    {7812, {0x0c, 0x94, 0xd9, 0x00}, 0x58c6cf55u},
};
}  // namespace

bool somagicFirmwareValid(const std::vector<uint8_t>& fw) {
    const uint32_t c = crc32Ieee(fw.data(), fw.size());
    for (const FwInfo& f : kFirmware)
        if (fw.size() == f.length && c == f.crc) return true;
    return false;
}

bool somagicExtractFirmware(const std::vector<uint8_t>& file, SomagicFirmware& out) {
    for (size_t pos = 0; pos + 4 <= file.size(); ++pos) {
        for (size_t p = 0; p < sizeof kFirmware / sizeof kFirmware[0]; ++p) {
            const FwInfo& f = kFirmware[p];
            if (std::memcmp(file.data() + pos, f.magic, 4) != 0) continue;
            if (pos + f.length > file.size()) continue;
            if (crc32Ieee(file.data() + pos, f.length) != f.crc) continue;
            out.data.assign(file.begin() + long(pos), file.begin() + long(pos + f.length));
            out.product = int(p);
            return true;
        }
    }
    return false;
}

}  // namespace pd
