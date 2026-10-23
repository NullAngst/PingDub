// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

class QLabel;

namespace pd {

// One-time Somagic EasyCap setup: extract firmware from the Windows driver,
// install the udev rule (through pkexec, rule text shown first), upload the
// firmware to an uninitialized device.
class SomagicSetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit SomagicSetupDialog(QWidget* parent = nullptr);

private:
    void refresh();
    void extract();
    void installRule();
    void initDevice();
    QLabel* m_status;
};

}  // namespace pd
