// SPDX-License-Identifier: GPL-3.0-or-later
// File > Verify capture. Quick recomputes the SHA-256 of each file and
// compares it with the .sha256 sidecar. Full also decodes every frame and
// compares it with the .framemd5 sidecar, naming the exact frames that
// changed, plus any FFV1 slice CRC errors.
#pragma once

#include <QDialog>
#include <QStringList>

#include <atomic>
#include <memory>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace pd {

class VerifyDialog : public QDialog {
    Q_OBJECT
public:
    VerifyDialog(const QString& capture, QWidget* parent = nullptr);
    ~VerifyDialog() override;

protected:
    void reject() override;

private:
    void run(bool full);
    void setBusy(bool busy);

    QStringList m_files;
    QList<qint64> m_starts;   // timeline slot where each file begins
    double m_fps = 30000.0 / 1001.0;
    QLabel* m_what;
    QProgressBar* m_progress;
    QPlainTextEdit* m_out;
    QPushButton* m_quick;
    QPushButton* m_full;
    QPushButton* m_close;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<bool>> m_alive;
    bool m_busy = false;
};

}  // namespace pd
