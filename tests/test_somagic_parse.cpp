// SPDX-License-Identifier: GPL-3.0-or-later
// Builds a Somagic isochronous stream by hand: BT.656 fields with SAV/EAV
// codes cut into [aa aa 00 00] blocks, audio in [aa aa 00 01] blocks between
// them, then checks the parser rebuilds the frames and the audio exactly.
#include "backends/SomagicParse.h"
#include "testing.h"

#include <cstring>
#include <vector>

using namespace pd;

namespace {

// BT.656 XY byte: 1 F V H P3 P2 P1 P0 with the protection bits.
uint8_t xy(int f, int v, int h) {
    const int p3 = v ^ h, p2 = f ^ h, p1 = f ^ v, p0 = f ^ v ^ h;
    return uint8_t(0x80 | (f << 6) | (v << 5) | (h << 4) | (p3 << 3) | (p2 << 2) | (p1 << 1) | p0);
}

uint8_t pixel(int frame, int field, int line, int col) {
    // Values 0x10..0xEF only, so no stray 0xff/0x00 sync patterns in the payload.
    return uint8_t(0x10 + ((frame * 31 + field * 7 + line * 3 + col) % 0xD0));
}

// One frame of BT.656: both fields, a few blanking lines each, then active lines.
void appendFrame(std::vector<uint8_t>& s, int frame, int lines) {
    for (int field = 0; field < 2; ++field) {
        for (int b = 0; b < 3; ++b) {  // vertical blanking lines
            s.insert(s.end(), {0xff, 0x00, 0x00, xy(field, 1, 1)});
            s.insert(s.end(), {0xff, 0x00, 0x00, xy(field, 1, 0)});
            for (int i = 0; i < 1440; ++i) s.push_back(i & 1 ? 0x10 : 0x80);
        }
        for (int line = 0; line < lines; ++line) {
            s.insert(s.end(), {0xff, 0x00, 0x00, xy(field, 0, 0)});  // SAV
            for (int col = 0; col < 1440; ++col) s.push_back(pixel(frame, field, line, col));
            s.insert(s.end(), {0xff, 0x00, 0x00, xy(field, 0, 1)});  // EAV
        }
    }
}

}  // namespace

TEST_CASE("BT.656 stream with interleaved audio blocks") {
    const int lines = 240, frames = 4;
    std::vector<uint8_t> video;
    for (int f = 0; f < frames; ++f) appendFrame(video, f, lines);
    // A final field-0 SAV closes the last frame.
    video.insert(video.end(), {0xff, 0x00, 0x00, xy(0, 1, 1), 0xff, 0x00, 0x00, xy(0, 1, 0)});

    std::vector<uint8_t> audioSrc;
    for (int i = 0; i < 40000; ++i) audioSrc.push_back(uint8_t(i * 17));

    // Cut into 0x400 blocks, an audio block after every 6 video blocks.
    std::vector<uint8_t> stream;
    size_t vpos = 0, apos = 0;
    int vcount = 0;
    while (vpos < video.size()) {
        stream.insert(stream.end(), {0xaa, 0xaa, 0x00, 0x00});
        for (size_t i = 0; i < 0x3fc; ++i) stream.push_back(vpos < video.size() ? video[vpos++] : 0x80);
        if (++vcount % 6 == 0 && apos + 0x3fc <= audioSrc.size()) {
            stream.insert(stream.end(), {0xaa, 0xaa, 0x00, 0x01});
            stream.insert(stream.end(), audioSrc.begin() + long(apos), audioSrc.begin() + long(apos + 0x3fc));
            apos += 0x3fc;
        }
    }
    // One garbage block must be counted and skipped.
    std::vector<uint8_t> junk(0x400, 0x55);
    stream.insert(stream.end(), junk.begin(), junk.end());

    SomagicParser p;
    p.reset(true);
    std::vector<std::vector<uint8_t>> got;
    std::vector<uint8_t> audio;
    p.onFrame = [&](const uint8_t* f, size_t n) { got.emplace_back(f, f + n); };
    p.onAudio = [&](const uint8_t* a, size_t n) {
        CHECK(n % 8 == 0);
        audio.insert(audio.end(), a, a + n);
    };
    // Feed in uneven packet sizes, always whole blocks, as the device does.
    size_t pos = 0;
    int k = 0;
    while (pos < stream.size()) {
        const size_t blocks = size_t(1 + (k++ % 3));
        const size_t n = std::min(blocks * 0x400, stream.size() - pos);
        p.feedPacket(stream.data() + pos, n);
        pos += n;
    }

    // The first frame is dropped as partial; frames 1..3 come out whole.
    CHECK_EQ(got.size(), size_t(frames - 1));
    CHECK_EQ(p.frameBytes(), size_t(720 * 2 * 480));
    bool exact = true;
    for (size_t g = 0; g < got.size(); ++g) {
        const int frame = int(g) + 1;
        for (int line = 0; line < lines && exact; ++line)
            for (int field = 0; field < 2 && exact; ++field) {
                const uint8_t* row = got[g].data() + size_t(2 * line + field) * 1440;
                for (int col = 0; col < 1440; ++col)
                    if (row[col] != pixel(frame, field, line, col)) {
                        exact = false;
                        std::printf("  mismatch frame %d field %d line %d col %d\n", frame, field, line, col);
                        break;
                    }
            }
    }
    CHECK(exact);
    CHECK_EQ(p.badBlocks(), 1u);
    // Audio comes out as whole 8-byte frames, in order, nothing lost.
    CHECK(audio.size() == (apos / 8) * 8);
    CHECK(std::memcmp(audio.data(), audioSrc.data(), audio.size()) == 0);
}

TEST_CASE("PAL line count") {
    SomagicParser p;
    p.reset(false);
    CHECK_EQ(p.linesPerField(), 288);
    CHECK_EQ(p.frameBytes(), size_t(720 * 2 * 576));
}

TEST_CASE("firmware search needs magic and CRC") {
    std::vector<uint8_t> file(20000, 0);
    // A magic sequence with the wrong CRC is not firmware.
    file[100] = 0x0c;
    file[101] = 0x94;
    file[102] = 0xce;
    file[103] = 0x00;
    SomagicFirmware fw;
    CHECK(!somagicExtractFirmware(file, fw));
    CHECK(!somagicFirmwareValid(std::vector<uint8_t>(7502, 0)));
    CHECK_EQ(crc32Ieee(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
}

TEST_MAIN()
