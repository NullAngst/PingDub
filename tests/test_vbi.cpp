// SPDX-License-Identifier: GPL-3.0-or-later
// Builds raw VBI frames with a line 21 caption waveform (clock run-in, start
// bits, two odd-parity bytes) the way a decoder chip samples them, runs them
// through zvbi via VbiProcessor, and checks the SCC and SRT that come out.
#include "engine/Cea608.h"
#include "engine/VbiCapture.h"
#include "testing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <linux/videodev2.h>
#include <unistd.h>

using namespace pd;
namespace fs = std::filesystem;

namespace {

const int kRate = 27000000;
const int kSpl = 2048;
const int kOffset = 128;  // samples from 0H to the first stored sample

// Renders one line 21 waveform carrying two bytes (parity already applied).
void renderLine21(uint8_t* line, uint8_t b1, uint8_t b2) {
    const double f = 503496.5;            // bit rate, 32 x fH
    const double t0 = 10.5e-6;            // clock run-in starts 10.5 us after 0H
    const double runIn = 7.0 / f;
    const double lo = 40, hi = 160;       // sample levels (blanking, 50 IRE-ish)
    const uint32_t bits = 0x4u | (uint32_t(b1) << 3) | (uint32_t(b2) << 11);  // start bits 0,0,1 then LSB first
    for (int i = 0; i < kSpl; ++i) {
        const double t = double(i + kOffset) / kRate;
        double v = lo;
        if (t >= t0 && t < t0 + runIn) {
            v = lo + (hi - lo) * 0.5 * (1.0 - std::cos(2.0 * M_PI * f * (t - t0)));
        } else if (t >= t0 + runIn) {
            const double pos = (t - t0 - runIn) * f;
            const int bi = int(pos);
            if (bi < 19) {
                auto bitAt = [&](int k) { return (k >= 0 && k < 19) ? double((bits >> k) & 1) : 0.0; };
                // raised-cosine edges over a quarter bit
                const double frac = pos - bi;
                double cur = bitAt(bi), prev = bitAt(bi - 1);
                double level = cur;
                if (frac < 0.125) {
                    const double w = 0.5 + 0.5 * std::sin(M_PI * (frac / 0.25));
                    level = prev + (cur - prev) * w;
                }
                v = lo + (hi - lo) * level;
            }
        }
        line[i] = uint8_t(std::clamp(v, 0.0, 255.0));
    }
}

VbiParams params() {
    VbiParams p;
    p.samplingRate = kRate;
    p.offset = kOffset;
    p.samplesPerLine = kSpl;
    p.start[0] = 10;
    p.count[0] = 12;
    p.start[1] = 273;
    p.count[1] = 12;
    p.lines525 = true;
    return p;
}

std::vector<uint8_t> frameWith(uint8_t f1a, uint8_t f1b) {
    const VbiParams p = params();
    std::vector<uint8_t> buf(size_t(p.count[0] + p.count[1]) * kSpl, 40);
    renderLine21(buf.data() + size_t(21 - p.start[0]) * kSpl, f1a, f1b);
    // field 2 line 284 carries nulls
    renderLine21(buf.data() + size_t(p.count[0] + 284 - p.start[1]) * kSpl, 0x80, 0x80);
    return buf;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("parity helper") {
    CHECK_EQ(withOddParity(0x14), 0x94);
    CHECK_EQ(withOddParity(0x20), 0x20);
    CHECK_EQ(withOddParity(0x00), 0x80);
    CHECK(oddParityOk(0x94));
    CHECK(!oddParityOk(0x14));
}

TEST_CASE("drop-frame timecode") {
    const Rational ntsc{30000, 1001};
    CHECK(SccWriter::timecode(0, ntsc) == "00:00:00;00");
    CHECK(SccWriter::timecode(1800, ntsc) == "00:01:00;02");
    CHECK(SccWriter::timecode(17982, ntsc) == "00:10:00;00");
    CHECK(SccWriter::timecode(107892, ntsc) == "01:00:00;00");
    CHECK(SccWriter::timecode(50, Rational{25, 1}) == "00:00:02:00");
}

TEST_CASE("raw VBI pop-on caption decodes to SCC and SRT") {
    const std::string dir = fs::temp_directory_path() / ("pingdub-vbi-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const std::string base = dir + "/cap";

    VbiProcessor proc;
    CHECK(proc.begin(params(), base, Rational{30000, 1001}));

    auto p = withOddParity;
    // Pop-on: RCL, ENM, PAC row 15, text, EOC to show, later EDM to clear.
    // Control codes go out twice, as encoders send them.
    std::vector<std::pair<uint8_t, uint8_t>> seq;
    for (int i = 0; i < 10; ++i) seq.push_back({0x80, 0x80});
    auto ctl = [&](uint8_t a, uint8_t b) {
        seq.push_back({p(a), p(b)});
        seq.push_back({p(a), p(b)});
    };
    ctl(0x14, 0x20);  // RCL
    ctl(0x14, 0x2E);  // ENM
    ctl(0x14, 0x70);  // PAC row 15 col 0
    seq.push_back({p('H'), p('E')});
    seq.push_back({p('L'), p('L')});
    seq.push_back({p('O'), p(0)});
    ctl(0x14, 0x2F);  // EOC: caption appears here
    const size_t eocFrame = seq.size() - 1;
    for (int i = 0; i < 60; ++i) seq.push_back({0x80, 0x80});
    ctl(0x14, 0x2C);  // EDM: caption cleared
    const size_t edmFrame = seq.size() - 1;
    for (int i = 0; i < 30; ++i) seq.push_back({0x80, 0x80});

    std::string live;
    for (size_t i = 0; i < seq.size(); ++i) {
        auto buf = frameWith(seq[i].first, seq[i].second);
        proc.process(buf.data(), buf.size(), int64_t(i));
        if (i == eocFrame + 5) live = proc.liveText(1);
    }
    CHECK_MSG(live == "HELLO", "live readout '%s'", live.c_str());
    CHECK(proc.captionBytes() >= 10);
    const auto files = proc.finish(int64_t(seq.size()));
    bool haveVbi = false, haveScc = false, haveSrt = false;
    for (const auto& f : files) {
        haveVbi |= f == base + ".vbi";
        haveScc |= f == base + ".cc1.scc";
        haveSrt |= f == base + ".cc1.srt";
    }
    CHECK(haveVbi);
    CHECK(haveScc);
    CHECK(haveSrt);

    const std::string scc = slurp(base + ".cc1.scc");
    CHECK(scc.rfind("Scenarist_SCC V1.0", 0) == 0);
    CHECK_MSG(scc.find("00:00:00;10\t9420 9420 94ae 94ae 9470 9470 c845 4c4c 4f80 942f 942f") != std::string::npos,
              "scc was:\n%s", scc.c_str());

    const std::string srt = slurp(base + ".cc1.srt");
    CHECK_MSG(srt.find("HELLO") != std::string::npos, "srt was:\n%s", srt.c_str());
    // Starts at the EOC frame, ends at the EDM frame (both within a frame or two).
    char expectStart[32], expectEnd[32];
    const double ts = double(eocFrame) * 1001.0 / 30000.0, te = double(edmFrame) * 1001.0 / 30000.0;
    std::snprintf(expectStart, sizeof expectStart, "00:00:%02d,%03d", int(ts), int(std::lround((ts - int(ts)) * 1000)) );
    std::snprintf(expectEnd, sizeof expectEnd, "00:00:%02d,%03d", int(te), int(std::lround((te - int(te)) * 1000)));
    const size_t arrow = srt.find(" --> ");
    CHECK(arrow != std::string::npos);
    if (arrow != std::string::npos) {
        auto toSec = [](const std::string& s) {
            return std::stoi(s.substr(0, 2)) * 3600 + std::stoi(s.substr(3, 2)) * 60 + std::stoi(s.substr(6, 2)) +
                   std::stoi(s.substr(9, 3)) / 1000.0;
        };
        const double s0 = toSec(srt.substr(arrow - 12, 12)), s1 = toSec(srt.substr(arrow + 5, 12));
        CHECK_MSG(std::fabs(s0 - ts) < 0.1, "start %.3f expected %.3f", s0, ts);
        CHECK_MSG(std::fabs(s1 - te) < 0.1, "end %.3f expected %.3f", s1, te);
    }

    // .vbi master copy: header plus one record per frame.
    const auto vbiSize = fs::file_size(base + ".vbi");
    CHECK(vbiSize > seq.size() * (size_t(24) * kSpl));
    fs::remove_all(dir);
}

TEST_CASE("sliced VBI path") {
    VbiParams p = params();
    p.sliced = true;
    VbiProcessor proc;
    proc.begin(p, "", Rational{30000, 1001});
    v4l2_sliced_vbi_data d[2] = {};
    d[0].id = V4L2_SLICED_CAPTION_525;
    d[0].field = 0;
    d[0].line = 21;
    d[0].data[0] = withOddParity('H');
    d[0].data[1] = withOddParity('I');
    proc.process(reinterpret_cast<const uint8_t*>(d), sizeof d, 5);
    CHECK_EQ(proc.captionBytes(), 1u);
}

TEST_MAIN()
