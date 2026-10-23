// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "devices/Profiles.h"

#include <QDialog>
#include <QMap>

class QComboBox;
class QLabel;
class QVBoxLayout;

namespace pd {

class SessionController;

// Input, standard, format, size, field order override, every driver control.
// Controls apply live to the preview; Cancel puts them back.
class VideoSourceDialog : public QDialog {
    Q_OBJECT
public:
    VideoSourceDialog(SessionController* ctl, QWidget* parent = nullptr);
    Profile result() const { return m_profile; }

protected:
    void reject() override;
    void accept() override;

private:
    void buildControls(QVBoxLayout* into);
    SessionController* m_ctl;
    Profile m_profile;
    QComboBox* m_input = nullptr;
    QComboBox* m_std = nullptr;
    QComboBox* m_fmt = nullptr;
    QComboBox* m_field = nullptr;
    QComboBox* m_sar = nullptr;
    QLabel* m_detected = nullptr;
    QMap<quint32, qint64> m_original;
};

}  // namespace pd
