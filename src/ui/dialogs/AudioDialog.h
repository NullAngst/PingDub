// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "devices/Profiles.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QTimer;

namespace pd {

class SessionController;
class Meters;

// Device, rate, format, channels, mixer controls, and a test meter.
class AudioDialog : public QDialog {
    Q_OBJECT
public:
    AudioDialog(SessionController* ctl, QWidget* parent = nullptr);

protected:
    void accept() override;

private:
    SessionController* m_ctl;
    Profile m_profile;
    QComboBox* m_device;
    QComboBox* m_rate;
    QLabel* m_format;
    Meters* m_meter;
    QTimer* m_timer;
};

}  // namespace pd
