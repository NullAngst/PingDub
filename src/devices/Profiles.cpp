// SPDX-License-Identifier: GPL-3.0-or-later
#include "Profiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <linux/videodev2.h>

#include <algorithm>

namespace pd {

QString ProfileStore::configDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.config";
    return base + "/pingdub";
}

QString ProfileStore::profilesDir() { return configDir() + "/profiles"; }
QString ProfileStore::appSettingsPath() { return configDir() + "/pingdub.ini"; }

QString ProfileStore::fileFor(const QString& name) {
    QString safe = name;
    safe.replace(QRegularExpression("[^A-Za-z0-9 _.,()+-]"), "_");
    return profilesDir() + "/" + safe + ".ini";
}

QString pixFmtToString(PixFmt f) { return f == PixFmt::Unknown ? QString() : QString::fromLatin1(pixFmtName(f)); }

PixFmt pixFmtFromString(const QString& s) {
    for (PixFmt f : {PixFmt::YUYV, PixFmt::UYVY, PixFmt::Y210, PixFmt::V210, PixFmt::HM12, PixFmt::NV12, PixFmt::GREY, PixFmt::MJPEG})
        if (s == QString::fromLatin1(pixFmtName(f))) return f;
    return PixFmt::Unknown;
}

FieldOrder fieldFromString(const QString& s) {
    if (s == "TFF") return FieldOrder::TFF;
    if (s == "BFF") return FieldOrder::BFF;
    if (s == "progressive") return FieldOrder::Progressive;
    return FieldOrder::Unknown;
}

Rational sarFromString(const QString& s) {
    const QStringList p = s.split(':');
    if (p.size() != 2) return {0, 1};
    return {p[0].toInt(), p[1].toInt()};
}

static QString enumName(SyncMode m) { return QString::fromLatin1(syncModeName(m)); }

bool ProfileStore::save(const Profile& p) const {
    QDir().mkpath(profilesDir());
    QSettings s(fileFor(p.name), QSettings::IniFormat);
    s.clear();
    s.setValue("profile/name", p.name);
    s.setValue("profile/device", p.deviceKey);
    s.setValue("video/input", p.input);
    s.setValue("video/standard", QString::number(p.standard, 16));
    s.setValue("video/pixel_format", p.pixfmt);
    s.setValue("video/field_order_override", p.fieldOverride);
    s.setValue("video/sar_override", p.sarOverride);
    s.beginGroup("controls");
    for (auto it = p.controls.begin(); it != p.controls.end(); ++it) s.setValue(QString::number(it.key(), 16), it.value());
    s.endGroup();
    s.setValue("audio/device", p.audioDevice);
    s.setValue("audio/rate", p.audioRate);
    s.beginGroup("mixer");
    for (auto it = p.mixer.begin(); it != p.mixer.end(); ++it) s.setValue(it.key(), it.value());
    s.endGroup();
    s.setValue("timing/sync", enumName(p.timing.sync));
    s.setValue("timing/gap_fill", p.timing.gapFill == GapFill::Black ? "black" : "last-frame");
    s.setValue("timing/v4l2_buffers", p.timing.v4l2Buffers);
    s.setValue("timing/ring_mb", qulonglong(p.timing.ringBytes >> 20));
    s.setValue("output/container", p.output.container == Container::AVI ? "avi" : "mkv");
    const char* vc = p.output.vcodec == VideoCodec::UTVideo ? "utvideo"
                     : p.output.vcodec == VideoCodec::Huff  ? "huffyuv"
                     : p.output.vcodec == VideoCodec::Raw   ? "raw"
                                                            : "ffv1";
    s.setValue("output/video_codec", vc);
    s.setValue("output/audio_codec", p.output.acodec == AudioCodec::FLAC ? "flac" : "pcm");
    s.setValue("output/folder", QString::fromStdString(p.output.folder));
    s.setValue("output/overflow_folder", QString::fromStdString(p.output.overflowFolder));
    s.setValue("output/overflow_free_gb", qulonglong(p.output.overflowFreeBytes >> 30));
    s.setValue("output/segment_mb", qulonglong(p.output.segmentBytes >> 20));
    s.setValue("output/segment_minutes", p.output.segmentSeconds / 60.0);
    s.setValue("output/chapter_interval_minutes", p.output.chapterIntervalMin);
    s.setValue("output/chapter_boundaries", p.output.chapterBoundaries);
    s.setValue("output/contact_sheet_seconds", p.output.contactSheetSec);
    s.sync();
    return s.status() == QSettings::NoError;
}

bool ProfileStore::load(const QString& name, Profile& p) const {
    const QString f = fileFor(name);
    if (!QFile::exists(f)) return false;
    QSettings s(f, QSettings::IniFormat);
    p = Profile{};
    p.name = s.value("profile/name", name).toString();
    p.deviceKey = s.value("profile/device").toString();
    p.input = s.value("video/input", -1).toInt();
    p.standard = s.value("video/standard", "0").toString().toULongLong(nullptr, 16);
    p.pixfmt = s.value("video/pixel_format").toString();
    p.fieldOverride = s.value("video/field_order_override").toString();
    p.sarOverride = s.value("video/sar_override").toString();
    s.beginGroup("controls");
    for (const QString& k : s.childKeys()) p.controls[k.toUInt(nullptr, 16)] = s.value(k).toLongLong();
    s.endGroup();
    p.audioDevice = s.value("audio/device", "auto").toString();
    p.audioRate = s.value("audio/rate", 48000).toInt();
    s.beginGroup("mixer");
    for (const QString& k : s.childKeys()) p.mixer[k] = s.value(k).toLongLong();
    s.endGroup();
    const QString sync = s.value("timing/sync", "video-master").toString();
    p.timing.sync = sync == "audio-master" ? SyncMode::AudioMaster : sync == "no-correction" ? SyncMode::NoCorrection : SyncMode::VideoMaster;
    p.timing.gapFill = s.value("timing/gap_fill").toString() == "black" ? GapFill::Black : GapFill::LastFrame;
    p.timing.v4l2Buffers = std::clamp(s.value("timing/v4l2_buffers", 32).toInt(), 8, 64);
    p.timing.ringBytes = std::clamp<qulonglong>(s.value("timing/ring_mb", 2048).toULongLong(), 256, 16384) << 20;
    p.output.container = s.value("output/container").toString() == "avi" ? Container::AVI : Container::MKV;
    const QString vc = s.value("output/video_codec", "ffv1").toString();
    p.output.vcodec = vc == "utvideo" ? VideoCodec::UTVideo : vc == "huffyuv" ? VideoCodec::Huff : vc == "raw" ? VideoCodec::Raw : VideoCodec::FFV1;
    p.output.acodec = s.value("output/audio_codec").toString() == "flac" ? AudioCodec::FLAC : AudioCodec::PCM;
    p.output.folder = s.value("output/folder", "~/Videos/PingDub").toString().toStdString();
    p.output.overflowFolder = s.value("output/overflow_folder").toString().toStdString();
    p.output.overflowFreeBytes = s.value("output/overflow_free_gb", 10).toULongLong() << 30;
    p.output.segmentBytes = s.value("output/segment_mb", 0).toULongLong() << 20;
    p.output.segmentSeconds = s.value("output/segment_minutes", 0).toDouble() * 60.0;
    p.output.chapterIntervalMin = std::clamp(s.value("output/chapter_interval_minutes", 0).toInt(), 0, 240);
    p.output.chapterBoundaries = s.value("output/chapter_boundaries", false).toBool();
    p.output.contactSheetSec = std::clamp(s.value("output/contact_sheet_seconds", 30.0).toDouble(), 0.0, 3600.0);
    return true;
}

QStringList ProfileStore::allProfiles() const {
    QStringList out;
    QDir d(profilesDir());
    for (const QFileInfo& fi : d.entryInfoList({"*.ini"}, QDir::Files, QDir::Name)) {
        QSettings s(fi.absoluteFilePath(), QSettings::IniFormat);
        out << s.value("profile/name", fi.completeBaseName()).toString();
    }
    return out;
}

QStringList ProfileStore::profilesFor(const QString& deviceKey) const {
    QStringList out;
    QDir d(profilesDir());
    for (const QFileInfo& fi : d.entryInfoList({"*.ini"}, QDir::Files, QDir::Name)) {
        QSettings s(fi.absoluteFilePath(), QSettings::IniFormat);
        if (s.value("profile/device").toString() == deviceKey) out << s.value("profile/name", fi.completeBaseName()).toString();
    }
    return out;
}

QString ProfileStore::defaultFor(const QString& deviceKey) const {
    QSettings s(appSettingsPath(), QSettings::IniFormat);
    s.beginGroup("default_profiles");
    return s.value(QString(deviceKey).replace('/', '_')).toString();
}

void ProfileStore::setDefault(const QString& deviceKey, const QString& name) {
    QDir().mkpath(configDir());
    QSettings s(appSettingsPath(), QSettings::IniFormat);
    s.beginGroup("default_profiles");
    s.setValue(QString(deviceKey).replace('/', '_'), name);
}

bool ProfileStore::remove(const QString& name) const { return QFile::remove(fileFor(name)); }

bool ProfileStore::exportTo(const QString& name, const QString& path) const {
    QFile::remove(path);
    return QFile::copy(fileFor(name), path);
}

bool ProfileStore::importFrom(const QString& path, QString& importedName) const {
    QSettings s(path, QSettings::IniFormat);
    importedName = s.value("profile/name", QFileInfo(path).completeBaseName()).toString();
    if (importedName.isEmpty()) return false;
    QDir().mkpath(profilesDir());
    const QString dest = fileFor(importedName);
    QFile::remove(dest);
    return QFile::copy(path, dest);
}

Profile ProfileStore::firstRun(const QString& deviceKey) {
    Profile p;
    p.name = "Default";
    p.deviceKey = deviceKey;
    // NTSC-M in the Americas, Japan's tapes aside; PAL-B/G elsewhere. The
    // user picks NTSC-J explicitly: a US tape captured as NTSC-J has lifted blacks.
    const QLocale::Territory t = QLocale::system().territory();
    const bool ntsc = t == QLocale::UnitedStates || t == QLocale::Canada || t == QLocale::Mexico || t == QLocale::Japan ||
                      t == QLocale::SouthKorea || t == QLocale::Taiwan || t == QLocale::Philippines || t == QLocale::AnyTerritory;
    p.standard = ntsc ? V4L2_STD_NTSC_M : V4L2_STD_PAL_BG;
    p.output.folder = "~/Videos/PingDub";
    return p;
}

}  // namespace pd
