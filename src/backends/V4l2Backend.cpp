// SPDX-License-Identifier: GPL-3.0-or-later
#include "V4l2Backend.h"

#include "engine/Clock.h"
#include "engine/PixelConvert.h"
#include "engine/SysInfo.h"
#include "engine/VbiCapture.h"

#include <linux/videodev2.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace pd {

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

V4l2Backend::V4l2Backend() = default;

V4l2Backend::~V4l2Backend() { close(); }

bool V4l2Backend::open(const std::string& path, std::string& err) {
    close();
    m_fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        const int e = errno;
        err = path + ": " + std::strerror(e);
        if (e == EACCES || e == EPERM) {
            err += ". You need read/write access to the device: add yourself to the video group "
                   "(sudo usermod -aG video $USER, then log out and in).";
        } else if (e == EBUSY) {
            err += ". Busy";
            for (const auto& p : processesHolding(path)) err += ", held by " + p.name + " (pid " + std::to_string(p.pid) + ")";
        }
        return false;
    }
    v4l2_capability cap{};
    if (xioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        err = path + ": not a V4L2 device";
        close();
        return false;
    }
    m_path = path;
    m_card = reinterpret_cast<const char*>(cap.card);
    m_driver = reinterpret_cast<const char*>(cap.driver);
    m_bus = reinterpret_cast<const char*>(cap.bus_info);
    m_caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(m_caps & V4L2_CAP_VIDEO_CAPTURE)) {
        err = path + " (" + m_card + ") is not a video capture node";
        close();
        return false;
    }
    if (!(m_caps & V4L2_CAP_STREAMING)) {
        // read() mode has no per-frame timestamps or sequence numbers.
        err = path + " (" + m_card + ") does not support streaming I/O, which PingDub needs for timestamps";
        close();
        return false;
    }
    return true;
}

void V4l2Backend::close() {
    stop();
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

std::vector<VideoInputInfo> V4l2Backend::inputs() {
    std::vector<VideoInputInfo> out;
    for (uint32_t i = 0; m_fd >= 0; ++i) {
        v4l2_input in{};
        in.index = i;
        if (xioctl(m_fd, VIDIOC_ENUMINPUT, &in) < 0) break;
        out.push_back({int(i), reinterpret_cast<const char*>(in.name)});
    }
    return out;
}

int V4l2Backend::currentInput() {
    int i = 0;
    if (m_fd < 0 || xioctl(m_fd, VIDIOC_G_INPUT, &i) < 0) return 0;
    return i;
}

bool V4l2Backend::setInput(int index, std::string& err) {
    int i = index;
    if (xioctl(m_fd, VIDIOC_S_INPUT, &i) < 0) {
        err = "VIDIOC_S_INPUT: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

std::vector<VideoStandardInfo> V4l2Backend::standards() {
    std::vector<VideoStandardInfo> out;
    for (uint32_t i = 0; m_fd >= 0; ++i) {
        v4l2_standard s{};
        s.index = i;
        if (xioctl(m_fd, VIDIOC_ENUMSTD, &s) < 0) break;
        VideoStandardInfo v;
        v.id = s.id;
        v.name = reinterpret_cast<const char*>(s.name);
        v.lines525 = s.framelines == 525 || (s.framelines == 0 && describeStd(s.id).lines525);
        if (s.frameperiod.numerator && s.frameperiod.denominator)
            v.fps = Rational{int(s.frameperiod.denominator), int(s.frameperiod.numerator)};
        else
            v.fps = describeStd(s.id).fps;
        out.push_back(v);
    }
    return out;
}

uint64_t V4l2Backend::currentStandard() {
    v4l2_std_id id = 0;
    if (m_fd < 0 || xioctl(m_fd, VIDIOC_G_STD, &id) < 0) return 0;
    return id;
}

bool V4l2Backend::setStandard(uint64_t id, std::string& err) {
    v4l2_std_id s = id;
    if (xioctl(m_fd, VIDIOC_S_STD, &s) < 0) {
        err = "VIDIOC_S_STD: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool V4l2Backend::detectStandard(std::string& name) {
    name.clear();
    v4l2_std_id id = 0;
    if (m_fd < 0 || xioctl(m_fd, VIDIOC_QUERYSTD, &id) < 0) return false;
    name = detectedStdName(id);
    return true;
}

std::vector<VideoFormatOption> V4l2Backend::formats() {
    std::vector<VideoFormatOption> out;
    for (uint32_t i = 0; m_fd >= 0; ++i) {
        v4l2_fmtdesc d{};
        d.index = i;
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(m_fd, VIDIOC_ENUM_FMT, &d) < 0) break;
        VideoFormatOption o;
        o.fourcc = d.pixelformat;
        o.pixfmt = pixFmtFromFourcc(d.pixelformat);
        o.description = std::string(reinterpret_cast<const char*>(d.description)) + " (" + fourccString(d.pixelformat) + ")";
        o.archival = o.pixfmt != PixFmt::MJPEG;
        if (o.pixfmt != PixFmt::Unknown) out.push_back(o);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const VideoFormatOption& a, const VideoFormatOption& b) { return pixFmtRank(a.pixfmt) < pixFmtRank(b.pixfmt); });
    return out;
}

bool V4l2Backend::configure(PixFmt pixfmt, VideoFormat& out, std::string& err) {
    if (m_fd < 0) {
        err = "device not open";
        return false;
    }
    const uint64_t stdId = currentStandard();
    bool lines525 = true;
    Rational fps{30000, 1001};
    std::string stdName = "unknown";
    if (stdId) {
        const StdInfo si = describeStd(stdId);
        lines525 = si.lines525;
        fps = si.fps;
        stdName = si.name;
    } else {
        // No standards (some UVC dongles): judge from the current format height.
        v4l2_format cur{};
        cur.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(m_fd, VIDIOC_G_FMT, &cur) == 0) lines525 = cur.fmt.pix.height <= 500;
        stdName = lines525 ? "525-line" : "625-line";
        fps = lines525 ? Rational{30000, 1001} : Rational{25, 1};
    }

    // 1. Crop to the full bounds so the driver trims nothing.
    Size bounds{};
    m_cropSummary.clear();
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_CROP_BOUNDS;
    if (xioctl(m_fd, VIDIOC_G_SELECTION, &sel) == 0) {
        bounds = {int(sel.r.width), int(sel.r.height)};
        v4l2_selection set{};
        set.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        set.target = V4L2_SEL_TGT_CROP;
        set.r = sel.r;
        const bool ok = xioctl(m_fd, VIDIOC_S_SELECTION, &set) == 0;
        char b[200];
        std::snprintf(b, sizeof b, "crop bounds %ux%u at %d,%d; crop %s %ux%u", sel.r.width, sel.r.height, sel.r.left,
                      sel.r.top, ok ? "set to" : "could not be set, driver kept", set.r.width, set.r.height);
        m_cropSummary = b;
    } else {
        v4l2_cropcap cc{};
        cc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(m_fd, VIDIOC_CROPCAP, &cc) == 0) {
            bounds = {int(cc.bounds.width), int(cc.bounds.height)};
            v4l2_crop c{};
            c.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            c.c = cc.bounds;
            const bool ok = xioctl(m_fd, VIDIOC_S_CROP, &c) == 0;
            char b[200];
            std::snprintf(b, sizeof b, "crop bounds %ux%u (CROPCAP); crop %s", cc.bounds.width, cc.bounds.height,
                          ok ? "set to bounds" : "not settable");
            m_cropSummary = b;
        } else {
            m_cropSummary = "driver has no crop control; frame is what the driver delivers";
        }
    }

    // 2. Largest unscaled size.
    const uint32_t fourcc = fourccFromPixFmt(pixfmt);
    std::vector<Size> offered;
    for (uint32_t i = 0;; ++i) {
        v4l2_frmsizeenum fs{};
        fs.index = i;
        fs.pixel_format = fourcc;
        if (xioctl(m_fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) break;
        if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) offered.push_back({int(fs.discrete.width), int(fs.discrete.height)});
        else {
            offered.push_back({int(fs.stepwise.max_width), int(fs.stepwise.max_height)});
            break;
        }
    }
    Size want = chooseFrameSize(bounds, offered, lines525);

    // 3. Set the format, interlaced, and read back what the driver chose.
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = uint32_t(want.w);
    f.fmt.pix.height = uint32_t(want.h);
    f.fmt.pix.pixelformat = fourcc;
    f.fmt.pix.field = V4L2_FIELD_INTERLACED;
    if (xioctl(m_fd, VIDIOC_S_FMT, &f) < 0) {
        err = "VIDIOC_S_FMT: " + std::string(std::strerror(errno));
        return false;
    }
    if (f.fmt.pix.pixelformat != fourcc) {
        err = "driver refused " + fourccString(fourcc) + " and offered " + fourccString(f.fmt.pix.pixelformat);
        return false;
    }
    const uint32_t field = correctedField(m_driver, f.fmt.pix.field);
    m_field = fieldFromV4l2(field, lines525);
    if (!m_field.supported) {
        err = std::string("driver delivers fields as ") + v4l2FieldName(f.fmt.pix.field) +
              ", which PingDub cannot weave losslessly yet";
        return false;
    }
    if (!m_cropSummary.empty()) m_cropSummary += "; ";
    char sz[160];
    std::snprintf(sz, sizeof sz, "format %ux%u %s, field %s", f.fmt.pix.width, f.fmt.pix.height,
                  fourccString(fourcc).c_str(), v4l2FieldName(f.fmt.pix.field));
    m_cropSummary += sz;
    if (field != f.fmt.pix.field)
        m_cropSummary += std::string(" (taken as ") + v4l2FieldName(field) +
                         ": this driver reports the wrong field type; NullAngst/GV-USB2-Driver fixes it)";
    if (bounds.w && (int(f.fmt.pix.width) != bounds.w || int(f.fmt.pix.height) != bounds.h))
        m_cropSummary += " (differs from crop bounds: the driver scales)";

    // 4. Frame rate from the standard, confirmed by G_PARM.
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_G_PARM, &parm) == 0) {
        const auto& tpf = parm.parm.capture.timeperframe;
        if (tpf.numerator && tpf.denominator) {
            const Rational r{int(tpf.denominator), int(tpf.numerator)};
            if (std::abs(r.toDouble() - fps.toDouble()) < 0.05) fps = r;
        }
    }

    m_fourcc = fourcc;
    m_fmt = VideoFormat{};
    m_fmt.pixfmt = pixfmt;
    m_fmt.width = int(f.fmt.pix.width);
    m_fmt.height = int(f.fmt.pix.height);
    m_fmt.bytesPerLine = int(f.fmt.pix.bytesperline ? f.fmt.pix.bytesperline : f.fmt.pix.width * 2);
    m_fmt.frameBytes = f.fmt.pix.sizeimage;
    if (pixfmt == PixFmt::HM12 && m_fmt.frameBytes < hm12Size(m_fmt.width, m_fmt.height))
        m_fmt.frameBytes = hm12Size(m_fmt.width, m_fmt.height);
    m_fmt.fps = fps;
    m_fmt.field = m_field.order;
    m_fmt.lines525 = lines525;
    m_fmt.sar = sarFor(m_fmt.width, lines525);
    m_fmt.standardName = stdName;
    out = m_fmt;
    return true;
}

std::vector<VideoControl> V4l2Backend::controls() {
    std::vector<VideoControl> out;
    if (m_fd < 0) return out;
    v4l2_query_ext_ctrl q{};
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    while (xioctl(m_fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        const uint32_t id = q.id;
        if (!(q.flags & V4L2_CTRL_FLAG_DISABLED) && q.type != V4L2_CTRL_TYPE_CTRL_CLASS) {
            VideoControl c;
            c.id = id;
            c.name = q.name;
            c.minimum = q.minimum;
            c.maximum = q.maximum;
            c.step = q.step ? int64_t(q.step) : 1;
            c.defaultValue = q.default_value;
            c.readOnly = q.flags & V4L2_CTRL_FLAG_READ_ONLY;
            c.inactive = q.flags & V4L2_CTRL_FLAG_INACTIVE;
            c.privateControl = V4L2_CTRL_DRIVER_PRIV(id) || (id & 0xffff) >= 0x1000;
            switch (q.type) {
            case V4L2_CTRL_TYPE_INTEGER: c.type = VideoControl::Type::Integer; break;
            case V4L2_CTRL_TYPE_BOOLEAN: c.type = VideoControl::Type::Boolean; break;
            case V4L2_CTRL_TYPE_MENU: c.type = VideoControl::Type::Menu; break;
            case V4L2_CTRL_TYPE_INTEGER_MENU: c.type = VideoControl::Type::IntegerMenu; break;
            case V4L2_CTRL_TYPE_BUTTON: c.type = VideoControl::Type::Button; break;
            case V4L2_CTRL_TYPE_INTEGER64: c.type = VideoControl::Type::Integer64; break;
            default: c.type = VideoControl::Type::Other; break;
            }
            if (c.type == VideoControl::Type::Menu || c.type == VideoControl::Type::IntegerMenu) {
                for (int64_t m = q.minimum; m <= q.maximum; ++m) {
                    v4l2_querymenu qm{};
                    qm.id = id;
                    qm.index = uint32_t(m);
                    if (xioctl(m_fd, VIDIOC_QUERYMENU, &qm) < 0) continue;
                    if (c.type == VideoControl::Type::Menu) c.menu.push_back({m, reinterpret_cast<const char*>(qm.name)});
                    else c.menu.push_back({m, std::to_string(qm.value)});
                }
            }
            if (c.type != VideoControl::Type::Button && c.type != VideoControl::Type::Other) {
                v4l2_ext_control ec{};
                ec.id = id;
                v4l2_ext_controls ecs{};
                ecs.which = V4L2_CTRL_WHICH_CUR_VAL;
                ecs.count = 1;
                ecs.controls = &ec;
                if (xioctl(m_fd, VIDIOC_G_EXT_CTRLS, &ecs) == 0)
                    c.value = c.type == VideoControl::Type::Integer64 ? ec.value64 : ec.value;
                else
                    c.value = c.defaultValue;
            }
            if (c.type != VideoControl::Type::Other) out.push_back(c);
        }
        q.id = id | V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    }
    return out;
}

bool V4l2Backend::setControl(uint32_t id, int64_t value, std::string& err) {
    v4l2_ext_control ec{};
    ec.id = id;
    ec.value64 = value;
    ec.value = int32_t(value);
    v4l2_ext_controls ecs{};
    ecs.which = V4L2_CTRL_WHICH_CUR_VAL;
    ecs.count = 1;
    ecs.controls = &ec;
    if (xioctl(m_fd, VIDIOC_S_EXT_CTRLS, &ecs) < 0) {
        v4l2_control c{};
        c.id = id;
        c.value = int32_t(value);
        if (xioctl(m_fd, VIDIOC_S_CTRL, &c) < 0) {
            err = std::strerror(errno);
            return false;
        }
    }
    return true;
}

SignalStatus V4l2Backend::signalStatus() {
    SignalStatus s;
    if (m_fd < 0) return s;
    v4l2_input in{};
    in.index = uint32_t(currentInput());
    if (xioctl(m_fd, VIDIOC_ENUMINPUT, &in) == 0) {
        s.known = true;
        s.noPower = in.status & V4L2_IN_ST_NO_POWER;
        s.noSignal = in.status & V4L2_IN_ST_NO_SIGNAL;
        s.noColor = in.status & V4L2_IN_ST_NO_COLOR;
        s.noHLock = in.status & V4L2_IN_ST_NO_H_LOCK;
    }
    // Many drivers answer QUERYSTD while streaming; some refuse with EBUSY.
    // Only a refusal falls back to what was detected before streaming
    // started. An answer of "can't tell" (no signal) means unknown now.
    if (!detectStandard(s.detected)) s.detected = m_detectedBeforeStart;
    return s;
}

bool V4l2Backend::start(int bufferCount, std::string& err) {
    if (m_streaming) return true;
    detectStandard(m_detectedBeforeStart);
    v4l2_requestbuffers rb{};
    rb.count = uint32_t(std::clamp(bufferCount, 2, 64));
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        err = "VIDIOC_REQBUFS: " + std::string(std::strerror(errno));
        return false;
    }
    m_maps.clear();
    for (uint32_t i = 0; i < rb.count; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(m_fd, VIDIOC_QUERYBUF, &b) < 0) {
            err = "VIDIOC_QUERYBUF failed";
            stop();
            return false;
        }
        void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, b.m.offset);
        if (p == MAP_FAILED) {
            err = "mmap failed: " + std::string(std::strerror(errno));
            stop();
            return false;
        }
        m_maps.push_back({p, b.length});
        if (xioctl(m_fd, VIDIOC_QBUF, &b) < 0) {
            err = "VIDIOC_QBUF failed";
            stop();
            return false;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        err = "VIDIOC_STREAMON: " + std::string(std::strerror(errno));
        stop();
        return false;
    }
    m_streaming = true;
    m_warnedClock = false;
    return true;
}

void V4l2Backend::stop() {
    if (m_fd >= 0 && m_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(m_fd, VIDIOC_STREAMOFF, &type);
    }
    m_streaming = false;
    for (auto& m : m_maps) munmap(m.ptr, m.len);
    m_maps.clear();
    if (m_fd >= 0) {
        v4l2_requestbuffers rb{};
        rb.count = 0;
        rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        rb.memory = V4L2_MEMORY_MMAP;
        xioctl(m_fd, VIDIOC_REQBUFS, &rb);
    }
}

int V4l2Backend::dequeue(VideoFrame& out, BufferPool& pool, int timeoutMs, std::string& err) {
    if (!m_streaming) {
        err = "not streaming";
        return -1;
    }
    pollfd pfd{m_fd, POLLIN, 0};
    const int pr = poll(&pfd, 1, timeoutMs);
    if (pr == 0) return 0;
    if (pr < 0) {
        if (errno == EINTR) return 0;
        err = std::string("poll: ") + std::strerror(errno);
        return -1;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        err = "device disconnected";
        return -1;
    }
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_DQBUF, &b) < 0) {
        if (errno == EAGAIN) return 0;
        err = errno == ENODEV ? "device disconnected" : std::string("VIDIOC_DQBUF: ") + std::strerror(errno);
        return -1;
    }
    const uint8_t* src = static_cast<const uint8_t*>(m_maps[b.index].ptr);
    const size_t used = b.bytesused ? b.bytesused : m_maps[b.index].len;
    const size_t n = std::min(used, m_maps[b.index].len);
    out.data = pool.acquire(std::max(n, m_fmt.frameBytes));
    if (m_field.weave && m_fmt.pixfmt != PixFmt::HM12 && m_fmt.pixfmt != PixFmt::NV12 && m_fmt.pixfmt != PixFmt::MJPEG &&
        n >= size_t(m_fmt.bytesPerLine) * m_fmt.height) {
        weaveFields(src, m_fmt.bytesPerLine, m_fmt.height, m_field.topFieldFirstInBuffer, out.data->data());
    } else {
        std::memcpy(out.data->data(), src, n);
    }
    out.size = n;
    out.seq = b.sequence;
    out.flags = (b.flags & V4L2_BUF_FLAG_ERROR) ? VF_Error : VF_None;
    const int64_t ts = int64_t(b.timestamp.tv_sec) * 1000000000LL + int64_t(b.timestamp.tv_usec) * 1000;
    if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC && ts > 0) {
        out.tsNs = ts;
        m_monotonic = true;
    } else {
        // No usable driver clock: stamp at dequeue. Less precise, still monotonic.
        out.tsNs = monotonicNs();
        m_monotonic = false;
    }
    xioctl(m_fd, VIDIOC_QBUF, &b);  // back to the driver at once
    return 1;
}

std::string V4l2Backend::vbiNode() const { return findVbiNodeFor(m_path); }

static std::string usbDirFor(const std::string& devPath) {
    const std::string name = devPath.substr(devPath.find_last_of('/') + 1);
    char real[PATH_MAX];
    const std::string link = "/sys/class/video4linux/" + name + "/device";
    if (!realpath(link.c_str(), real)) return {};
    std::string p = real;
    // Walk up to the USB device (the directory with idVendor).
    while (p.size() > 1) {
        if (fileExists(p + "/idVendor")) return p;
        const size_t s = p.find_last_of('/');
        if (s == std::string::npos) break;
        p = p.substr(0, s);
    }
    return {};
}

int V4l2Backend::usbSpeedMbps() const {
    const std::string dir = usbDirFor(m_path);
    if (dir.empty()) return 0;
    return std::atoi(readSmallFile(dir + "/speed").c_str());
}

bool V4l2Backend::hardwareId(bool& usb, uint16_t& vendor, uint16_t& product) const {
    const std::string dir = usbDirFor(m_path);
    if (!dir.empty()) {
        usb = true;
        vendor = uint16_t(std::strtoul(readSmallFile(dir + "/idVendor").c_str(), nullptr, 16));
        product = uint16_t(std::strtoul(readSmallFile(dir + "/idProduct").c_str(), nullptr, 16));
        return vendor != 0;
    }
    const std::string name = m_path.substr(m_path.find_last_of('/') + 1);
    char real[PATH_MAX];
    const std::string link = "/sys/class/video4linux/" + name + "/device";
    if (!realpath(link.c_str(), real)) return false;
    const std::string d = real;
    if (!fileExists(d + "/vendor")) return false;
    usb = false;
    vendor = uint16_t(std::strtoul(readSmallFile(d + "/vendor").c_str(), nullptr, 16));
    product = uint16_t(std::strtoul(readSmallFile(d + "/device").c_str(), nullptr, 16));
    return vendor != 0;
}

std::string V4l2Backend::profileKey() const {
    const std::string dir = usbDirFor(m_path);
    if (!dir.empty()) {
        const std::string vid = readSmallFile(dir + "/idVendor"), pid = readSmallFile(dir + "/idProduct");
        const std::string serial = readSmallFile(dir + "/serial");
        if (!serial.empty()) return "usb-" + vid + ":" + pid + "-" + serial;
        return "usb-" + vid + ":" + pid + "-port-" + dir.substr(dir.find_last_of('/') + 1);
    }
    if (m_bus.rfind("PCI:", 0) == 0 || m_bus.rfind("PCIe:", 0) == 0) return "pci-" + m_driver + "-" + m_bus.substr(m_bus.find(':') + 1);
    return "v4l2-" + m_driver + "-" + m_card;
}

std::string V4l2Backend::diagnostics() {
    std::ostringstream o;
    o << "Device " << m_path << "\n";
    o << "  Driver name   : " << m_driver << "\n  Card type     : " << m_card << "\n  Bus info      : " << m_bus << "\n";
    char caps[16];
    std::snprintf(caps, sizeof caps, "0x%08x", m_caps);
    o << "  Device caps   : " << caps;
    if (m_caps & V4L2_CAP_VIDEO_CAPTURE) o << " video-capture";
    if (m_caps & V4L2_CAP_VBI_CAPTURE) o << " vbi-capture";
    if (m_caps & V4L2_CAP_SLICED_VBI_CAPTURE) o << " sliced-vbi";
    if (m_caps & V4L2_CAP_AUDIO) o << " audio";
    if (m_caps & V4L2_CAP_TUNER) o << " tuner";
    if (m_caps & V4L2_CAP_READWRITE) o << " read/write";
    if (m_caps & V4L2_CAP_STREAMING) o << " streaming";
    o << "\n  Timestamps    : " << (m_monotonic ? "monotonic" : "not monotonic, stamped at dequeue") << "\n";
    const int sp = usbSpeedMbps();
    if (sp) o << "  USB speed     : " << sp << " Mbit/s\n";
    o << "Inputs:\n";
    const int curIn = currentInput();
    for (const auto& i : inputs()) o << "  " << i.index << ": " << i.name << (i.index == curIn ? "  (current)" : "") << "\n";
    o << "Standards:\n";
    const uint64_t cur = currentStandard();
    for (const auto& s : standards()) {
        char id[24];
        std::snprintf(id, sizeof id, "0x%016llx", (unsigned long long)s.id);
        o << "  " << s.name << "  " << id << "  " << s.fps.num << "/" << s.fps.den << (s.id == cur ? "  (current)" : "") << "\n";
    }
    {
        // Some drivers (the GV-USB2's among them) can only tell while streaming.
        std::string det;
        const bool answered = detectStandard(det);
        o << "  Detected      : "
          << (!answered ? "unknown (driver refuses QUERYSTD)"
                        : det.empty() ? (m_streaming ? "unknown (no signal, or still detecting)"
                                                     : "unknown (the driver may only tell while streaming)")
                                      : det)
          << "\n";
    }
    o << "Formats:\n";
    for (const auto& f : formats()) {
        o << "  " << f.description << (f.archival ? "" : "  NOT ARCHIVAL") << "\n";
        for (uint32_t i = 0;; ++i) {
            v4l2_frmsizeenum fs{};
            fs.index = i;
            fs.pixel_format = f.fourcc;
            if (xioctl(m_fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) break;
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) o << "      " << fs.discrete.width << "x" << fs.discrete.height << "\n";
            else {
                o << "      " << fs.stepwise.min_width << "x" << fs.stepwise.min_height << " to " << fs.stepwise.max_width
                  << "x" << fs.stepwise.max_height << "\n";
                break;
            }
        }
    }
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_G_FMT, &f) == 0) {
        o << "Current format: " << f.fmt.pix.width << "x" << f.fmt.pix.height << " " << fourccString(f.fmt.pix.pixelformat)
          << " field " << v4l2FieldName(f.fmt.pix.field) << " bytesperline " << f.fmt.pix.bytesperline << " sizeimage "
          << f.fmt.pix.sizeimage << "\n";
    }
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_CROP_BOUNDS;
    if (xioctl(m_fd, VIDIOC_G_SELECTION, &sel) == 0)
        o << "Crop bounds   : " << sel.r.width << "x" << sel.r.height << " at " << sel.r.left << "," << sel.r.top << "\n";
    sel.target = V4L2_SEL_TGT_CROP;
    if (xioctl(m_fd, VIDIOC_G_SELECTION, &sel) == 0)
        o << "Crop          : " << sel.r.width << "x" << sel.r.height << " at " << sel.r.left << "," << sel.r.top << "\n";
    if (!m_cropSummary.empty()) o << "Configured    : " << m_cropSummary << "\n";
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_G_PARM, &parm) == 0)
        o << "Frame period  : " << parm.parm.capture.timeperframe.numerator << "/"
          << parm.parm.capture.timeperframe.denominator << "\n";
    o << "Controls:\n";
    for (const auto& c : controls()) {
        o << "  " << c.name << " = " << c.value << "  (min " << c.minimum << ", max " << c.maximum << ", default "
          << c.defaultValue << ")" << (c.privateControl ? "  [driver]" : "") << (c.inactive ? "  [inactive]" : "") << "\n";
        for (const auto& m : c.menu) o << "      " << m.first << ": " << m.second << "\n";
    }
    const std::string vbi = vbiNode();
    o << "VBI node      : " << (vbi.empty() ? "none (no closed caption capture on this device)" : vbi) << "\n";
    return o.str();
}

}  // namespace pd
