// SPDX-License-Identifier: GPL-3.0-or-later
#include "SomagicSetupDialog.h"

#include "backends/SomagicBackend.h"
#include "engine/SysInfo.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

namespace pd {

static const char* kRulePath = "/etc/udev/rules.d/70-pingdub-somagic.rules";

SomagicSetupDialog::SomagicSetupDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Somagic EasyCap setup (experimental)"));
    auto* top = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Somagic EasyCaps have no kernel driver. PingDub drives them over libusb. That needs "
                                "three things, once:\n\n"
                                "1. The firmware, which only exists inside Somagic's Windows driver (SmiUsbGrabber3C.sys, "
                                "from the CD or the vendor's download). It cannot be shipped with PingDub.\n"
                                "2. A udev rule so your user can talk to the device.\n"
                                "3. Uploading the firmware each time the device is plugged in (it shows up as 1c88:0007 "
                                "until then)."),
                             this);
    intro->setWordWrap(true);
    top->addWidget(intro);
    auto* b1 = new QPushButton(tr("1. Extract firmware from the Windows driver..."), this);
    auto* b2 = new QPushButton(tr("2. Install udev rule..."), this);
    auto* b3 = new QPushButton(tr("3. Upload firmware to the device"), this);
    connect(b1, &QPushButton::clicked, this, &SomagicSetupDialog::extract);
    connect(b2, &QPushButton::clicked, this, &SomagicSetupDialog::installRule);
    connect(b3, &QPushButton::clicked, this, &SomagicSetupDialog::initDevice);
    top->addWidget(b1);
    top->addWidget(b2);
    top->addWidget(b3);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    top->addWidget(m_status);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
    refresh();
    resize(560, 0);
}

void SomagicSetupDialog::refresh() {
    QStringList s;
    s << (fileExists(SomagicBackend::firmwarePath()) ? tr("Firmware: installed at %1").arg(QString::fromStdString(SomagicBackend::firmwarePath()))
                                                    : tr("Firmware: not installed"));
    s << (fileExists(kRulePath) ? tr("udev rule: installed") : tr("udev rule: not installed"));
    const auto devs = SomagicBackend::detect();
    if (devs.empty()) s << tr("Device: none connected");
    for (const auto& d : devs)
        s << tr("Device: 1c88:%1 at USB %2, %3")
                 .arg(d.pid, 4, 16, QChar('0'))
                 .arg(QString::fromStdString(d.port))
                 .arg(d.needsFirmware ? tr("needs firmware") : tr("ready"));
    m_status->setText(s.join("\n"));
}

void SomagicSetupDialog::extract() {
    const QString f = QFileDialog::getOpenFileName(this, tr("Somagic Windows driver"), QString(),
                                                   tr("Windows driver (*.sys *.SYS);;All files (*)"));
    if (f.isEmpty()) return;
    std::string err;
    if (SomagicBackend::installFirmwareFromDriver(f.toStdString(), err))
        QMessageBox::information(this, windowTitle(), tr("Firmware extracted."));
    else
        QMessageBox::warning(this, windowTitle(), QString::fromStdString(err));
    refresh();
}

void SomagicSetupDialog::installRule() {
    const QString rule = QString::fromStdString(SomagicBackend::udevRuleText());
    QMessageBox box(QMessageBox::Question, windowTitle(),
                    tr("PingDub will write this to %1 with pkexec (you will be asked for your password), then reload "
                       "udev:\n\n%2")
                        .arg(kRulePath, rule),
                    QMessageBox::Ok | QMessageBox::Cancel, this);
    if (box.exec() != QMessageBox::Ok) return;
    QProcess p;
    p.start("pkexec", {"sh", "-c",
                       QString("cat > %1 && udevadm control --reload-rules && udevadm trigger --subsystem-match=usb").arg(kRulePath)});
    if (!p.waitForStarted(5000)) {
        QMessageBox::warning(this, windowTitle(), tr("pkexec is not available. Write the rule above to %1 as root.").arg(kRulePath));
        return;
    }
    p.write(rule.toUtf8());
    p.closeWriteChannel();
    p.waitForFinished(120000);
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        QMessageBox::warning(this, windowTitle(), tr("Installing the rule failed: %1").arg(QString::fromUtf8(p.readAllStandardError())));
    refresh();
}

void SomagicSetupDialog::initDevice() {
    std::string err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = SomagicBackend::initDevice(err);
    QApplication::restoreOverrideCursor();
    if (ok) QMessageBox::information(this, windowTitle(), tr("Firmware uploaded. The device is ready."));
    else QMessageBox::warning(this, windowTitle(), QString::fromStdString(err));
    refresh();
}

}  // namespace pd
