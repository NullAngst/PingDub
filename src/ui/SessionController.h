// SPDX-License-Identifier: GPL-3.0-or-later
// Glue between the windows and the capture session: which device is open,
// which profile drives it, app settings, and turning profiles into a
// SessionConfig. Session callbacks arrive on other threads and are passed
// to the GUI thread here.
#pragma once

#include "devices/DeviceScanner.h"
#include "devices/Profiles.h"
#include "engine/CaptureSession.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>

namespace pd {

struct AppSettings {
    QString theme;
    QString keyStart = "F5";
    QString keyStop = "Esc";
    StopConditions stop;
    bool captureVbi = true;
    QString previewDeinterlace = "weave";
    QString lastDevice;
    QString lastLabel;
    bool confirmStop = true;
    bool notifyAutoStop = true;      // desktop notification when a capture stops on its own
    double levelThresholdPct = 0.5;  // crush/clip/chroma warning threshold
    double levelSeconds = 3.0;       // ...held this long
    int chromaLimit = 112;           // chroma distance from neutral counted as past the limit
    QString scope = "histogram";     // histogram, waveform, vectorscope
    bool parade = false;
    QString lastCaptureFolder;

    AnalysisSettings analysis() const;
    void load();
    void save() const;
};

class SessionController : public QObject {
    Q_OBJECT
public:
    explicit SessionController(DeviceScanner* scanner, QObject* parent = nullptr);
    ~SessionController() override;

    CaptureSession& session() { return m_session; }
    DeviceScanner* scanner() { return m_scanner; }
    ProfileStore& profiles() { return m_store; }
    AppSettings& settings() { return m_settings; }
    const Profile& profile() const { return m_profile; }
    QString deviceId() const { return m_deviceId; }
    const CaptureDevice* device() const;
    QString deviceKey() const { return m_deviceKey; }

    // Opens a device with its default profile (or a first-run profile).
    bool openDevice(const QString& id, QString& err);
    void closeDevice();
    // Applies a profile. Device-level changes reopen the device; the rest applies live.
    bool applyProfile(const Profile& p, QString& err);
    bool saveProfileAs(const QString& name);
    bool switchProfile(const QString& name, QString& err);

    // durationOverride > 0: this capture stops at exactly that length (timed capture).
    bool startCapture(const QString& label, QString& err, double durationOverride = 0);
    CaptureSummary stopCapture(const QString& reason = "stopped by user");
    // Finalizes on a worker thread so the window stays live while the RAM
    // buffer drains; captureFinished arrives when it is done.
    void stopCaptureAsync(const QString& reason = "stopped by user");
    bool stopping() const { return m_stopping.load(); }
    bool capturing() const { return m_session.capturing(); }

    QStringList audioChoices() const;      // "auto", "none", ALSA pcms
    QString audioChoiceLabel(const QString& pcm) const;
    void applyStopConditions(const StopConditions& s);
    void applyAnalysisSettings();
    // Writes the device's current control values into the profile file, so
    // values from Set Levels come back with the device and input.
    bool saveControlsToProfile();

signals:
    void deviceChanged();
    void captureStarted();
    void captureFinished(const CaptureSummary& s, bool automatic);
    void deviceLost(const QString& why);
    void message(const QString& text);

private:
    SessionConfig configFrom(const Profile& p) const;
    std::unique_ptr<AudioBackend> makeAudio(const Profile& p, const CaptureDevice& d, QString& name) const;
    bool openWith(const CaptureDevice& d, std::unique_ptr<VideoBackend> v, const Profile& p, QString& err);

    DeviceScanner* m_scanner;
    CaptureSession m_session;
    ProfileStore m_store;
    AppSettings m_settings;
    Profile m_profile;
    QString m_deviceId;
    QString m_deviceKey;
    CaptureDevice m_device;
    bool m_haveDevice = false;
    std::atomic<bool> m_stopping{false};
};

}  // namespace pd
