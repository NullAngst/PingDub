// SPDX-License-Identifier: GPL-3.0-or-later
// Finds capture hardware with libudev, watches hotplug, pairs each video
// node with the ALSA card under the same USB/PCI parent, and spots known
// devices that are plugged in but have no driver bound.
#pragma once

#include "backends/SomagicBackend.h"
#include "backends/VideoBackend.h"

#include <QObject>
#include <QTimer>

#include <memory>
#include <string>
#include <vector>

class QSocketNotifier;
struct udev;
struct udev_monitor;

namespace pd {

struct CaptureDevice {
    enum class Kind { V4L2, Somagic, DeckLink, Fake };
    Kind kind = Kind::V4L2;
    std::string id;           // stable within a session: "v4l2:/dev/video0", "somagic:1-2.1", ...
    std::string name;
    std::string node;
    std::string driver;
    std::string busInfo;
    bool experimental = false;
    bool needsSetup = false;  // Somagic without firmware loaded
    SomagicDeviceInfo somagic;
    int decklinkIndex = -1;
    std::string pairedAudio;  // ALSA pcm under the same parent, if any
};

struct MissingDriver {
    std::string name;
    std::string ids;
    std::string message;
};

class DeviceScanner : public QObject {
    Q_OBJECT
public:
    explicit DeviceScanner(QObject* parent = nullptr);
    ~DeviceScanner() override;

    void rescan();
    const std::vector<CaptureDevice>& devices() const { return m_devices; }
    const std::vector<MissingDriver>& missing() const { return m_missing; }
    std::string hardwareReport() const;

    static std::unique_ptr<VideoBackend> openVideo(const CaptureDevice& d, std::string& err);

signals:
    void changed();

private:
    void onUdev();
    void scanV4l2();
    void scanMissing();
    udev* m_udev = nullptr;
    udev_monitor* m_mon = nullptr;
    QSocketNotifier* m_notifier = nullptr;
    QTimer m_debounce;
    std::vector<CaptureDevice> m_devices;
    std::vector<MissingDriver> m_missing;
    std::vector<std::string> m_report;
};

}  // namespace pd
