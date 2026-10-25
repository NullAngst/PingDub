// SPDX-License-Identifier: GPL-3.0-or-later
#include "VerifyDialog.h"

#include "engine/CaptureRecord.h"
#include "engine/Checksums.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>
#include <thread>

namespace pd {

namespace {

QString clockOf(int64_t slot, double fps) {
    const double sec = double(slot) / fps;
    const int h = int(sec / 3600), m = int(std::fmod(sec, 3600) / 60);
    return QString::asprintf("%02d:%02d:%06.3f", h, m, std::fmod(sec, 60));
}

QString frameList(const std::vector<int64_t>& v, int64_t start, double fps) {
    QStringList s;
    for (size_t i = 0; i < v.size() && i < 20; ++i) s << QObject::tr("frame %1 (%2)").arg(v[i] + start).arg(clockOf(v[i] + start, fps));
    if (v.size() > 20) s << QObject::tr("and %1 more").arg(v.size() - 20);
    return s.join(", ");
}

}  // namespace

VerifyDialog::VerifyDialog(const QString& capture, QWidget* parent)
    : QDialog(parent), m_cancel(std::make_shared<std::atomic<bool>>(false)), m_alive(std::make_shared<std::atomic<bool>>(true)) {
    setWindowTitle(tr("Verify capture"));
    CaptureRecord rec;
    std::string err;
    if (loadCaptureRecord(capture.toStdString(), rec, err) && !rec.segments.empty()) {
        for (const RecordSegment& s : rec.segments) {
            m_files << QString::fromStdString(s.file);
            m_starts << s.startSlot;
        }
        m_fps = rec.fps.toDouble();
    } else {
        m_files << capture;
        m_starts << 0;
    }
    auto* top = new QVBoxLayout(this);
    m_what = new QLabel(m_files.size() == 1 ? QFileInfo(m_files[0]).fileName()
                                           : tr("%1 files: %2 and the rest of the capture").arg(m_files.size()).arg(QFileInfo(m_files[0]).fileName()),
                        this);
    m_what->setWordWrap(true);
    top->addWidget(m_what);
    auto* note = new QLabel(tr("Quick recomputes the SHA-256: it catches any change to the file, in seconds per gigabyte. "
                               "Full also decodes every frame and finds the exact frames that changed."),
                            this);
    note->setObjectName("panelLabel");
    note->setWordWrap(true);
    top->addWidget(note);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    top->addWidget(m_progress);
    m_out = new QPlainTextEdit(this);
    m_out->setReadOnly(true);
    top->addWidget(m_out, 1);
    auto* row = new QHBoxLayout;
    m_quick = new QPushButton(tr("Quick verify"), this);
    m_full = new QPushButton(tr("Full verify"), this);
    m_close = new QPushButton(tr("Close"), this);
    row->addWidget(m_quick);
    row->addWidget(m_full);
    row->addStretch(1);
    row->addWidget(m_close);
    top->addLayout(row);
    connect(m_quick, &QPushButton::clicked, this, [this] { run(false); });
    connect(m_full, &QPushButton::clicked, this, [this] { run(true); });
    connect(m_close, &QPushButton::clicked, this, &VerifyDialog::reject);
    resize(620, 380);
}

VerifyDialog::~VerifyDialog() {
    *m_alive = false;
    *m_cancel = true;
}

void VerifyDialog::reject() {
    if (m_busy) {
        *m_cancel = true;
        return;
    }
    QDialog::reject();
}

void VerifyDialog::setBusy(bool busy) {
    m_busy = busy;
    m_quick->setEnabled(!busy);
    m_full->setEnabled(!busy);
    m_close->setText(busy ? tr("Cancel") : tr("Close"));
}

void VerifyDialog::run(bool full) {
    setBusy(true);
    *m_cancel = false;
    m_out->appendPlainText(full ? tr("Full verify...") : tr("Quick verify..."));
    QPointer<VerifyDialog> self(this);
    auto cancel = m_cancel;
    const QStringList files = m_files;
    const QList<qint64> starts = m_starts;
    const double fps = m_fps;
    std::thread([self, cancel, files, starts, fps, full] {
        bool allOk = true;
        for (int i = 0; i < files.size(); ++i) {
            const int idx = i, n = int(files.size());
            const VerifyResult r = verifyCapture(files[i].toStdString(), full, [&](double f, const std::string&) {
                const int v = int(1000.0 * (double(idx) + f) / double(n));
                QMetaObject::invokeMethod(qApp, [self, v] {
                    if (self) self->m_progress->setValue(v);
                }, Qt::QueuedConnection);
                return !cancel->load();
            });
            QString text = QFileInfo(files[i]).fileName() + ": " + QString::fromStdString(r.summary());
            if (!r.badFrames.empty()) text += "\n  " + tr("Changed: %1").arg(frameList(r.badFrames, starts[i], fps));
            if (!r.decodeErrors.empty()) text += "\n  " + tr("Would not decode: %1").arg(frameList(r.decodeErrors, starts[i], fps));
            if (!r.crcErrors.empty()) text += "\n  " + tr("Slice CRC errors: %1").arg(frameList(r.crcErrors, starts[i], fps));
            if (!r.missingFrames.empty()) text += "\n  " + tr("Missing: %1").arg(frameList(r.missingFrames, starts[i], fps));
            allOk &= r.ok;
            const bool stop = r.cancelled;
            QMetaObject::invokeMethod(qApp, [self, text] {
                if (self) self->m_out->appendPlainText(text);
            }, Qt::QueuedConnection);
            if (stop) break;
        }
        QMetaObject::invokeMethod(qApp, [self, allOk, cancel] {
            if (!self) return;
            self->setBusy(false);
            self->m_progress->setValue(cancel->load() ? self->m_progress->value() : 1000);
            if (!cancel->load())
                self->m_out->appendPlainText(allOk ? tr("Result: the capture is exactly as it was written.")
                                                   : tr("Result: NOT VERIFIED, see above."));
        }, Qt::QueuedConnection);
    }).detach();
}

}  // namespace pd
