// SPDX-License-Identifier: GPL-3.0-or-later
#include "DiagnosticsDialog.h"

#include "SomagicSetupDialog.h"
#include "ui/SessionController.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace pd {

DiagnosticsDialog::DiagnosticsDialog(SessionController* ctl, QWidget* parent) : QDialog(parent), m_ctl(ctl) {
    setWindowTitle(tr("Diagnostics"));
    auto* top = new QVBoxLayout(this);
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    top->addWidget(m_text, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    auto* refreshBtn = bb->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    auto* somagic = bb->addButton(tr("Somagic setup..."), QDialogButtonBox::ActionRole);
    // Out-of-tree drivers (the GV-USB2): open the driver's repository.
    m_driverPage = bb->addButton(tr("Driver page"), QDialogButtonBox::ActionRole);
    connect(m_driverPage, &QPushButton::clicked, this, [this] {
        for (const auto& s : m_ctl->scanner()->driverSources()) QDesktopServices::openUrl(QUrl(QString::fromStdString(s.link)));
    });
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_text->toPlainText()); });
    connect(refreshBtn, &QPushButton::clicked, this, [this] {
        m_ctl->scanner()->rescan();
        this->refresh();
    });
    connect(somagic, &QPushButton::clicked, this, [this] {
        SomagicSetupDialog d(this);
        d.exec();
        m_ctl->scanner()->rescan();
        this->refresh();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
    refresh();
    resize(860, 720);
}

QString DiagnosticsDialog::report(SessionController* ctl) {
    QString r;
    r += QString("PingDub %1, Qt %2, FFmpeg libavcodec %3 / libavformat %4\n")
             .arg(PINGDUB_VERSION, qVersion(), AV_STRINGIFY(LIBAVCODEC_VERSION), AV_STRINGIFY(LIBAVFORMAT_VERSION));
    r += QString("System: %1, kernel %2, %3\n\n")
             .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(), QGuiApplication::platformName());
    r += QString::fromStdString(ctl->scanner()->hardwareReport()) + "\n";
    CaptureSession& s = ctl->session();
    if (VideoBackend* v = s.video()) {
        r += "=== Open video device (" + QString::fromStdString(v->backendName()) + ") ===\n";
        r += QString::fromStdString(v->diagnostics()) + "\n";
        const VideoFormat& f = s.videoFormat();
        r += QString("Capture format: %1 %2x%3, %4/%5 fps, field %6, SAR %7:%8, %9\n\n")
                 .arg(pixFmtName(f.pixfmt))
                 .arg(f.width)
                 .arg(f.height)
                 .arg(f.fps.num)
                 .arg(f.fps.den)
                 .arg(fieldOrderName(f.field))
                 .arg(f.sar.num)
                 .arg(f.sar.den)
                 .arg(QString::fromStdString(f.standardName));
    } else {
        r += "No video device open.\n\n";
    }
    if (AudioBackend* a = s.audio()) {
        r += "=== Open audio device ===\n" + QString::fromStdString(a->diagnostics());
        const AudioFormat& af = s.audioFormat();
        r += QString("Capture format: %1, %2 Hz, %3 ch\n\n").arg(sampleFmtName(af.fmt)).arg(af.rate).arg(af.channels);
    }
    const auto warns = s.warnings();
    if (!warns.empty()) {
        r += "=== Warnings ===\n";
        for (const auto& w : warns) r += QString::fromStdString(w) + "\n";
    }
    return r;
}

void DiagnosticsDialog::refresh() {
    m_text->setPlainText(report(m_ctl));
    const auto& src = m_ctl->scanner()->driverSources();
    m_driverPage->setVisible(!src.empty());
    QStringList tips;
    for (const auto& s : src) tips << QString::fromStdString(s.name + ": " + s.link);
    m_driverPage->setToolTip(tips.join("\n"));
}

}  // namespace pd
