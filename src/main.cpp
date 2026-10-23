// SPDX-License-Identifier: GPL-3.0-or-later
// PingDub: analog video capture for Linux.
#include "devices/DeviceScanner.h"
#include "engine/CaptureSession.h"
#include "ui/MainWindow.h"
#include "ui/SessionController.h"
#include "ui/Theme.h"

#include <QApplication>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--version")) {
            std::printf("PingDub %s\n", PINGDUB_VERSION);
            return 0;
        }
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::printf("Usage: pingdub [--version] [--self-test [--verbose]]\n"
                        "  --version     print the version and exit\n"
                        "  --self-test   run a 10-second capture through test-pattern devices, verify the file, exit 0 on success\n"
                        "Environment: PINGDUB_FAKE_DEVICES=1 adds a test-pattern device to the device list.\n");
            return 0;
        }
        if (!std::strcmp(argv[i], "--verbose")) verbose = true;
    }
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--self-test") || !std::strcmp(argv[i], "--selftest")) return pd::runSelfTest(verbose);

    QApplication app(argc, argv);
    QApplication::setApplicationName("PingDub");
    QApplication::setApplicationVersion(PINGDUB_VERSION);
    QApplication::setDesktopFileName("io.github.nullangst.PingDub");
    QApplication::setWindowIcon(QIcon(":/pingdub.svg"));

    pd::AppSettings settings;
    settings.load();
    pd::ThemeManager::instance().apply(settings.theme.isEmpty() ? pd::ThemeManager::instance().systemDefault() : settings.theme);

    pd::DeviceScanner scanner;
    scanner.rescan();
    pd::MainWindow w(&scanner);
    w.show();
    return app.exec();
}
