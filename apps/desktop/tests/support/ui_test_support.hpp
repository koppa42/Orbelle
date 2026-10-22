#pragma once

#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QThread>
#include <memory>

namespace orbelle::desktop {
// A test binary may contain several suites. Qt permits one QApplication per process.
inline QApplication &test_application(const char *application_name) {
    static int argc = 1;
    // Preserve each executable's settings/cache namespace when run through CTest.
    static QByteArray name(application_name);
    static char *argv[] = {name.data(), nullptr};
    static std::unique_ptr<QApplication> app;
    if (!QApplication::instance())
        app = std::make_unique<QApplication>(argc, argv);
    return *qApp;
}

template <typename Predicate> bool wait_for_ui(Predicate ready, int timeout_ms = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return ready();
}
} // namespace orbelle::desktop
