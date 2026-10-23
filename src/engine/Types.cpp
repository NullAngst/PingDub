// SPDX-License-Identifier: GPL-3.0-or-later
#include "Types.h"

namespace pd {

const char* pixFmtName(PixFmt f) {
    switch (f) {
    case PixFmt::YUYV: return "YUYV";
    case PixFmt::UYVY: return "UYVY";
    case PixFmt::Y210: return "Y210";
    case PixFmt::V210: return "v210";
    case PixFmt::HM12: return "HM12";
    case PixFmt::NV12: return "NV12";
    case PixFmt::GREY: return "GREY";
    case PixFmt::MJPEG: return "MJPEG";
    default: return "unknown";
    }
}

bool pixFmtIs10Bit(PixFmt f) { return f == PixFmt::Y210 || f == PixFmt::V210; }
bool pixFmtIs420(PixFmt f) { return f == PixFmt::HM12 || f == PixFmt::NV12; }

const char* fieldOrderName(FieldOrder f) {
    switch (f) {
    case FieldOrder::Progressive: return "progressive";
    case FieldOrder::TFF: return "TFF";
    case FieldOrder::BFF: return "BFF";
    default: return "unknown";
    }
}

int sampleFmtBits(SampleFmt f) {
    switch (f) {
    case SampleFmt::S16: return 16;
    case SampleFmt::S24: return 24;
    default: return 32;
    }
}

const char* sampleFmtName(SampleFmt f) {
    switch (f) {
    case SampleFmt::S16: return "S16_LE";
    case SampleFmt::S24: return "S24_LE";
    default: return "S32_LE";
    }
}

const char* syncModeName(SyncMode m) {
    switch (m) {
    case SyncMode::VideoMaster: return "video-master";
    case SyncMode::AudioMaster: return "audio-master";
    default: return "no-correction";
    }
}

}  // namespace pd
