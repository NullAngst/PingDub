// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeviceScanner.h"

#include "KnownDevices.h"
#include "backends/AlsaBackend.h"
#include "backends/DeckLinkBackend.h"
#include "backends/FakeBackends.h"
#include "backends/V4l2Backend.h"
#include "engine/SysInfo.h"

#include <QSocketNotifier>

#include <libudev.h>
#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace pd {

DeviceScanner::DeviceScanner(QObject* parent) : QObject(parent) {
    m_udev = udev_new();
    if (m_udev) {
        m_mon = udev_monitor_new_from_netlink(m_udev, "udev");
        if (m_mon) {
            udev_monitor_filter_add_match_subsystem_devtype(m_mon, "video4linux", nullptr);
            udev_monitor_filter_add_match_subsystem_devtype(m_mon, "sound", nullptr);
            udev_monitor_filter_add_match_subsystem_devtype(m_mon, "usb", "usb_device");
            udev_monitor_enable_receiving(m_mon);
            m_notifier = new QSocketNotifier(udev_monitor_get_fd(m_mon), QSocketNotifier::Read, this);
            connect(m_notifier, &QSocketNotifier::activated, this, [this] { onUdev(); });
        }
    }
    // Devices come up in bursts (video, audio, firmware re-enumeration):
    // wait for things to settle before rescanning.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(700);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        rescan();
        emit changed();
    });
}

DeviceScanner::~DeviceScanner() {
    if (m_mon) udev_monitor_unref(m_mon);
    if (m_udev) udev_unref(m_udev);
}

void DeviceScanner::onUdev() {
    while (udev_device* d = udev_monitor_receive_device(m_mon)) udev_device_unref(d);
    m_debounce.start();
}

void DeviceScanner::scanV4l2() {
    if (!m_udev) return;
    udev_enumerate* e = udev_enumerate_new(m_udev);
    udev_enumerate_add_match_subsystem(e, "video4linux");
    udev_enumerate_scan_devices(e);
    udev_list_entry* entry;
    std::vector<CaptureDevice> found;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
        udev_device* d = udev_device_new_from_syspath(m_udev, udev_list_entry_get_name(entry));
        if (!d) continue;
        const char* node = udev_device_get_devnode(d);
        if (node && std::strncmp(udev_device_get_sysname(d), "video", 5) == 0) {
            const int fd = ::open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                v4l2_capability cap{};
                if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
                    if ((caps & V4L2_CAP_VIDEO_CAPTURE) && !(caps & V4L2_CAP_META_CAPTURE)) {
                        CaptureDevice c;
                        c.kind = CaptureDevice::Kind::V4L2;
                        c.node = node;
                        c.id = std::string("v4l2:") + node;
                        c.name = std::string(reinterpret_cast<const char*>(cap.card)) + " (" + node + ")";
                        c.driver = reinterpret_cast<const char*>(cap.driver);
                        c.busInfo = reinterpret_cast<const char*>(cap.bus_info);
                        c.pairedAudio = AlsaBackend::pairedPcmFor(node);
                        found.push_back(c);
                    }
                }
                ::close(fd);
            } else if (errno == EACCES) {
                CaptureDevice c;
                c.kind = CaptureDevice::Kind::V4L2;
                c.node = node;
                c.id = std::string("v4l2:") + node;
                c.name = std::string(node) + " (no permission)";
                found.push_back(c);
            }
        }
        udev_device_unref(d);
    }
    udev_enumerate_unref(e);
    std::sort(found.begin(), found.end(), [](const CaptureDevice& a, const CaptureDevice& b) { return a.node < b.node; });
    for (auto& c : found) m_devices.push_back(c);
}

static uint16_t hexAttr(udev_device* d, const char* attr) {
    const char* v = udev_device_get_sysattr_value(d, attr);
    return v ? uint16_t(std::strtoul(v, nullptr, 16)) : 0;
}

void DeviceScanner::scanMissing() {
    m_missing.clear();
    m_report.clear();
    if (!m_udev) return;
    for (const char* subsystem : {"usb", "pci"}) {
        udev_enumerate* e = udev_enumerate_new(m_udev);
        udev_enumerate_add_match_subsystem(e, subsystem);
        udev_enumerate_scan_devices(e);
        udev_list_entry* entry;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
            udev_device* d = udev_device_new_from_syspath(m_udev, udev_list_entry_get_name(entry));
            if (!d) continue;
            const bool usb = std::strcmp(subsystem, "usb") == 0;
            const char* devtype = udev_device_get_devtype(d);
            if (usb && (!devtype || std::strcmp(devtype, "usb_device") != 0)) {
                udev_device_unref(d);
                continue;
            }
            const uint16_t vid = usb ? hexAttr(d, "idVendor") : hexAttr(d, "vendor");
            const uint16_t pid = usb ? hexAttr(d, "idProduct") : hexAttr(d, "device");
            const KnownDevice* k = lookupKnownDevice(usb ? KnownDevice::Bus::USB : KnownDevice::Bus::PCI, vid, pid);
            if (!k) {
                udev_device_unref(d);
                continue;
            }
            // Bound drivers: the USB interfaces, or the PCI function itself.
            std::vector<std::string> bound;
            if (usb) {
                udev_enumerate* ie = udev_enumerate_new(m_udev);
                udev_enumerate_add_match_parent(ie, d);
                udev_enumerate_add_match_subsystem(ie, "usb");
                udev_enumerate_scan_devices(ie);
                udev_list_entry* ient;
                udev_list_entry_foreach(ient, udev_enumerate_get_list_entry(ie)) {
                    udev_device* id = udev_device_new_from_syspath(m_udev, udev_list_entry_get_name(ient));
                    if (!id) continue;
                    const char* dt = udev_device_get_devtype(id);
                    const char* drv = udev_device_get_driver(id);
                    if (dt && std::strcmp(dt, "usb_interface") == 0 && drv) bound.push_back(drv);
                    udev_device_unref(id);
                }
                udev_enumerate_unref(ie);
            } else if (const char* drv = udev_device_get_driver(d)) {
                bound.push_back(drv);
            }
            char ids[32];
            std::snprintf(ids, sizeof ids, "%s %04x:%04x", usb ? "USB" : "PCI", vid, pid);
            std::string line = std::string(ids) + "  " + k->name + "  drivers: ";
            if (bound.empty()) line += "none";
            for (size_t i = 0; i < bound.size(); ++i) line += (i ? ", " : "") + bound[i];
            if (usb) line += std::string("  at ") + udev_device_get_sysname(d);
            m_report.push_back(line);

            // Somagic is driven from userspace, DeckLink through its own API.
            const bool userspace = vid == 0x1c88 || vid == 0xbdbd;
            bool ok = userspace;
            if (!ok) {
                auto norm = [](std::string x) {
                    for (char& ch : x)
                        if (ch == '-') ch = '_';
                    return x;
                };
                const std::string want = norm(k->drivers);
                if (want.empty()) ok = !bound.empty();
                for (const auto& b : bound)
                    if (!want.empty() && want.find(norm(b)) != std::string::npos) ok = true;
            }
            if (!ok) m_missing.push_back({k->name, ids, missingDriverMessage(*k)});
            udev_device_unref(d);
        }
        udev_enumerate_unref(e);
    }
}

void DeviceScanner::rescan() {
    m_devices.clear();
    scanV4l2();
    for (const SomagicDeviceInfo& s : SomagicBackend::detect()) {
        CaptureDevice c;
        c.kind = CaptureDevice::Kind::Somagic;
        c.id = "somagic:" + s.port;
        char b[96];
        std::snprintf(b, sizeof b, "Somagic EasyCap 1c88:%04x at USB %s [experimental]%s", s.pid, s.port.c_str(),
                      s.needsFirmware ? " (needs firmware)" : "");
        c.name = b;
        c.driver = "pingdub-somagic";
        c.experimental = true;
        c.needsSetup = s.needsFirmware;
        c.somagic = s;
        m_devices.push_back(c);
    }
    for (const DeckLinkCardInfo& k : DeckLinkBackend::enumerate()) {
        CaptureDevice c;
        c.kind = CaptureDevice::Kind::DeckLink;
        c.id = "decklink:" + std::to_string(k.index);
        c.name = k.name + " [experimental]";
        c.driver = "DeckLink API";
        c.experimental = true;
        c.decklinkIndex = k.index;
        m_devices.push_back(c);
    }
    if (const char* f = std::getenv("PINGDUB_FAKE_DEVICES"); f && *f && *f != '0') {
        CaptureDevice c;
        c.kind = CaptureDevice::Kind::Fake;
        c.id = "fake:0";
        c.name = "Test pattern (fake device)";
        c.driver = "pingdub-fake";
        c.pairedAudio = "fake";
        m_devices.push_back(c);
    }
    scanMissing();
}

std::unique_ptr<VideoBackend> DeviceScanner::openVideo(const CaptureDevice& d, std::string& err) {
    switch (d.kind) {
    case CaptureDevice::Kind::V4L2: {
        auto b = std::make_unique<V4l2Backend>();
        if (!b->open(d.node, err)) return nullptr;
        return b;
    }
    case CaptureDevice::Kind::Somagic: {
        if (d.needsSetup) {
            err = "This Somagic device needs its firmware uploaded first (Diagnostics > Somagic setup).";
            return nullptr;
        }
        auto b = std::make_unique<SomagicBackend>();
        if (!b->open(d.somagic, err)) return nullptr;
        return b;
    }
    case CaptureDevice::Kind::DeckLink: {
        auto b = std::make_unique<DeckLinkBackend>();
        if (!b->open(d.decklinkIndex, err)) return nullptr;
        return b;
    }
    case CaptureDevice::Kind::Fake: return std::make_unique<FakeVideoBackend>();
    }
    err = "unknown device";
    return nullptr;
}

std::string DeviceScanner::hardwareReport() const {
    std::ostringstream o;
    o << "Capture hardware (USB/PCI IDs and bound drivers):\n";
    if (m_report.empty()) o << "  none of the known capture devices are connected\n";
    for (const auto& l : m_report) o << "  " << l << "\n";
    o << "Capture nodes:\n";
    if (m_devices.empty()) o << "  none\n";
    for (const auto& d : m_devices) {
        o << "  " << d.name << "  driver " << d.driver;
        if (!d.busInfo.empty()) o << "  bus " << d.busInfo;
        if (!d.pairedAudio.empty()) o << "  audio " << d.pairedAudio;
        o << "\n";
    }
    o << "Missing drivers:\n";
    if (m_missing.empty()) o << "  none\n";
    for (const auto& m : m_missing) o << "  " << m.ids << ": " << m.message << "\n";
    o << "ALSA capture devices:\n";
    for (const auto& a : AlsaBackend::enumerate()) o << "  " << a.name << "\n";
    o << "Groups: ";
    for (const auto& g : currentGroups()) o << g << " ";
    o << "\n  video group: " << (inGroup("video") ? "yes" : "no (fine if logind grants device access; add it if opening fails)")
      << "\n  audio group: " << (inGroup("audio") ? "yes" : "no (fine on most desktops; add it if hw: devices refuse to open)") << "\n";
    o << "DeckLink API: " << (DeckLinkBackend::available() ? "libDeckLinkAPI.so loaded" : "not installed") << "\n";
    o << "Somagic firmware: " << (fileExists(SomagicBackend::firmwarePath()) ? SomagicBackend::firmwarePath() : std::string("not installed")) << "\n";
    return o.str();
}

}  // namespace pd
