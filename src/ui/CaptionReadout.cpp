// SPDX-License-Identifier: GPL-3.0-or-later
#include "CaptionReadout.h"

namespace pd {

CaptionReadout::CaptionReadout(QWidget* parent) : QLabel(parent) {
    setAlignment(Qt::AlignCenter);
    setWordWrap(true);
    setMinimumHeight(22);
    QFont f = font();
    f.setFamily("monospace");
    f.setStyleHint(QFont::Monospace);
    setFont(f);
    setStyleSheet("background: #101010; color: #F0F0F0; padding: 3px; border-radius: 3px;");
    setToolTip(tr("Line 21 closed captions (CC1), decoded live from the VBI"));
    hide();
}

void CaptionReadout::setCaption(bool vbiActive, const QString& text) {
    setVisible(vbiActive);
    if (!vbiActive) return;
    setText(text.isEmpty() ? tr("CC1: (no caption on screen)") : text);
}

}  // namespace pd
