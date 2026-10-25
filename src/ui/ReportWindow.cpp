// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReportWindow.h"

#include "StatsStrip.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <thread>

namespace pd {

namespace {

constexpr int kLabelW = 64;
constexpr int kMarkH = 16;
constexpr int kLaneH = 24;

QString count(qint64 n, const char* one, const char* many) {
    return QString::number(n) + " " + ReportWindow::tr(n == 1 ? one : many);
}

QColor laneColor(ReportLane l) {
    switch (l) {
    case ReportLane::Video: return StatusColor::Bad;
    case ReportLane::Levels: return StatusColor::Warn;
    case ReportLane::Audio: return QColor(0x58, 0xA6, 0xFF);
    default: return QColor(0x80, 0x80, 0x80);
    }
}

const char* laneName(ReportLane l) {
    switch (l) {
    case ReportLane::Video: return QT_TRANSLATE_NOOP("pd::ReportWindow", "Video");
    case ReportLane::Levels: return QT_TRANSLATE_NOOP("pd::ReportWindow", "Levels");
    case ReportLane::Audio: return QT_TRANSLATE_NOOP("pd::ReportWindow", "Audio");
    default: return QT_TRANSLATE_NOOP("pd::ReportWindow", "Info");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// timeline

ReportTimeline::ReportTimeline(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(kMarkH + 3 * kLaneH + 18);
    setMouseTracking(true);
}

void ReportTimeline::setRecord(const CaptureRecord* r) {
    m_r = r;
    m_selEvent = -1;
    m_selSlot = -1;
    update();
}

void ReportTimeline::setSelected(int eventIndex, int64_t slot) {
    m_selEvent = eventIndex;
    m_selSlot = slot;
    update();
}

QRect ReportTimeline::lanesRect() const { return QRect(kLabelW, kMarkH, width() - kLabelW - 6, 3 * kLaneH); }

int64_t ReportTimeline::total() const {
    if (!m_r) return 1;
    int64_t t = m_r->frames;
    for (const RecordEvent& e : m_r->events) t = std::max(t, e.endSlot + 1);
    return std::max<int64_t>(1, t);
}

double ReportTimeline::xOf(int64_t slot) const {
    const QRect a = lanesRect();
    return a.left() + double(slot) * a.width() / double(total());
}

int ReportTimeline::hit(const QPoint& p) const {
    if (!m_r) return -1;
    const QRect a = lanesRect();
    const int lane = (p.y() - a.top()) / kLaneH;
    if (p.y() < a.top() || lane < 0 || lane > 2) return -1;
    int best = -1;
    double bestD = 6.0;
    for (size_t i = 0; i < m_r->events.size(); ++i) {
        const RecordEvent& e = m_r->events[i];
        if (int(e.lane) != lane || e.slot < 0) continue;
        const double x0 = xOf(e.slot), x1 = std::max(x0 + 2, xOf(e.endSlot + 1));
        const double d = p.x() < x0 ? x0 - p.x() : (p.x() > x1 ? p.x() - x1 : 0.0);
        if (d < bestD) {
            bestD = d;
            best = int(i);
        }
    }
    return best;
}

void ReportTimeline::mousePressEvent(QMouseEvent* e) {
    if (!m_r) return;
    const QPoint pos = e->position().toPoint();
    const int h = hit(pos);
    if (h >= 0) {
        emit eventClicked(h);
        return;
    }
    const QRect a = lanesRect();
    if (pos.x() >= a.left() && pos.x() <= a.right()) {
        const int64_t slot = std::clamp<int64_t>(int64_t(double(pos.x() - a.left()) * double(total()) / a.width()), 0, total() - 1);
        emit slotClicked(slot);
    }
}

bool ReportTimeline::event(QEvent* ev) {
    if (ev->type() == QEvent::ToolTip && m_r) {
        auto* he = static_cast<QHelpEvent*>(ev);
        const int h = hit(he->pos());
        if (h >= 0) {
            const RecordEvent& e = m_r->events[size_t(h)];
            QToolTip::showText(he->globalPos(), QString("%1  %2\n%3").arg(QString::fromStdString(m_r->clock(e.slot)),
                                                                           QString::fromStdString(e.label), QString::fromStdString(e.detail)));
            return true;
        }
        if (he->pos().y() < kMarkH) {
            for (const Chapter& c : m_r->chapters)
                if (std::fabs(xOf(c.start) - he->pos().x()) < 4) {
                    QToolTip::showText(he->globalPos(), QString::fromStdString(c.title));
                    return true;
                }
        }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(ev);
}

void ReportTimeline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRect a = lanesRect();
    const QPalette pal = palette();
    p.fillRect(a, pal.color(QPalette::Base));
    p.setPen(pal.color(QPalette::Text));
    for (int i = 0; i < 3; ++i) {
        const QRect lane(0, a.top() + i * kLaneH, kLabelW - 6, kLaneH);
        p.drawText(lane, Qt::AlignRight | Qt::AlignVCenter, ReportWindow::tr(laneName(ReportLane(i))));
        p.setPen(pal.color(QPalette::Mid));
        p.drawLine(a.left(), a.top() + i * kLaneH, a.right(), a.top() + i * kLaneH);
        p.setPen(pal.color(QPalette::Text));
    }
    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(a.adjusted(0, 0, -1, -1));
    if (!m_r) return;
    // Chapter marks along the top.
    p.setPen(QPen(pal.color(QPalette::Text), 1));
    for (const Chapter& c : m_r->chapters) {
        const int x = int(xOf(c.start));
        p.drawLine(x, 2, x, kMarkH - 2);
        p.drawLine(x, kMarkH - 2, x + 4, kMarkH - 6);
    }
    for (size_t i = 0; i < m_r->events.size(); ++i) {
        const RecordEvent& e = m_r->events[i];
        if (e.lane == ReportLane::Info || e.slot < 0) continue;
        const int lane = int(e.lane);
        const double x0 = xOf(e.slot), x1 = std::max(x0 + 2.0, xOf(e.endSlot + 1));
        const QRectF r(x0, a.top() + lane * kLaneH + 3, x1 - x0, kLaneH - 6);
        p.fillRect(r, laneColor(e.lane));
        if (int(i) == m_selEvent) {
            p.setPen(QPen(pal.color(QPalette::Highlight), 2));
            p.drawRect(r.adjusted(-2, -2, 2, 2));
        }
    }
    if (m_selSlot >= 0) {
        const int x = int(xOf(m_selSlot));
        p.setPen(QPen(pal.color(QPalette::Highlight), 1));
        p.drawLine(x, a.top(), x, a.bottom());
    }
    // Time scale under the lanes.
    p.setPen(pal.color(QPalette::PlaceholderText));
    const QRect t(a.left(), a.bottom() + 2, a.width(), 14);
    p.drawText(t, Qt::AlignLeft | Qt::AlignVCenter, "00:00:00");
    p.drawText(t, Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(slotClock(total(), m_r->fps)));
}

// ---------------------------------------------------------------------------
// window

ReportWindow* ReportWindow::open(const QString& path, QWidget* parent) {
    auto* w = new ReportWindow(parent);
    QString err;
    if (!w->load(path, err)) {
        delete w;
        return nullptr;
    }
    w->show();
    w->raise();
    return w;
}

ReportWindow::ReportWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Capture report"));
    auto* top = new QVBoxLayout(this);
    m_title = new QLabel(this);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_title->setWordWrap(true);
    top->addWidget(m_title);
    auto* vrow = new QHBoxLayout;
    m_verdict = new QLabel(this);
    QFont vf = m_verdict->font();
    vf.setPointSizeF(vf.pointSizeF() * 1.6);
    vf.setBold(true);
    m_verdict->setFont(vf);
    m_totals = new QLabel(this);
    m_totals->setWordWrap(true);
    vrow->addWidget(m_verdict);
    vrow->addSpacing(16);
    vrow->addWidget(m_totals, 1);
    top->addLayout(vrow);
    m_timeline = new ReportTimeline(this);
    top->addWidget(m_timeline);

    m_thumbScroll = new QScrollArea(this);
    m_thumbScroll->setWidgetResizable(true);
    m_thumbScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_thumbScroll->setFixedHeight(118);
    m_thumbBox = new QWidget(m_thumbScroll);
    m_thumbRow = new QHBoxLayout(m_thumbBox);
    m_thumbRow->setContentsMargins(2, 2, 2, 2);
    m_thumbRow->setSpacing(4);
    m_thumbScroll->setWidget(m_thumbBox);
    top->addWidget(m_thumbScroll);
    m_noThumbs = new QLabel(tr("No contact sheet for this capture."), this);
    m_noThumbs->setObjectName("panelLabel");
    m_noThumbs->hide();
    top->addWidget(m_noThumbs);

    auto* split = new QSplitter(Qt::Horizontal, this);
    m_events = new QTreeWidget(split);
    m_events->setHeaderLabels({tr("Time"), tr("Lane"), tr("Event"), tr("Details")});
    m_events->setRootIsDecorated(false);
    m_events->setUniformRowHeights(true);
    m_events->header()->setStretchLastSection(true);
    auto* right = new QWidget(split);
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    m_frame = new QLabel(right);
    m_frame->setAlignment(Qt::AlignCenter);
    m_frame->setMinimumSize(320, 240);
    m_frame->setStyleSheet(QString("background: %1;").arg(StatusColor::PreviewSurround.name()));
    rv->addWidget(m_frame, 1);
    m_info = new QLabel(tr("Click an event, a thumbnail or the timeline to see that frame."), right);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rv->addWidget(m_info);
    m_copy = new QPushButton(tr("Copy timecode"), right);
    m_copy->setEnabled(false);
    rv->addWidget(m_copy, 0, Qt::AlignLeft);
    split->addWidget(m_events);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    top->addWidget(split, 1);

    auto* row = new QHBoxLayout;
    auto* folder = new QPushButton(tr("Open folder"), this);
    auto* verify = new QPushButton(tr("Verify..."), this);
    auto* close = new QPushButton(tr("Close"), this);
    row->addWidget(folder);
    row->addWidget(verify);
    row->addStretch(1);
    row->addWidget(close);
    top->addLayout(row);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(folder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QString::fromStdString(m_rec.basePath)).absolutePath()));
    });
    connect(verify, &QPushButton::clicked, this, [this] { emit verifyRequested(QString::fromStdString(m_rec.mediaPath)); });
    connect(m_copy, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(m_clock); });
    connect(m_timeline, &ReportTimeline::eventClicked, this, [this](int i) { selectEvent(i); });
    connect(m_timeline, &ReportTimeline::slotClicked, this, [this](int64_t s) { selectSlot(s, QString()); });
    connect(m_events, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (!it) return;
        const int row = m_events->indexOfTopLevelItem(it);
        if (row >= 0 && size_t(row) < m_rows.size()) selectEvent(m_rows[size_t(row)]);
    });
    resize(1000, 760);
}

bool ReportWindow::load(const QString& path, QString& err) {
    std::string e;
    if (!loadCaptureRecord(path.toStdString(), m_rec, e)) {
        err = QString::fromStdString(e);
        return false;
    }
    const QString name = QFileInfo(QString::fromStdString(m_rec.mediaPath.empty() ? m_rec.basePath : m_rec.mediaPath)).fileName();
    setWindowTitle(tr("Capture report: %1").arg(name));
    QString title = m_rec.label.empty() ? name : QString::fromStdString(m_rec.label) + "  (" + name + ")";
    QStringList meta;
    if (!m_rec.startTime.empty()) meta << QString::fromStdString(m_rec.startTime);
    if (!m_rec.device.empty()) meta << QString::fromStdString(m_rec.device);
    if (!m_rec.input.empty()) meta << QString::fromStdString(m_rec.input);
    if (!m_rec.standard.empty()) meta << QString::fromStdString(m_rec.standard);
    if (!m_rec.stopReason.empty()) meta << tr("stopped: %1").arg(QString::fromStdString(m_rec.stopReason));
    m_title->setText("<b>" + title.toHtmlEscaped() + "</b><br>" + meta.join(", ").toHtmlEscaped());
    m_verdict->setText(QString::fromStdString(m_rec.verdict()));
    m_verdict->setStyleSheet(QString("color: %1;").arg((m_rec.clean() ? StatusColor::Ok : StatusColor::Warn).name()));
    auto tot = [&](const char* k) { return qint64(m_rec.totals.count(k) ? m_rec.totals.at(k) : 0); };
    QStringList t;
    t << humanDuration(m_rec.seconds) << count(m_rec.frames, "frame", "frames");
    t << tr("%1 inserted").arg(tot("inserted")) << tr("%1 dropped").arg(tot("dropped"))
      << count(tot("driver_drops"), "driver drop", "driver drops") << tr("%1 buffer overflow").arg(tot("buffer_overflow"))
      << count(tot("xruns"), "xrun", "xruns");
    if (m_rec.totals.count("level_warnings")) t << count(tot("level_warnings"), "level warning", "level warnings");
    if (m_rec.totals.count("audio_faults")) t << count(tot("audio_faults"), "audio fault", "audio faults");
    if (!m_rec.chapters.empty()) t << count(qint64(m_rec.chapters.size()), "chapter", "chapters");
    if (!m_rec.haveJson) t << tr("no .json sidecar");
    if (!m_rec.haveCsv) t << tr("no .log.csv sidecar");
    m_totals->setText(t.join(", "));
    m_timeline->setRecord(&m_rec);

    m_events->clear();
    m_rows.clear();
    for (size_t i = 0; i < m_rec.events.size(); ++i) {
        const RecordEvent& ev = m_rec.events[i];
        if (ev.lane == ReportLane::Info || ev.slot < 0) continue;
        auto* it = new QTreeWidgetItem(m_events);
        it->setText(0, QString::fromStdString(m_rec.clock(ev.slot)));
        it->setText(1, tr(laneName(ev.lane)));
        it->setText(2, QString::fromStdString(ev.label));
        it->setText(3, QString::fromStdString(ev.detail));
        it->setForeground(1, laneColor(ev.lane));
        m_rows.push_back(int(i));
    }
    if (m_rows.empty()) {
        auto* it = new QTreeWidgetItem(m_events);
        it->setText(2, tr("Nothing to review."));
        it->setFlags(Qt::NoItemFlags);
    }
    for (int c = 0; c < 3; ++c) m_events->resizeColumnToContents(c);
    buildThumbs();
    return true;
}

void ReportWindow::buildThumbs() {
    while (QLayoutItem* li = m_thumbRow->takeAt(0)) {
        delete li->widget();
        delete li;
    }
    const ContactSheetInfo& cs = m_rec.sheet;
    const bool have = !cs.thumbs.empty() && !cs.pages.empty();
    m_thumbScroll->setVisible(have);
    m_noThumbs->setVisible(!have);
    if (!have) return;
    QMap<int, QImage> pages;
    for (const ContactThumb& t : cs.thumbs) {
        if (t.page < 0 || size_t(t.page) >= cs.pages.size()) continue;
        if (!pages.contains(t.page)) pages[t.page] = QImage(QString::fromStdString(cs.pages[size_t(t.page)]));
        const QImage& page = pages[t.page];
        if (page.isNull()) continue;
        const QImage tile = page.copy(cs.tileX(t.index), cs.tileY(t.index), cs.tileWidth, cs.tileHeight + cs.labelHeight);
        auto* b = new QToolButton(m_thumbBox);
        b->setIcon(QPixmap::fromImage(tile.scaledToHeight(96, Qt::SmoothTransformation)));
        b->setIconSize(QSize(96 * tile.width() / std::max(1, tile.height()), 96));
        b->setAutoRaise(true);
        b->setToolTip(QString::fromStdString(m_rec.clock(t.slot)));
        const int64_t slot = t.slot;
        connect(b, &QToolButton::clicked, this, [this, slot] { selectSlot(slot, tr("Contact sheet thumbnail.")); });
        m_thumbRow->addWidget(b);
    }
    m_thumbRow->addStretch(1);
}

void ReportWindow::selectEvent(int index) {
    if (index < 0 || size_t(index) >= m_rec.events.size()) return;
    const RecordEvent& e = m_rec.events[size_t(index)];
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_rows[r] == index) {
            QTreeWidgetItem* it = m_events->topLevelItem(int(r));
            if (it && m_events->currentItem() != it) {
                QSignalBlocker block(m_events);
                m_events->setCurrentItem(it);
            }
        }
    QString details = QString::fromStdString(e.label);
    if (!e.detail.empty()) details += ": " + QString::fromStdString(e.detail);
    if (e.endSlot > e.slot) details += "\n" + tr("Until %1.").arg(QString::fromStdString(m_rec.clock(e.endSlot)));
    selectSlot(e.slot, details, index);
}

void ReportWindow::selectSlot(int64_t slot, const QString& details, int eventIndex) {
    m_clock = QString::fromStdString(m_rec.clock(slot));
    m_copy->setEnabled(true);
    m_info->setText(tr("Frame %1 at %2").arg(slot).arg(m_clock) + (details.isEmpty() ? QString() : "\n" + details));
    m_timeline->setSelected(eventIndex, slot);
    showFrame(slot);
}

void ReportWindow::showFrame(int64_t slot) {
    std::string file;
    int64_t frame = 0;
    if (!m_rec.locate(slot, file, frame)) {
        m_frame->setText(tr("The capture file is not here."));
        return;
    }
    m_wantSlot = slot;
    m_frame->setText(tr("Decoding..."));
    QPointer<ReportWindow> self(this);
    std::thread([self, file, frame, slot] {
        RgbImage img;
        std::string err;
        const bool ok = decodeFrameRgb(file, frame, img, err);
        QMetaObject::invokeMethod(qApp, [self, ok, img = std::move(img), err, slot] {
            if (!self || self->m_wantSlot != slot) return;
            if (!ok) {
                self->m_frame->setText(tr("Cannot show this frame: %1").arg(QString::fromStdString(err)));
                return;
            }
            QImage q(img.rgb.data(), img.width, img.height, img.width * 3, QImage::Format_RGB888);
            // Display width from the pixel aspect, so circles look round.
            const int dispW = int(std::lround(double(img.width) * img.sar.toDouble()));
            QImage shown = q.scaled(dispW, img.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            const QSize box = self->m_frame->size();
            self->m_frame->setPixmap(QPixmap::fromImage(shown.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }, Qt::QueuedConnection);
    }).detach();
}

}  // namespace pd
