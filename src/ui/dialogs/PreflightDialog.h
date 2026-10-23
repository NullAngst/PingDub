// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/CaptureSession.h"

#include <QDialog>

namespace pd {

// Warnings before a capture, never blocks: data rate times duration against
// free space, signal present, standard matches detection, audio delivering.
class PreflightDialog : public QDialog {
    Q_OBJECT
public:
    PreflightDialog(const std::vector<PreflightItem>& items, QWidget* parent = nullptr);
    static bool needsAttention(const std::vector<PreflightItem>& items);
};

}  // namespace pd
