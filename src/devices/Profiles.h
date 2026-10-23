// SPDX-License-Identifier: GPL-3.0-or-later
// Per-device named profiles in ~/.config/pingdub/profiles/<name>.ini and app
// settings in ~/.config/pingdub/pingdub.ini. Plain INI: easy to back up,
// export and import are file copies.
#pragma once

#include "engine/CaptureConfig.h"
#include "engine/Types.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace pd {

struct Profile {
    QString name;
    QString deviceKey;
    int input = -1;
    quint64 standard = 0;
    QString pixfmt;            // "" = most native
    QString fieldOverride;     // "", "TFF", "BFF", "progressive"
    QString sarOverride;       // "", "10:11", ...
    QMap<quint32, qint64> controls;
    QString audioDevice = "auto";  // "auto" (paired card), "none", or an ALSA pcm name
    int audioRate = 48000;
    QMap<QString, qint64> mixer;   // "name#index" -> value
    TimingSettings timing;
    OutputSettings output;
};

class ProfileStore {
public:
    static QString configDir();    // ~/.config/pingdub
    static QString profilesDir();  // ~/.config/pingdub/profiles
    static QString appSettingsPath();

    QStringList profilesFor(const QString& deviceKey) const;
    QStringList allProfiles() const;
    QString defaultFor(const QString& deviceKey) const;
    void setDefault(const QString& deviceKey, const QString& name);
    bool load(const QString& name, Profile& out) const;
    bool save(const Profile& p) const;
    bool remove(const QString& name) const;
    bool exportTo(const QString& name, const QString& path) const;
    bool importFrom(const QString& path, QString& importedName) const;

    // First-run profile: most native format, NTSC-M or PAL by locale, paired
    // audio, FFV1 + PCM in MKV, video master.
    static Profile firstRun(const QString& deviceKey);
    static QString fileFor(const QString& name);
};

QString pixFmtToString(PixFmt f);
PixFmt pixFmtFromString(const QString& s);
FieldOrder fieldFromString(const QString& s);
Rational sarFromString(const QString& s);

}  // namespace pd
