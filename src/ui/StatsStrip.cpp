// SPDX-License-Identifier: GPL-3.0-or-later
#include "StatsStrip.h"

#include "Theme.h"

#include <QGridLayout>
#include <QLabel>

#include <cmath>

namespace pd {

QString humanBytes(double b) {
    if (b < 1024) return QString::number(qint64(b)) + " B";
    if (b < 1024.0 * 1024) return QString::number(b / 1024, 'f', 1) + " KB";
    if (b < 1024.0 * 1024 * 1024) return QString::number(b / 1024 / 1024, 'f', 1) + " MB";
    return QString::number(b / 1024 / 1024 / 1024, 'f', 2) + " GB";
}

QString humanDuration(double s) {
    if (s < 0) return "-";
    const qint64 t = qint64(s);
    return QString("%1:%2:%3").arg(t / 3600).arg((t / 60) % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
}

static const char* kKeys[] = {"State",      "Elapsed",   "Frames",   "Inserted",  "Dropped",   "Driver drops",
                              "Buffer ovf", "Xruns",     "A/V offset", "Drift",   "Data rate", "File size",
                              "Disk free",  "Time left", "RAM buffer", "Signal",  "Detected",  "Levels",
                              "Audio"};

StatsStrip::StatsStrip(QWidget* parent) : QFrame(parent) {
    setObjectName("statsStrip");
    auto* g = new QGridLayout(this);
    g->setContentsMargins(8, 4, 8, 4);
    g->setHorizontalSpacing(14);
    g->setVerticalSpacing(0);
    const int perRow = 10;
    int i = 0;
    for (const char* k : kKeys) {
        auto* key = new QLabel(tr(k), this);
        key->setObjectName("statKey");
        auto* val = new QLabel("-", this);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont f = val->font();
        f.setFamily("monospace");
        f.setStyleHint(QFont::Monospace);
        val->setFont(f);
        const int row = (i / perRow) * 2, col = i % perRow;
        g->addWidget(key, row, col);
        g->addWidget(val, row + 1, col);
        m_values[k] = val;
        ++i;
    }
    m_values["Inserted"]->setToolTip(tr("Repeated or black frames added to hold the timeline: driver drops, signal gaps, buffer overflow"));
    m_values["Dropped"]->setToolTip(tr("Frames removed by audio-master correction"));
    m_values["Driver drops"]->setToolTip(tr("Gaps in the driver's sequence numbers"));
    m_values["Buffer ovf"]->setToolTip(tr("Frames lost because the RAM buffer was full"));
    m_values["A/V offset"]->setToolTip(tr("Measured offset between audio and video; positive means audio would play late"));
    m_values["Drift"]->setToolTip(tr("Resampling applied to audio to follow the video clock, in parts per million"));
    m_values["Levels"]->setToolTip(tr("Crush, clip and chroma warnings: amber while more than the threshold of the picture "
                                      "sits outside 16-235 or past the chroma limit. A warning, never a stop."));
    m_values["Audio"]->setToolTip(tr("Hi-fi dropouts, dead channel and phase problems found so far. Reported only; the audio is never changed."));
}

void StatsStrip::set(const QString& key, const QString& value, Level level) {
    QLabel* l = m_values.value(key);
    if (!l) return;
    l->setText(value);
    QString style;
    if (level == Warn) style = QString("color: %1; font-weight: bold;").arg(StatusColor::Warn.name());
    else if (level == Bad) style = QString("color: %1; font-weight: bold;").arg(StatusColor::Bad.name());
    l->setStyleSheet(style);
}

void StatsStrip::showStats(const SessionStats& s, const VideoFormat& fmt, const std::string& standardName) {
    const bool cap = s.state == SessionState::Capturing;
    QString state;
    switch (s.state) {
    case SessionState::Closed: state = tr("No device"); break;
    case SessionState::Previewing: state = tr("Preview"); break;
    case SessionState::Capturing: state = tr("CAPTURING"); break;
    case SessionState::Stopping: state = s.finishing.empty() ? tr("Finishing") : tr("Finishing: %1").arg(QString::fromStdString(s.finishing)); break;
    }
    set("State", state, !s.error.empty() ? Bad : Normal);
    const SyncCounters& c = s.sync;
    set("Elapsed", cap ? humanDuration(s.elapsedSec) : "-");
    set("Frames", cap ? QString::number(c.framesOut) : "-");
    set("Inserted", cap ? QString::number(c.inserted) : "-", c.inserted ? Warn : Normal);
    set("Dropped", cap ? QString::number(c.dropped) : "-", c.dropped ? Warn : Normal);
    set("Driver drops", cap ? QString::number(c.driverDrops) : "-", c.driverDrops ? Warn : Normal);
    set("Buffer ovf", cap ? QString::number(c.ringDrops) : "-", c.ringDrops ? Bad : Normal);
    set("Xruns", cap ? QString::number(c.xruns) : "-", c.xruns ? Warn : Normal);
    const double off = std::fabs(c.avOffsetMs);
    const double frameMs = fmt.fps.valid() ? 1000.0 * fmt.fps.den / fmt.fps.num : 33.4;
    set("A/V offset", cap ? QString::number(c.avOffsetMs, 'f', 2) + " ms" : "-",
        off > frameMs ? Bad : off > frameMs / 2 ? Warn : Normal);
    set("Drift", cap ? QString::number(c.driftPpm, 'f', 1) + " ppm" : "-",
        c.clampHit ? Bad : std::fabs(c.driftPpm) > 1000 ? Warn : Normal);
    set("Data rate", cap ? humanBytes(s.dataRate) + "/s" : "-");
    set("File size", cap ? humanBytes(double(s.bytesWritten)) : "-");
    set("Disk free", humanBytes(double(s.diskFree)), s.diskFree < (5ull << 30) ? Bad : s.diskFree < (20ull << 30) ? Warn : Normal);
    set("Time left", humanDuration(s.timeLeftSec),
        s.timeLeftSec >= 0 && s.timeLeftSec < 600 ? Bad : s.timeLeftSec >= 0 && s.timeLeftSec < 1800 ? Warn : Normal);
    set("RAM buffer", QString::number(s.ringFill * 100.0, 'f', 1) + "%", s.ringFill > 0.85 ? Bad : s.ringFill > 0.5 ? Warn : Normal);
    QString sig = tr("locked");
    Level sl = Normal;
    if (s.state == SessionState::Closed) {
        sig = "-";
    } else if (s.signal.known && s.signal.noPower) {
        sig = tr("no power");
        sl = Bad;
    } else if (!s.signalLocked || (s.signal.known && s.signal.noSignal)) {
        sig = tr("no signal");
        sl = Bad;
    } else if (s.signal.known && s.signal.noColor) {
        sig = tr("no color");
        sl = Warn;
    }
    set("Signal", sig, sl);
    const QString det = QString::fromStdString(s.signal.detected);
    const bool mismatch = !det.isEmpty() && !QString::fromStdString(standardName).contains(det) &&
                          !det.contains(QString::fromStdString(standardName));
    set("Detected", det.isEmpty() ? "-" : det, mismatch ? Warn : Normal);
    if (!cap) {
        set("Levels", "-");
        set("Audio", "-");
        return;
    }
    QStringList active;
    static const char* names[3] = {"crush", "clip", "chroma"};
    for (int k = 0; k < 3; ++k)
        if (s.levels.active[k]) active << QString("%1 %2%").arg(names[k]).arg(s.levels.pct[k], 0, 'f', 1);
    if (!active.isEmpty()) set("Levels", active.join(", "), Warn);
    else set("Levels", s.levelEpisodes ? tr("%1 logged").arg(s.levelEpisodes) : tr("ok"));
    set("Audio", QString::number(s.audioEvents), s.audioEvents ? Warn : Normal);
}

}  // namespace pd
