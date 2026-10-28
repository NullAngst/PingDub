// SPDX-License-Identifier: GPL-3.0-or-later
// V4L2 decisions and the lossless repacks behind them.
#include "backends/V4l2Logic.h"
#include "engine/PixelConvert.h"
#include "testing.h"

#include <linux/videodev2.h>

#include <cstring>
#include <vector>

using namespace pd;

TEST_CASE("pixel format preference") {
    CHECK(choosePixFmt({PixFmt::MJPEG, PixFmt::YUYV}) == PixFmt::YUYV);
    CHECK(choosePixFmt({PixFmt::UYVY, PixFmt::YUYV}) == PixFmt::YUYV);
    CHECK(choosePixFmt({PixFmt::YUYV, PixFmt::Y210}) == PixFmt::Y210);
    CHECK(choosePixFmt({PixFmt::NV12, PixFmt::HM12, PixFmt::MJPEG}) == PixFmt::HM12);
    CHECK(choosePixFmt({PixFmt::MJPEG}) == PixFmt::MJPEG);
    CHECK(choosePixFmt({}) == PixFmt::Unknown);
    CHECK(pixFmtFromFourcc(V4L2_PIX_FMT_YUYV) == PixFmt::YUYV);
    CHECK(pixFmtFromFourcc(v4l2_fourcc('H', 'M', '1', '2')) == PixFmt::HM12);
    CHECK(fourccString(V4L2_PIX_FMT_UYVY) == "UYVY");
}

TEST_CASE("field order from the driver's field type") {
    CHECK(fieldFromV4l2(V4L2_FIELD_INTERLACED_TB, true).order == FieldOrder::TFF);
    CHECK(fieldFromV4l2(V4L2_FIELD_INTERLACED_BT, false).order == FieldOrder::BFF);
    // Plain INTERLACED follows the standard: BFF for NTSC, TFF otherwise.
    CHECK(fieldFromV4l2(V4L2_FIELD_INTERLACED, true).order == FieldOrder::BFF);
    CHECK(fieldFromV4l2(V4L2_FIELD_INTERLACED, false).order == FieldOrder::TFF);
    const FieldDecision seq = fieldFromV4l2(V4L2_FIELD_SEQ_BT, true);
    CHECK(seq.weave && !seq.topFieldFirstInBuffer && seq.order == FieldOrder::BFF);
    CHECK(!fieldFromV4l2(V4L2_FIELD_ALTERNATE, true).supported);
    CHECK(fieldFromV4l2(V4L2_FIELD_NONE, true).order == FieldOrder::Progressive);
}

TEST_CASE("the upstream gvusb2 driver's field type is corrected") {
    CHECK(correctedField("gvusb2", V4L2_FIELD_INTERLACED) == V4L2_FIELD_INTERLACED_TB);
    CHECK(fieldFromV4l2(correctedField("gvusb2", V4L2_FIELD_INTERLACED), true).order == FieldOrder::TFF);
    // The fixed driver already says TB; other drivers are left alone.
    CHECK(correctedField("gvusb2", V4L2_FIELD_INTERLACED_TB) == V4L2_FIELD_INTERLACED_TB);
    CHECK(correctedField("em28xx", V4L2_FIELD_INTERLACED) == V4L2_FIELD_INTERLACED);
}

TEST_CASE("detected standard: only answers that narrow it down") {
    CHECK(detectedStdName(V4L2_STD_NTSC_M) == "NTSC-M");
    CHECK(detectedStdName(V4L2_STD_NTSC_M | V4L2_STD_NTSC_M_JP) == "NTSC");
    CHECK(detectedStdName(V4L2_STD_PAL) == "PAL");
    CHECK(detectedStdName(V4L2_STD_PAL_M) == "PAL-M");
    CHECK(detectedStdName(V4L2_STD_SECAM) == "SECAM");
    // No signal, still detecting, or "anything I support".
    CHECK(detectedStdName(V4L2_STD_UNKNOWN).empty());
    CHECK(detectedStdName(V4L2_STD_525_60).empty());
    CHECK(detectedStdName(V4L2_STD_625_50).empty());
    CHECK(detectedStdName(V4L2_STD_ALL).empty());
}

TEST_CASE("pixel aspect from sampling") {
    CHECK(sarFor(720, true) == Rational(10, 11));
    CHECK(sarFor(704, true) == Rational(10, 11));
    CHECK(sarFor(720, false) == Rational(12, 11));
    CHECK(sarFor(640, true) == Rational(1, 1));
    CHECK(sarFor(768, false) == Rational(1, 1));
    CHECK(sarFor(720, true, Rational{8, 9}) == Rational(8, 9));
}

TEST_CASE("standards") {
    CHECK(describeStd(V4L2_STD_NTSC_M).name == "NTSC-M");
    CHECK(describeStd(V4L2_STD_NTSC_M).lines525);
    CHECK(describeStd(V4L2_STD_NTSC_M_JP).name == "NTSC-J");
    CHECK(describeStd(V4L2_STD_PAL_M).lines525);
    CHECK(describeStd(V4L2_STD_PAL_60).lines525);
    CHECK(!describeStd(V4L2_STD_PAL_BG).lines525);
    CHECK(describeStd(V4L2_STD_PAL_BG).fps == Rational(25, 1));
    CHECK(!describeStd(V4L2_STD_SECAM_L).lines525);
    CHECK(describeStd(V4L2_STD_NTSC_443).name == "NTSC-443");
}

TEST_CASE("frame size: crop bounds win, 486 over 480, never scaled") {
    CHECK(chooseFrameSize({720, 486}, {{720, 480}}, true) == Size({720, 486}));
    CHECK(chooseFrameSize({}, {{720, 480}, {720, 486}, {360, 240}}, true) == Size({720, 486}));
    CHECK(chooseFrameSize({}, {{720, 576}, {360, 288}, {720, 480}}, false) == Size({720, 576}));
    CHECK(chooseFrameSize({}, {}, true) == Size({720, 480}));
    CHECK(chooseFrameSize({768, 576}, {}, false) == Size({768, 576}));
}

TEST_CASE("YUYV and UYVY repack losslessly") {
    const int w = 8, h = 2;
    std::vector<uint8_t> yuyv(size_t(w) * 2 * h);
    for (size_t i = 0; i < yuyv.size(); ++i) yuyv[i] = uint8_t(i * 7 + 3);
    std::vector<uint8_t> y(size_t(w) * h), u(size_t(w / 2) * h), v(size_t(w / 2) * h);
    yuyvToPlanar(yuyv.data(), w * 2, w, h, {y.data(), u.data(), v.data(), w, w / 2, w / 2});
    CHECK_EQ(y[0], yuyv[0]);
    CHECK_EQ(u[0], yuyv[1]);
    CHECK_EQ(y[1], yuyv[2]);
    CHECK_EQ(v[0], yuyv[3]);
    CHECK_EQ(y[w + 2], yuyv[size_t(w) * 2 + 4]);
    // UYVY is the same bytes in another order
    std::vector<uint8_t> uyvy(yuyv.size());
    for (size_t i = 0; i < yuyv.size(); i += 4) {
        uyvy[i] = yuyv[i + 1];
        uyvy[i + 1] = yuyv[i];
        uyvy[i + 2] = yuyv[i + 3];
        uyvy[i + 3] = yuyv[i + 2];
    }
    std::vector<uint8_t> y2(y.size()), u2(u.size()), v2(v.size());
    uyvyToPlanar(uyvy.data(), w * 2, w, h, {y2.data(), u2.data(), v2.data(), w, w / 2, w / 2});
    CHECK(y == y2 && u == u2 && v == v2);
}

TEST_CASE("v210 unpacks to 10-bit planes") {
    // One 6-pixel group: Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5
    const uint16_t Y[6] = {64, 100, 200, 300, 400, 940}, Cb[3] = {512, 10, 1000}, Cr[3] = {600, 700, 800};
    auto word = [](uint32_t a, uint32_t b, uint32_t c) { return a | (b << 10) | (c << 20); };
    uint32_t words[4] = {word(Cb[0], Y[0], Cr[0]), word(Y[1], Cb[1], Y[2]), word(Cr[1], Y[3], Cb[2]), word(Y[4], Cr[2], Y[5])};
    std::vector<uint8_t> row(size_t(v210Stride(6)), 0);
    std::memcpy(row.data(), words, 16);
    uint16_t y[6], u[3], v[3];
    v210ToPlanar10(row.data(), v210Stride(6), 6, 1, {y, u, v, 6, 3, 3});
    bool ok = true;
    for (int i = 0; i < 6; ++i) ok &= y[i] == Y[i];
    for (int i = 0; i < 3; ++i) ok &= u[i] == Cb[i] && v[i] == Cr[i];
    CHECK(ok);
    CHECK_EQ(v210Stride(720), 1920);
}

TEST_CASE("Y210 keeps the top 10 bits") {
    const uint16_t words[4] = {uint16_t(940 << 6), uint16_t(512 << 6), uint16_t(64 << 6), uint16_t(1 << 6)};
    uint16_t y[2], u[1], v[1];
    y210ToPlanar10(reinterpret_cast<const uint8_t*>(words), 8, 2, 1, {y, u, v, 2, 1, 1});
    CHECK(y[0] == 940 && u[0] == 512 && y[1] == 64 && v[0] == 1);
}

TEST_CASE("HM12 detiles to planar 4:2:0") {
    const int w = 32, h = 32;  // 2x2 luma tiles, 2x1 chroma tiles
    std::vector<uint8_t> hm(hm12Size(w, h));
    // Build the tiled buffer from a known planar image.
    std::vector<uint8_t> Y(size_t(w) * h), U(size_t(w / 2) * (h / 2)), V(U.size());
    for (size_t i = 0; i < Y.size(); ++i) Y[i] = uint8_t(i * 13);
    for (size_t i = 0; i < U.size(); ++i) {
        U[i] = uint8_t(i * 5 + 1);
        V[i] = uint8_t(i * 3 + 2);
    }
    size_t o = 0;
    for (int ty = 0; ty < h; ty += 16)
        for (int tx = 0; tx < w; tx += 16)
            for (int i = 0; i < 16; ++i)
                for (int j = 0; j < 16; ++j) hm[o++] = Y[size_t(ty + i) * w + size_t(tx + j)];
    for (int ty = 0; ty < h / 2; ty += 16)
        for (int tx = 0; tx < w; tx += 16)
            for (int i = 0; i < 16; ++i)
                for (int j = 0; j < 8; ++j) {
                    const int row = ty + i;
                    const int cx = tx / 2 + j;
                    hm[o++] = row < h / 2 ? U[size_t(row) * (w / 2) + size_t(cx)] : 0;
                    hm[o++] = row < h / 2 ? V[size_t(row) * (w / 2) + size_t(cx)] : 0;
                }
    std::vector<uint8_t> y(Y.size()), u(U.size()), v(V.size());
    hm12ToPlanar(hm.data(), w, h, {y.data(), u.data(), v.data(), w, w / 2, w / 2});
    CHECK(y == Y);
    CHECK(u == U);
    CHECK(v == V);
}

TEST_CASE("sequential fields weave into one frame") {
    const int lineBytes = 4, h = 4;
    // field buffer: top lines T0 T1 then bottom lines B0 B1
    std::vector<uint8_t> src = {'T', '0', 0, 0, 'T', '1', 0, 0, 'B', '0', 0, 0, 'B', '1', 0, 0};
    std::vector<uint8_t> dst(16);
    weaveFields(src.data(), lineBytes, h, true, dst.data());
    CHECK(dst[0] == 'T' && dst[1] == '0' && dst[4] == 'B' && dst[5] == '0' && dst[8] == 'T' && dst[9] == '1');
    weaveFields(src.data(), lineBytes, h, false, dst.data());
    CHECK(dst[0] == 'B' && dst[4] == 'T');
}

TEST_MAIN()
