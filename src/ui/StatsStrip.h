// SPDX-License-Identifier: GPL-3.0-or-later
// The numbers that matter during a capture, always visible. Anything
// abnormal turns amber, then red.
#pragma once

#include "engine/CaptureSession.h"

#include <QFrame>
#include <QMap>

class QLabel;

namespace pd {

class StatsStrip : public QFrame {
    Q_OBJECT
public:
    explicit StatsStrip(QWidget* parent = nullptr);
    void showStats(const SessionStats& s, const VideoFormat& fmt, const std::string& standardName);

private:
    enum Level { Normal, Warn, Bad };
    void set(const QString& key, const QString& value, Level level = Normal);
    QMap<QString, QLabel*> m_values;
};

QString humanBytes(double bytes);
QString humanDuration(double seconds);

}  // namespace pd
