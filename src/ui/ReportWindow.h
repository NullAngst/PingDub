// SPDX-License-Identifier: GPL-3.0-or-later
// The capture report: answers "is this capture good, or do I re-run it" in
// seconds. A verdict line, a timeline with one lane each for video, levels
// and audio events and the chapter marks along the top, the contact sheet as
// a thumbnail strip, and for any event the frame decoded from the file.
// Opens after each capture, and from File > Open report for any PingDub
// capture, reading its .json and .log.csv sidecars.
#pragma once

#include "engine/CaptureRecord.h"

#include <QImage>
#include <QWidget>

#include <vector>

class QLabel;
class QPushButton;
class QScrollArea;
class QTreeWidget;
class QHBoxLayout;

namespace pd {

class ReportTimeline : public QWidget {
    Q_OBJECT
public:
    explicit ReportTimeline(QWidget* parent = nullptr);
    void setRecord(const CaptureRecord* r);
    void setSelected(int eventIndex, int64_t slot);
    QSize sizeHint() const override { return {800, 104}; }

signals:
    void eventClicked(int eventIndex);
    void slotClicked(int64_t slot);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    bool event(QEvent* e) override;

private:
    QRect lanesRect() const;
    int64_t total() const;
    double xOf(int64_t slot) const;
    int hit(const QPoint& p) const;

    const CaptureRecord* m_r = nullptr;
    int m_selEvent = -1;
    int64_t m_selSlot = -1;
};

class ReportWindow : public QWidget {
    Q_OBJECT
public:
    explicit ReportWindow(QWidget* parent = nullptr);
    // Accepts the capture file, its .json or its .log.csv.
    bool load(const QString& path, QString& err);
    const CaptureRecord& record() const { return m_rec; }
    static ReportWindow* open(const QString& path, QWidget* parent);

signals:
    void verifyRequested(const QString& capture);

private:
    void buildThumbs();
    void selectEvent(int index);
    void selectSlot(int64_t slot, const QString& details, int eventIndex = -1);
    void showFrame(int64_t slot);

    CaptureRecord m_rec;
    std::vector<int> m_rows;  // tree row -> event index
    QLabel* m_title;
    QLabel* m_verdict;
    QLabel* m_totals;
    ReportTimeline* m_timeline;
    QScrollArea* m_thumbScroll;
    QWidget* m_thumbBox;
    QHBoxLayout* m_thumbRow;
    QLabel* m_noThumbs;
    QTreeWidget* m_events;
    QLabel* m_frame;
    QLabel* m_info;
    QPushButton* m_copy;
    QString m_clock;
    int64_t m_wantSlot = -1;
};

}  // namespace pd
