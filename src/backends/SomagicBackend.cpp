// SPDX-License-Identifier: GPL-3.0-or-later
// Init and capture sequence ported from somagic-easycap (somagic-init.c,
// somagic-capture.c, somagic-audio-capture.c), GPL-2.0-or-later.
#include "SomagicBackend.h"

#include "V4l2Logic.h"
#include "engine/Clock.h"
#include "engine/SysInfo.h"

#include <libusb.h>
#include <linux/videodev2.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unistd.h>

namespace pd {

namespace {
constexpr uint16_t kVendor = 0x1c88;
constexpr uint16_t kUninit = 0x0007;
const uint16_t kReady[] = {0x003c, 0x003d, 0x003e, 0x003f};
constexpr int kTransfers = 8;
constexpr int kPackets = 64;
constexpr int kPacketSize = 3072;
constexpr uint8_t kVendorOut = uint8_t(LIBUSB_REQUEST_TYPE_VENDOR) | uint8_t(LIBUSB_RECIPIENT_DEVICE) | uint8_t(LIBUSB_ENDPOINT_OUT);
constexpr uint8_t kVendorIn = uint8_t(LIBUSB_REQUEST_TYPE_VENDOR) | uint8_t(LIBUSB_RECIPIENT_DEVICE) | uint8_t(LIBUSB_ENDPOINT_IN);

// SAA7113 analog input select values (subaddress 0x02, low bits).
constexpr int kInCvbsDc60 = 0;   // DC60 "CVBS" jack (VIDEO3)
constexpr int kInSvideo = 7;
const int kIn002[] = {2, 3, 0, 1};  // EasyCAP002 VIDEO1..4

enum : uint32_t {
    CtlBrightness = V4L2_CID_BRIGHTNESS,
    CtlContrast = V4L2_CID_CONTRAST,
    CtlSaturation = V4L2_CID_SATURATION,
    CtlHue = V4L2_CID_HUE,
    CtlLumaMode = V4L2_CID_USER_BASE + 0x1001,
    CtlLumaAperture = V4L2_CID_USER_BASE + 0x1002,
    CtlLumaPrefilter = V4L2_CID_USER_BASE + 0x1003,
};
}  // namespace

static std::string portString(libusb_device* d) {
    uint8_t ports[8];
    const int n = libusb_get_port_numbers(d, ports, 8);
    std::string s = std::to_string(libusb_get_bus_number(d)) + "-";
    for (int i = 0; i < n; ++i) s += (i ? "." : "") + std::to_string(ports[i]);
    return s;
}

std::vector<SomagicDeviceInfo> SomagicBackend::detect() {
    std::vector<SomagicDeviceInfo> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return out;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != kVendor) continue;
        bool ready = false;
        for (uint16_t p : kReady) ready |= d.idProduct == p;
        if (!ready && d.idProduct != kUninit) continue;
        SomagicDeviceInfo info;
        info.pid = d.idProduct;
        info.needsFirmware = !ready;
        info.bus = libusb_get_bus_number(list[i]);
        info.address = libusb_get_device_address(list[i]);
        info.port = portString(list[i]);
        out.push_back(info);
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

std::string SomagicBackend::firmwarePath() {
    return homeDir() + "/.local/share/pingdub/firmware/somagic_firmware.bin";
}

std::string SomagicBackend::udevRuleText() {
    return "# PingDub: let the logged-in user talk to Somagic EasyCaps through libusb\n"
           "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1c88\", ATTR{idProduct}==\"0007\", TAG+=\"uaccess\"\n"
           "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1c88\", ATTR{idProduct}==\"003c\", TAG+=\"uaccess\"\n"
           "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1c88\", ATTR{idProduct}==\"003d\", TAG+=\"uaccess\"\n"
           "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1c88\", ATTR{idProduct}==\"003e\", TAG+=\"uaccess\"\n"
           "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1c88\", ATTR{idProduct}==\"003f\", TAG+=\"uaccess\"\n";
}

bool SomagicBackend::installFirmwareFromDriver(const std::string& driverFile, std::string& err) {
    std::ifstream f(driverFile, std::ios::binary);
    if (!f) {
        err = "cannot read " + driverFile;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    SomagicFirmware fw;
    if (!somagicExtractFirmware(data, fw)) {
        err = "no known Somagic firmware found in " + driverFile + " (expected SmiUsbGrabber3C.sys from the Windows driver)";
        return false;
    }
    const std::string path = firmwarePath();
    makeDirs(path.substr(0, path.find_last_of('/')));
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(fw.data.data()), std::streamsize(fw.data.size()));
    if (!o) {
        err = "cannot write " + path;
        return false;
    }
    return true;
}

bool SomagicBackend::initDevice(std::string& err) {
    std::ifstream f(firmwarePath(), std::ios::binary);
    std::vector<uint8_t> fw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (fw.empty() || !somagicFirmwareValid(fw)) {
        err = "Somagic firmware missing or not recognized; extract it from the Windows driver first";
        return false;
    }
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        err = "libusb init failed";
        return false;
    }
    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, kVendor, kUninit);
    if (!h) {
        err = "device 1c88:0007 not found or no permission (install the udev rule)";
        libusb_exit(ctx);
        return false;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    bool ok = libusb_claim_interface(h, 0) == 0;
    ok = ok && libusb_set_interface_alt_setting(h, 0, 0) == 0;
    uint8_t buf[64];
    if (ok) {
        libusb_release_interface(h, 0);
        libusb_set_configuration(h, 1);
        ok = libusb_claim_interface(h, 0) == 0 && libusb_set_interface_alt_setting(h, 0, 0) == 0;
    }
    if (ok) {
        usleep(1000);
        libusb_control_transfer(h, kVendorIn, 0x01, 0x0001, 0x0000, buf, 2, 1000);
        for (size_t i = 0; i < fw.size(); i += 62) {
            buf[0] = 0x05;
            buf[1] = 0xff;
            const size_t n = std::min<size_t>(62, fw.size() - i);
            std::memset(buf + 2, 0, 62);
            std::memcpy(buf + 2, fw.data() + i, n);
            if (libusb_control_transfer(h, kVendorOut, 0x01, 0x0005, 0x0000, buf, 64, 1000) < 0) {
                ok = false;
                break;
            }
            usleep(1000);
        }
        buf[0] = 0x07;
        buf[1] = 0x00;
        libusb_control_transfer(h, kVendorOut, 0x01, 0x0007, 0x0000, buf, 2, 1000);
    }
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    if (!ok) {
        err = "firmware upload failed";
        return false;
    }
    // The device re-enumerates with a new product id.
    for (int i = 0; i < 50; ++i) {
        usleep(100000);
        for (const auto& d : detect())
            if (!d.needsFirmware) return true;
    }
    err = "device did not come back after firmware upload";
    return false;
}

SomagicBackend::SomagicBackend() = default;

SomagicBackend::~SomagicBackend() {
    stop();
    if (m_h) {
        libusb_release_interface(m_h, 0);
        libusb_close(m_h);
    }
    if (m_ctx) libusb_exit(m_ctx);
}

bool SomagicBackend::writeReg(uint16_t reg, uint8_t val) {
    uint8_t buf[8] = {0x0b, 0x00, 0x00, 0x82, 0x01, uint8_t(reg >> 8), uint8_t(reg & 0xff), val};
    return libusb_control_transfer(m_h, kVendorOut, 0x01, 0x000b, 0x0000, buf, 8, 1000) == 8;
}

bool SomagicBackend::writeI2c(uint8_t reg, uint8_t val) {
    uint8_t buf[8] = {0x0b, 0x4a, 0xc0, 0x01, 0x01, reg, val, 0xf4};
    return libusb_control_transfer(m_h, kVendorOut, 0x01, 0x000b, 0x0000, buf, 8, 1000) == 8;
}

bool SomagicBackend::open(const SomagicDeviceInfo& info, std::string& err) {
    if (info.needsFirmware) {
        err = "device needs firmware first (1c88:0007)";
        return false;
    }
    m_info = info;
    if (libusb_init(&m_ctx) != 0) {
        err = "libusb init failed";
        return false;
    }
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(m_ctx, &list);
    libusb_device* dev = nullptr;
    for (ssize_t i = 0; i < n; ++i)
        if (libusb_get_bus_number(list[i]) == info.bus && libusb_get_device_address(list[i]) == info.address) dev = list[i];
    int r = dev ? libusb_open(dev, &m_h) : LIBUSB_ERROR_NO_DEVICE;
    if (list) libusb_free_device_list(list, 1);
    if (r != 0) {
        err = std::string("cannot open Somagic device: ") + libusb_strerror(libusb_error(r));
        if (r == LIBUSB_ERROR_ACCESS) err += ". Install the udev rule (PingDub offers it in Diagnostics).";
        return false;
    }
    libusb_set_auto_detach_kernel_driver(m_h, 1);
    if (libusb_claim_interface(m_h, 0) != 0) {
        err = "cannot claim the device; another program has it";
        return false;
    }
    libusb_set_interface_alt_setting(m_h, 0, 0);
    uint8_t buf[2];
    libusb_release_interface(m_h, 0);
    libusb_set_configuration(m_h, 1);
    if (libusb_claim_interface(m_h, 0) != 0 || libusb_set_interface_alt_setting(m_h, 0, 0) != 0) {
        err = "cannot configure the device";
        return false;
    }
    libusb_control_transfer(m_h, kVendorIn, 0x01, 0x0001, 0x0000, buf, 2, 1000);
    m_std = V4L2_STD_NTSC_M;
    m_lines525 = true;
    m_input = 0;
    return true;
}

std::string SomagicBackend::deviceName() const {
    char b[96];
    std::snprintf(b, sizeof b, "Somagic EasyCap %s (1c88:%04x)", m_info.pid == 0x003c ? "DC60" : "SMI2021", m_info.pid);
    return b;
}

std::vector<VideoInputInfo> SomagicBackend::inputs() {
    return {{0, "Composite"}, {1, "S-Video"}, {2, "Composite 1 (EasyCAP002)"}, {3, "Composite 2 (EasyCAP002)"},
            {4, "Composite 3 (EasyCAP002)"}, {5, "Composite 4 (EasyCAP002)"}};
}

bool SomagicBackend::setInput(int index, std::string& err) {
    if (index < 0 || index > 5) {
        err = "no such input";
        return false;
    }
    m_input = index;
    if (m_h && m_run) programDecoder();
    return true;
}

std::vector<VideoStandardInfo> SomagicBackend::standards() {
    std::vector<VideoStandardInfo> v;
    auto add = [&](uint64_t id, const char* name, bool l525) {
        v.push_back({id, name, l525 ? Rational{30000, 1001} : Rational{25, 1}, l525});
    };
    add(V4L2_STD_NTSC_M, "NTSC-M", true);
    add(V4L2_STD_NTSC_443, "NTSC-443", true);
    add(V4L2_STD_PAL_M, "PAL-M", true);
    add(V4L2_STD_PAL_60, "PAL-60", true);
    add(V4L2_STD_PAL_BG | V4L2_STD_PAL_DK | V4L2_STD_PAL_I, "PAL-B/G/D/K/I", false);
    add(V4L2_STD_PAL_Nc, "PAL-Nc", false);
    add(V4L2_STD_SECAM, "SECAM", false);
    return v;
}

bool SomagicBackend::setStandard(uint64_t id, std::string& err) {
    m_std = id;
    m_lines525 = (id & V4L2_STD_525_60) != 0;
    if (m_h && m_run) programDecoder();
    return true;
}

std::vector<VideoFormatOption> SomagicBackend::formats() {
    return {{PixFmt::UYVY, 0, "UYVY 4:2:2 (BT.656 from the SAA7113)", true}};
}

bool SomagicBackend::configure(PixFmt pixfmt, VideoFormat& out, std::string& err) {
    if (pixfmt != PixFmt::UYVY) {
        err = "Somagic devices deliver UYVY only";
        return false;
    }
    out = VideoFormat{};
    out.pixfmt = PixFmt::UYVY;
    out.width = SomagicParser::kWidth;
    out.height = (m_lines525 ? 240 : 288) * 2;
    out.bytesPerLine = SomagicParser::kWidth * 2;
    out.frameBytes = size_t(out.bytesPerLine) * size_t(out.height);
    out.lines525 = m_lines525;
    out.fps = m_lines525 ? Rational{30000, 1001} : Rational{25, 1};
    // Frames are built with BT.656 field 0 first, on the top line.
    out.field = FieldOrder::TFF;
    out.sar = sarFor(out.width, m_lines525);
    out.standardName = describeStd(m_std).name;
    return true;
}

std::vector<VideoControl> SomagicBackend::controls() {
    auto num = [](uint32_t id, const char* name, int64_t mn, int64_t mx, int64_t def, int64_t val) {
        VideoControl c;
        c.id = id;
        c.name = name;
        c.minimum = mn;
        c.maximum = mx;
        c.defaultValue = def;
        c.value = val;
        return c;
    };
    std::vector<VideoControl> v;
    v.push_back(num(CtlBrightness, "Brightness", 0, 255, 128, m_brightness));
    v.push_back(num(CtlContrast, "Contrast", -128, 127, 71, m_contrast));
    v.push_back(num(CtlSaturation, "Saturation", -128, 127, 64, m_saturation));
    v.push_back(num(CtlHue, "Hue", -128, 127, 0, m_hue));
    VideoControl lm = num(CtlLumaMode, "Luma bandwidth (CVBS)", 0, 3, 0, m_lumaMode);
    lm.type = VideoControl::Type::Menu;
    lm.menu = {{0, "4.1 MHz"}, {1, "3.8 MHz"}, {2, "2.6 MHz"}, {3, "2.9 MHz"}};
    lm.privateControl = true;
    v.push_back(lm);
    VideoControl la = num(CtlLumaAperture, "Luma aperture", 0, 3, 1, m_lumaAperture);
    la.type = VideoControl::Type::Menu;
    la.menu = {{0, "0"}, {1, "0.25"}, {2, "0.5"}, {3, "1.0"}};
    la.privateControl = true;
    v.push_back(la);
    VideoControl lp = num(CtlLumaPrefilter, "Luma prefilter", 0, 1, 0, m_lumaPrefilter);
    lp.type = VideoControl::Type::Boolean;
    lp.privateControl = true;
    v.push_back(lp);
    return v;
}

bool SomagicBackend::setControl(uint32_t id, int64_t value, std::string& err) {
    switch (id) {
    case CtlBrightness: m_brightness = int(value); if (m_h) writeI2c(0x0a, uint8_t(m_brightness)); return true;
    case CtlContrast: m_contrast = int(value); if (m_h) writeI2c(0x0b, uint8_t(int8_t(m_contrast))); return true;
    case CtlSaturation: m_saturation = int(value); if (m_h) writeI2c(0x0c, uint8_t(int8_t(m_saturation))); return true;
    case CtlHue: m_hue = int(value); if (m_h) writeI2c(0x0d, uint8_t(int8_t(m_hue))); return true;
    case CtlLumaMode: m_lumaMode = int(value); break;
    case CtlLumaAperture: m_lumaAperture = int(value); break;
    case CtlLumaPrefilter: m_lumaPrefilter = int(value); break;
    default: err = "unknown control"; return false;
    }
    if (m_h) {
        const bool sv = m_input == 1;
        uint8_t w = uint8_t(((m_lumaPrefilter & 1) << 6) | ((m_lumaMode & 3) << 4) | (m_lumaAperture & 3));
        if (sv) w |= 0x80;
        writeI2c(0x09, w);
    }
    return true;
}

// Register program from somagic-capture.c's somagic_init().
bool SomagicBackend::programDecoder() {
    const bool svideo = m_input == 1;
    int sel = kInCvbsDc60;
    if (svideo) sel = kInSvideo;
    else if (m_input >= 2) sel = kIn002[m_input - 2];

    writeReg(0x3a, 0x80);
    writeReg(0x3b, 0x00);
    writeReg(0x34, 0x01);
    writeReg(0x35, 0x00);
    writeReg(0x34, 0x11);
    writeReg(0x35, 0x11);
    writeReg(0x3b, 0x80);
    writeReg(0x3b, 0x00);

    writeI2c(0x01, 0x08);
    writeI2c(0x02, uint8_t(0xc0 | sel));
    writeI2c(0x03, svideo ? 0x31 : 0x33);
    writeI2c(0x04, 0x00);
    writeI2c(0x05, 0x00);
    writeI2c(0x06, 0xe9);
    writeI2c(0x07, 0x0d);
    writeI2c(0x08, 0x98);
    uint8_t lum = uint8_t(((m_lumaPrefilter & 1) << 6) | ((m_lumaMode & 3) << 4) | (m_lumaAperture & 3));
    if (svideo) lum |= 0x80;
    writeI2c(0x09, lum);
    writeI2c(0x0a, uint8_t(m_brightness));
    writeI2c(0x0b, uint8_t(int8_t(m_contrast)));
    writeI2c(0x0c, uint8_t(int8_t(m_saturation)));
    writeI2c(0x0d, uint8_t(int8_t(m_hue)));
    uint8_t chroma = 0x01;  // NTSC-M or PAL-B/G/D/K/I
    if (m_std & V4L2_STD_PAL_60) chroma = 0x11;
    else if (m_std & V4L2_STD_NTSC_443) chroma = 0x21;
    else if (m_std & V4L2_STD_PAL_Nc) chroma = 0x21;
    else if (m_std & V4L2_STD_PAL_M) chroma = 0x31;
    else if (m_std & V4L2_STD_SECAM) chroma = 0x50;
    writeI2c(0x0e, chroma);
    writeI2c(0x0f, 0x2a);
    writeI2c(0x10, 0x40);
    writeI2c(0x11, 0x0c);
    writeI2c(0x12, 0x01);
    writeI2c(0x13, svideo ? 0x00 : 0x80);
    writeI2c(0x15, 0x00);
    writeI2c(0x16, 0x00);
    writeI2c(0x17, 0x00);
    writeI2c(0x40, m_lines525 ? 0x82 : 0x02);
    if (!svideo) {
        for (uint8_t r = 0x41; r <= 0x54; ++r) writeI2c(r, 0x77);
        writeI2c(0x55, 0xff);
    }
    writeI2c(0x58, 0x00);
    writeI2c(0x59, 0x54);
    writeI2c(0x5a, m_lines525 ? 0x0a : 0x07);
    writeI2c(0x5b, 0x83);
    writeI2c(0x5e, 0x00);
    return true;
}

static void LIBUSB_CALL transferCb(libusb_transfer* t) { static_cast<SomagicBackend*>(t->user_data)->onTransfer(t); }

bool SomagicBackend::start(int /*bufferCount*/, std::string& err) {
    if (!m_h) {
        err = "not open";
        return false;
    }
    if (m_run) return true;
    m_parser.reset(m_lines525);
    m_parser.onFrame = [this](const uint8_t* f, size_t n) {
        QFrame q;
        q.data.assign(f, f + n);
        q.ts = monotonicNs();
        q.seq = m_seq++;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_frames.push_back(std::move(q));
        while (m_frames.size() > 16) m_frames.pop_front();
        m_cvVideo.notify_one();
    };
    m_parser.onAudio = [this](const uint8_t* p, size_t n) {
        AudioChunk c;
        const size_t frames = n / SomagicParser::kAudioFrameBytes;
        c.samples = std::make_shared<std::vector<int32_t>>(frames * 2);
        std::memcpy(c.samples->data(), p, frames * 8);
        c.frames = uint32_t(frames);
        // Stamped on arrival; the sync engine's slow loop absorbs USB jitter.
        c.tsNs = monotonicNs() - int64_t(double(frames) * 1e9 / 48000.0);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_audioQ.push_back(std::move(c));
        while (m_audioQ.size() > 2000) m_audioQ.pop_front();
        m_cvAudio.notify_one();
    };
    programDecoder();
    writeReg(0x1740, 0x40);
    writeReg(0x1740, 0x00);
    usleep(250 * 1000);
    writeReg(0x1740, 0x00);
    uint8_t mode[2] = {0x01, 0x05};
    libusb_control_transfer(m_h, kVendorOut, 0x01, 0x0001, 0x0000, mode, 2, 1000);
    if (libusb_set_interface_alt_setting(m_h, 0, 2) != 0) {
        err = "cannot select the isochronous alternate setting";
        return false;
    }
    // 0x1d: CS5340 audio on, 32-bit little-endian stereo at 48 kHz, sent as
    // [aa aa 00 01] blocks between the video blocks.
    writeReg(0x1740, 0x1d);
    usleep(30 * 1000);

    m_isoBufs.assign(kTransfers, std::vector<uint8_t>(size_t(kPackets) * kPacketSize));
    for (int i = 0; i < kTransfers; ++i) {
        libusb_transfer* t = libusb_alloc_transfer(kPackets);
        libusb_fill_iso_transfer(t, m_h, 0x82, m_isoBufs[size_t(i)].data(), kPackets * kPacketSize, kPackets, transferCb,
                                 this, 2000);
        libusb_set_iso_packet_lengths(t, kPacketSize);
        m_transfers.push_back(t);
    }
    m_run = true;
    m_gone = false;
    for (libusb_transfer* t : m_transfers) {
        if (libusb_submit_transfer(t) == 0) ++m_pending;
    }
    if (!m_pending) {
        m_run = false;
        freeTransfers();
        err = "could not submit isochronous transfers";
        return false;
    }
    writeReg(0x1800, 0x0d);  // start streaming
    m_thread = std::thread([this] { eventLoop(); });
    return true;
}

void SomagicBackend::onTransfer(libusb_transfer* t) {
    --m_pending;
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        m_gone = true;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_error = "Somagic device disconnected";
        m_cvVideo.notify_all();
        m_cvAudio.notify_all();
        return;
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; ++i) {
            const auto& d = t->iso_packet_desc[i];
            if (d.status != LIBUSB_TRANSFER_COMPLETED || !d.actual_length) continue;
            m_parser.feedPacket(libusb_get_iso_packet_buffer_simple(t, unsigned(i)), d.actual_length);
        }
    }
    if (m_run && !m_gone && libusb_submit_transfer(t) == 0) ++m_pending;
}

void SomagicBackend::eventLoop() {
    timeval tv{0, 100000};
    while (m_run || m_pending > 0) {
        libusb_handle_events_timeout(m_ctx, &tv);
        if (!m_run && m_pending > 0) {
            for (libusb_transfer* t : m_transfers) libusb_cancel_transfer(t);
        }
        if (m_gone && !m_pending) break;
    }
}

void SomagicBackend::freeTransfers() {
    for (libusb_transfer* t : m_transfers) libusb_free_transfer(t);
    m_transfers.clear();
}

void SomagicBackend::stop() {
    if (!m_run && !m_thread.joinable()) return;
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
    freeTransfers();
    if (m_h && !m_gone) {
        writeReg(0x1800, 0x00);
        libusb_set_interface_alt_setting(m_h, 0, 0);
    }
}

int SomagicBackend::dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cvVideo.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !m_frames.empty() || m_gone; });
    if (m_gone && m_frames.empty()) {
        err = m_error;
        return -1;
    }
    if (m_frames.empty()) return 0;
    QFrame q = std::move(m_frames.front());
    m_frames.pop_front();
    lk.unlock();
    out.data = pool.acquire(q.data.size());
    std::memcpy(out.data->data(), q.data.data(), q.data.size());
    out.size = q.data.size();
    out.tsNs = q.ts;
    out.seq = q.seq;
    out.flags = 0;
    return 1;
}

int SomagicBackend::readAudio(AudioChunk& out, int timeoutMs, std::string& err) {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cvAudio.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !m_audioQ.empty() || m_gone; });
    if (m_gone && m_audioQ.empty()) {
        err = m_error;
        return -1;
    }
    if (m_audioQ.empty()) return 0;
    out = std::move(m_audioQ.front());
    m_audioQ.pop_front();
    return int(out.frames);
}

bool SomagicAudio::open(int, AudioFormat& out, std::string&) {
    out.rate = 48000;
    out.channels = 2;
    out.fmt = SampleFmt::S32;
    return true;
}

int SomagicAudio::read(AudioChunk& out, int timeoutMs, bool& xrun, std::string& err) {
    xrun = false;
    return m_dev->readAudio(out, timeoutMs, err);
}

std::string SomagicBackend::profileKey() const {
    char b[64];
    std::snprintf(b, sizeof b, "usb-1c88:%04x-port-%s", m_info.pid, m_info.port.c_str());
    return b;
}

std::string SomagicBackend::diagnostics() {
    std::ostringstream o;
    o << deviceName() << " at USB " << m_info.port << " (EXPERIMENTAL userspace backend, untested on real hardware)\n";
    o << "  firmware: " << (fileExists(firmwarePath()) ? firmwarePath() : std::string("not installed")) << "\n";
    o << "  frames parsed: " << m_parser.frames() << ", bad blocks: " << m_parser.badBlocks()
      << ", audio bytes: " << m_parser.audioBytes() << "\n";
    return o.str();
}

}  // namespace pd
