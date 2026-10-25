// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionController.h"

#include "backends/AlsaBackend.h"
#include "backends/FakeBackends.h"
#include "devices/KnownDevices.h"

#include <QDir>
#include <QMetaObject>
#include <QThread>
#include <QSettings>

#include <algorithm>
#include <thread>

namespace pd {

void AppSettings::load() {
    QSettings s(ProfileStore::appSettingsPath(), QSettings::IniFormat);
    theme = s.value("ui/theme").toString();
    keyStart = s.value("keys/start", "F5").toString();
    keyStop = s.value("keys/stop", "Esc").toString();
    previewDeinterlace = s.value("ui/preview_deinterlace", "weave").toString();
    lastDevice = s.value("ui/last_device").toString();
    lastLabel = s.value("ui/last_label").toString();
    confirmStop = s.value("ui/confirm_stop", true).toBool();
    captureVbi = s.value("capture/vbi", true).toBool();
    notifyAutoStop = s.value("capture/notify_auto_stop", true).toBool();
    levelThresholdPct = std::clamp(s.value("levels/threshold_pct", 0.5).toDouble(), 0.01, 50.0);
    levelSeconds = std::clamp(s.value("levels/seconds", 3.0).toDouble(), 0.5, 600.0);
    chromaLimit = std::clamp(s.value("levels/chroma_limit", 112).toInt(), 60, 180);
    scope = s.value("ui/scope", "histogram").toString();
    parade = s.value("ui/parade", false).toBool();
    lastCaptureFolder = s.value("ui/last_capture_folder").toString();
    stop.durationSec = s.value("stop/duration_min", 0).toDouble() * 60.0;
    stop.fileBytes = s.value("stop/file_gb", 0).toULongLong() << 30;
    stop.minFreeBytes = s.value("stop/min_free_gb", 2).toULongLong() << 30;
    stop.maxDropPerMin = s.value("stop/max_drops_per_min", 0).toDouble();
    stop.signalLostSec = s.value("stop/signal_lost_sec", 0).toDouble();
    stop.staticSec = s.value("stop/static_sec", 0).toDouble();
    stop.staticStddev = s.value("stop/static_stddev", 2.5).toDouble();
}

void AppSettings::save() const {
    QDir().mkpath(ProfileStore::configDir());
    QSettings s(ProfileStore::appSettingsPath(), QSettings::IniFormat);
    s.setValue("ui/theme", theme);
    s.setValue("keys/start", keyStart);
    s.setValue("keys/stop", keyStop);
    s.setValue("ui/preview_deinterlace", previewDeinterlace);
    s.setValue("ui/last_device", lastDevice);
    s.setValue("ui/last_label", lastLabel);
    s.setValue("ui/confirm_stop", confirmStop);
    s.setValue("capture/vbi", captureVbi);
    s.setValue("capture/notify_auto_stop", notifyAutoStop);
    s.setValue("levels/threshold_pct", levelThresholdPct);
    s.setValue("levels/seconds", levelSeconds);
    s.setValue("levels/chroma_limit", chromaLimit);
    s.setValue("ui/scope", scope);
    s.setValue("ui/parade", parade);
    s.setValue("ui/last_capture_folder", lastCaptureFolder);
    s.setValue("stop/duration_min", stop.durationSec / 60.0);
    s.setValue("stop/file_gb", qulonglong(stop.fileBytes >> 30));
    s.setValue("stop/min_free_gb", qulonglong(stop.minFreeBytes >> 30));
    s.setValue("stop/max_drops_per_min", stop.maxDropPerMin);
    s.setValue("stop/signal_lost_sec", stop.signalLostSec);
    s.setValue("stop/static_sec", stop.staticSec);
    s.setValue("stop/static_stddev", stop.staticStddev);
}

AnalysisSettings AppSettings::analysis() const {
    AnalysisSettings a;
    a.chromaLimit = chromaLimit;
    a.watch.thresholdPct = levelThresholdPct;
    a.watch.minSeconds = levelSeconds;
    return a;
}

SessionController::SessionController(DeviceScanner* scanner, QObject* parent) : QObject(parent), m_scanner(scanner) {
    m_settings.load();
    m_session.onAutoStop = [this](const CaptureSummary& s) {
        QMetaObject::invokeMethod(this, [this, s] { emit captureFinished(s, true); }, Qt::QueuedConnection);
    };
    m_session.onDeviceError = [this](const std::string& e) {
        const QString msg = QString::fromStdString(e);
        QMetaObject::invokeMethod(this, [this, msg] {
            m_session.close();
            emit deviceLost(msg);
            emit deviceChanged();
        }, Qt::QueuedConnection);
    };
}

SessionController::~SessionController() {
    while (m_stopping.load()) QThread::msleep(20);
    if (m_session.capturing()) m_session.stopCapture("PingDub closed");
    m_session.close();
}

const CaptureDevice* SessionController::device() const { return m_haveDevice ? &m_device : nullptr; }

QStringList SessionController::audioChoices() const {
    QStringList out{"auto", "none"};
    for (const AlsaDevice& d : AlsaBackend::enumerate()) out << QString::fromStdString(d.pcm);
    return out;
}

QString SessionController::audioChoiceLabel(const QString& pcm) const {
    if (pcm == "auto") return tr("Automatic (card paired with the video device)");
    if (pcm == "none") return tr("No audio");
    for (const AlsaDevice& d : AlsaBackend::enumerate())
        if (QString::fromStdString(d.pcm) == pcm) return QString::fromStdString(d.name);
    return pcm;
}

std::unique_ptr<AudioBackend> SessionController::makeAudio(const Profile& p, const CaptureDevice& d, QString& name) const {
    QString pcm = p.audioDevice;
    if (pcm == "none") return nullptr;
    if (d.kind == CaptureDevice::Kind::Fake && pcm == "auto") {
        name = "PingDub test tone";
        return std::make_unique<FakeAudioBackend>();
    }
    if (pcm == "auto") {
        // Somagic and DeckLink carry audio in the same stream: let the session use it.
        if (d.kind == CaptureDevice::Kind::Somagic || d.kind == CaptureDevice::Kind::DeckLink) return nullptr;
        pcm = QString::fromStdString(d.pairedAudio);
        if (pcm.isEmpty()) return nullptr;
    }
    name = audioChoiceLabel(pcm);
    return std::make_unique<AlsaBackend>(pcm.toStdString());
}

SessionConfig SessionController::configFrom(const Profile& p) const {
    SessionConfig c;
    c.input = p.input;
    c.standard = p.standard;
    c.pixfmt = pixFmtFromString(p.pixfmt);
    c.fieldOverride = fieldFromString(p.fieldOverride);
    c.sarOverride = sarFromString(p.sarOverride);
    for (auto it = p.controls.begin(); it != p.controls.end(); ++it) c.controls[it.key()] = it.value();
    c.audioEnabled = p.audioDevice != "none";
    c.audioRate = p.audioRate;
    c.timing = p.timing;
    c.output = p.output;
    c.stop = m_settings.stop;
    c.captureVbi = m_settings.captureVbi;
    c.realtime = true;
    c.profileName = p.name.toStdString();
    c.analysis = m_settings.analysis();
    return c;
}

bool SessionController::openWith(const CaptureDevice& d, std::unique_ptr<VideoBackend> v, const Profile& p, QString& err) {
    QString audioName;
    auto audio = makeAudio(p, d, audioName);
    if (audio) {
        // stk1160 audio is silent until its mixer source is Line.
        const std::string q = AlsaBackend::applyMixerQuirks(d.driver);
        if (!q.empty()) emit message(QString::fromStdString(q));
    }
    SessionConfig cfg = configFrom(p);
    cfg.audioDeviceName = audioName.toStdString();
    // Per-chip pixel aspect from the quirk table, unless the profile overrides it.
    bool usb = false;
    uint16_t vid = 0, pid = 0;
    if (!cfg.sarOverride.valid() && v->hardwareId(usb, vid, pid)) {
        if (const KnownDevice* k = lookupKnownDevice(usb ? KnownDevice::Bus::USB : KnownDevice::Bus::PCI, vid, pid);
            k && k->sar.valid())
            cfg.sarOverride = k->sar;
    }
    std::string e;
    if (!m_session.open(std::move(v), std::move(audio), cfg, e)) {
        err = QString::fromStdString(e);
        return false;
    }
    // Saved mixer state.
    if (AudioBackend* a = m_session.audio()) {
        for (auto it = p.mixer.begin(); it != p.mixer.end(); ++it) {
            const QStringList parts = it.key().split('#');
            std::string me;
            a->setMixer(parts.value(0).toStdString(), parts.value(1).toInt(), long(it.value()), me);
        }
    }
    return true;
}

bool SessionController::openDevice(const QString& id, QString& err) {
    if (m_session.capturing()) {
        err = tr("Stop the capture first.");
        return false;
    }
    m_session.close();
    m_haveDevice = false;
    const CaptureDevice* found = nullptr;
    for (const CaptureDevice& d : m_scanner->devices())
        if (QString::fromStdString(d.id) == id) found = &d;
    if (!found) {
        err = tr("Device not found.");
        emit deviceChanged();
        return false;
    }
    std::string e;
    auto v = DeviceScanner::openVideo(*found, e);
    if (!v) {
        err = QString::fromStdString(e);
        emit deviceChanged();
        return false;
    }
    m_device = *found;
    m_haveDevice = true;
    m_deviceId = id;
    m_deviceKey = QString::fromStdString(v->profileKey());
    Profile p;
    const QString def = m_store.defaultFor(m_deviceKey);
    if (def.isEmpty() || !m_store.load(def, p)) p = ProfileStore::firstRun(m_deviceKey);
    m_profile = p;
    const bool ok = openWith(m_device, std::move(v), m_profile, err);
    m_settings.lastDevice = id;
    m_settings.save();
    emit deviceChanged();
    return ok;
}

void SessionController::closeDevice() {
    if (m_session.capturing()) m_session.stopCapture("device closed");
    m_session.close();
    m_haveDevice = false;
    emit deviceChanged();
}

static bool deviceLevelChange(const Profile& a, const Profile& b) {
    return a.input != b.input || a.standard != b.standard || a.pixfmt != b.pixfmt || a.fieldOverride != b.fieldOverride ||
           a.sarOverride != b.sarOverride || a.audioDevice != b.audioDevice || a.audioRate != b.audioRate ||
           a.timing.v4l2Buffers != b.timing.v4l2Buffers;
}

bool SessionController::applyProfile(const Profile& p, QString& err) {
    if (m_session.capturing()) {
        err = tr("Settings are locked while capturing.");
        return false;
    }
    const bool reopen = deviceLevelChange(m_profile, p) || !m_session.isOpen();
    m_profile = p;
    if (!m_haveDevice) return true;
    if (reopen) {
        m_session.close();
        std::string e;
        auto v = DeviceScanner::openVideo(m_device, e);
        if (!v) {
            err = QString::fromStdString(e);
            emit deviceChanged();
            return false;
        }
        const bool ok = openWith(m_device, std::move(v), m_profile, err);
        emit deviceChanged();
        return ok;
    }
    m_session.setRuntimeSettings(p.output, p.timing, m_settings.stop, m_settings.captureVbi);
    m_session.setProfileName(p.name.toStdString());
    for (auto it = p.controls.begin(); it != p.controls.end(); ++it) {
        std::string e;
        m_session.setControl(it.key(), it.value(), e);
    }
    emit deviceChanged();
    return true;
}

void SessionController::applyStopConditions(const StopConditions& s) {
    m_settings.stop = s;
    m_settings.save();
    m_session.setRuntimeSettings(m_profile.output, m_profile.timing, s, m_settings.captureVbi);
}

void SessionController::applyAnalysisSettings() { m_session.setAnalysisSettings(m_settings.analysis()); }

bool SessionController::saveControlsToProfile() {
    if (m_session.capturing() || !m_session.video()) return false;
    for (const VideoControl& c : m_session.video()->controls())
        if (!c.readOnly && c.type != VideoControl::Type::Button) m_profile.controls[c.id] = c.value;
    // A first-run profile that was never saved gets written now under its name.
    return saveProfileAs(m_profile.name.isEmpty() ? QStringLiteral("Default") : m_profile.name);
}

bool SessionController::saveProfileAs(const QString& name) {
    Profile p = m_profile;
    p.name = name;
    p.deviceKey = m_deviceKey;
    // Current control values and mixer state go in with the rest.
    if (VideoBackend* v = m_session.video()) {
        p.controls.clear();
        for (const VideoControl& c : v->controls())
            if (!c.readOnly && c.type != VideoControl::Type::Button) p.controls[c.id] = c.value;
    }
    if (AudioBackend* a = m_session.audio()) {
        p.mixer.clear();
        for (const MixerControl& m : a->mixer()) p.mixer[QString::fromStdString(m.name) + "#" + QString::number(m.index)] = m.value;
    }
    if (!m_store.save(p)) return false;
    m_profile = p;
    m_session.setProfileName(name.toStdString());
    if (m_store.defaultFor(m_deviceKey).isEmpty()) m_store.setDefault(m_deviceKey, name);
    return true;
}

bool SessionController::switchProfile(const QString& name, QString& err) {
    Profile p;
    if (!m_store.load(name, p)) {
        err = tr("Cannot read profile %1").arg(name);
        return false;
    }
    return applyProfile(p, err);
}

bool SessionController::startCapture(const QString& label, QString& err, double durationOverride) {
    StopConditions stop = m_settings.stop;
    if (durationOverride > 0) stop.durationSec = durationOverride;
    m_session.setRuntimeSettings(m_profile.output, m_profile.timing, stop, m_settings.captureVbi);
    std::string e;
    if (!m_session.startCapture(label.toStdString(), e)) {
        err = QString::fromStdString(e);
        return false;
    }
    m_settings.lastLabel = label;
    m_settings.save();
    emit captureStarted();
    return true;
}

void SessionController::stopCaptureAsync(const QString& reason) {
    if (m_stopping.exchange(true)) return;
    std::thread([this, reason] {
        CaptureSummary s = m_session.stopCapture(reason.toStdString());
        QMetaObject::invokeMethod(this, [this, s] {
            m_stopping = false;
            emit captureFinished(s, false);
        }, Qt::QueuedConnection);
    }).detach();
}

CaptureSummary SessionController::stopCapture(const QString& reason) {
    CaptureSummary s = m_session.stopCapture(reason.toStdString());
    emit captureFinished(s, false);
    return s;
}

}  // namespace pd
