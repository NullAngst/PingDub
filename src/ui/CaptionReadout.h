// SPDX-License-Identifier: GPL-3.0-or-later
// Shows decoded CC1 while capturing, so you know captions are coming through.
#pragma once

#include <QLabel>

namespace pd {

class CaptionReadout : public QLabel {
    Q_OBJECT
public:
    explicit CaptionReadout(QWidget* parent = nullptr);
    void setCaption(bool vbiActive, const QString& text);
};

}  // namespace pd
