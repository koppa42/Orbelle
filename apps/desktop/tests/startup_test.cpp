#include "../src/viewer/project_update.hpp"
#include "../src/viewer/startup_resources.hpp"
#include "../src/viewer/workbench_style.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <psapi.h>
#endif

namespace orbitforge::desktop {
namespace {
QApplication &startup_app() {
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance()))
        return *app;
    static int argc = 1;
    static char name[] = "startup_test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    apply_workbench_style(app);
    return app;
}
void capture(StartupSplash &splash, const QString &name) {
    const auto directory = qEnvironmentVariable("ORBITFORGE_STARTUP_CAPTURE_DIR");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        EXPECT_TRUE(splash.grab().save(directory + "/" + name + ".png"));
    }
}
runtime::CatalogGravity fixture(const QString &directory) {
    QFile file(directory + "/gravity.gfc");
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write test gravity");
    file.write("product_type gravity_field\nmodelname Fixture\n"
               "earth_gravity_constant 3.986004415e14\nradius 6378136.3\n"
               "max_degree 2\ntide_system tide_free\nend_of_head\n"
               "gfc 0 0 1 0\ngfc 2 0 -0.000484165371736 0\ngfc 2 1 0 0\ngfc 2 2 0 0\n");
    file.close();
    const auto path = std::filesystem::path(file.fileName().toStdWString());
    return {"egm2008", runtime::load_gravity(path)->info(), path};
}
qint64 private_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)))
        return static_cast<qint64>(memory.PrivateUsage);
#endif
    return -1;
}
} // namespace

TEST(Startup, WorkerProgressKeepsWindowResponsiveAndClickDoesNotDismissIt) {
    auto &app = startup_app();
    StartupSplash splash(app.primaryScreen());
    splash.show();
    const auto gui_thread = std::this_thread::get_id();
    std::atomic_bool first_done{false}, release{false}, background{false};
    int ticks = 0;
    bool observed_first = false, observed_second = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &splash, [&] {
        ++ticks;
        observed_first |= splash.message().contains("1 / 2");
        observed_second |= splash.message().contains("2 / 2");
        EXPECT_TRUE(splash.isVisible());
        if (ticks == 2) {
            QMouseEvent click(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&splash, &click);
            EXPECT_TRUE(splash.isVisible());
            capture(splash, "loading-gravity");
        }
        if (ticks == 6)
            first_done = true;
        if (ticks >= 12)
            release = true;
    });
    timer.start(20);
    ASSERT_TRUE(splash.prepare({{"正在加载并校验 EGM2008 重力场…",
                                 [&](const auto &checkpoint) {
                                     background = std::this_thread::get_id() != gui_thread;
                                     while (!first_done) {
                                         checkpoint();
                                         std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                     }
                                 }},
                                {"正在准备 DE440s 太阳系星历…", [&](const auto &checkpoint) {
                                     while (!release) {
                                         checkpoint();
                                         std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                     }
                                 }}}));
    EXPECT_TRUE(background);
    EXPECT_TRUE(observed_first);
    EXPECT_TRUE(observed_second);
    EXPECT_GE(ticks, 12);
    EXPECT_EQ(splash.findChild<QProgressBar *>("startupProgress")->value(), 2);
    EXPECT_EQ(splash.message(), "数据已就绪，正在打开工作台…");
    capture(splash, "ready");
}

TEST(Startup, ButtonEscapeAndCloseCancelAndJoinBeforeReturning) {
    auto &app = startup_app();
    for (int action = 0; action != 3; ++action) {
        StartupSplash splash(app.primaryScreen());
        std::atomic_bool started{false}, stopped{false};
        bool second = false;
        QTimer cancel_timer;
        QObject::connect(&cancel_timer, &QTimer::timeout, &splash, [&] {
            if (!started)
                return;
            cancel_timer.stop();
            if (action == 0)
                splash.findChild<QPushButton *>("startupCancel")->click();
            else if (action == 1) {
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(&splash, &escape);
            } else
                splash.close();
            EXPECT_EQ(splash.message(), "正在取消启动…");
            EXPECT_FALSE(splash.findChild<QPushButton *>("startupCancel")->isEnabled());
            capture(splash, "cancelling");
        });
        cancel_timer.start(20);
        EXPECT_FALSE(splash.prepare({{"loading",
                                      [&](const auto &checkpoint) {
                                          started = true;
                                          try {
                                              for (;;) {
                                                  checkpoint();
                                                  std::this_thread::sleep_for(
                                                      std::chrono::milliseconds(1));
                                              }
                                          } catch (...) {
                                              stopped = true;
                                              throw;
                                          }
                                      }},
                                     {"next", [&](const auto &) { second = true; }}}));
        EXPECT_TRUE(stopped);
        EXPECT_FALSE(second);
    }
}

TEST(Startup, FailedPreparationCanRetryAndNeverCachesInvalidIdentity) {
    auto &app = startup_app();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto file = fixture(directory.path());
    auto bad = file;
    bad.info.sha256 = std::string(64, '0');
    StartupResources cache;
    StartupSplash splash(app.primaryScreen());
    EXPECT_THROW(splash.prepare(startup_resource_steps(cache, {bad}, {})), std::runtime_error);
    EXPECT_TRUE(cache.gravity.empty());
    ASSERT_TRUE(splash.prepare(startup_resource_steps(cache, {file}, {})));
    const auto loaded = cache.gravity.at("egm2008");
    // An immutable loaded provider remains useful without re-reading the source.
    ASSERT_TRUE(QFile::remove(QString::fromStdWString(file.file.wstring())));
    ASSERT_TRUE(splash.prepare(startup_resource_steps(cache, {file}, {})));
    EXPECT_EQ(cache.gravity.at("egm2008"), loaded);
}

TEST(Startup, DefaultCacheSurvivesSceneReleaseButDropsReplacedAndUnselectedModels) {
    auto &app = startup_app();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto file = fixture(directory.path());
    StartupResources cache;
    StartupSplash splash(app.primaryScreen());
    ASSERT_TRUE(splash.prepare(startup_resource_steps(cache, {file}, {})));
    std::weak_ptr<const core::GravityModel> original = cache.gravity.at("egm2008");
    int loads = 0;
    const auto prepare = [&](const StartupResources::Gravity &active) {
        return prepare_resources(
            std::vector{file}, active, runtime::gravity_reference,
            [](const auto &model) -> const auto & { return model.info(); },
            [&](const auto &record) {
                ++loads;
                return runtime::load_gravity(record.file);
            },
            [] {}, cache.gravity);
    };
    auto active = prepare({});
    active.clear(); // New scenario: application cache survives.
    active = prepare({});
    EXPECT_EQ(loads, 0);
    EXPECT_EQ(active.at("egm2008"), original.lock());
    auto unrelated = active;
    unrelated.emplace("custom", core::builtin_egm96());
    cache.remember(unrelated, {});
    EXPECT_EQ(cache.gravity.size(), 1);
    unrelated.clear();
    file.info.solid_tide_k20 += 0.001; // Same bytes, changed interpretation: must not reuse.
    cache.reconcile({file}, {});
    EXPECT_TRUE(cache.gravity.empty());
    EXPECT_FALSE(original.expired()); // A running scene still owns its input.
    EXPECT_THROW(prepare(active), std::runtime_error);
    EXPECT_EQ(loads, 1);
    active.clear();
    EXPECT_TRUE(original.expired());
}

TEST(Startup, FullDefaultDataWarmupMemoryAndRepeatedReuse) {
    const auto input = qEnvironmentVariable("ORBITFORGE_STARTUP_GRAVITY_FILE");
    if (input.isEmpty())
        GTEST_SKIP() << "Optional full EGM2008 memory/profile fixture";
    auto &app = startup_app();
    StartupSplash splash(app.primaryScreen());
    splash.show();
    app.processEvents();
    // The pinned metadata matches the complete reference file; do not parse it
    // before the measured preparation (which would warm the allocator too).
    core::GravityInfo info{"EGM2008",
                           "ab5b524da073e63b5bdceb7ca47a0de07a26dd44a1c5798f39fc98dc80af70fd",
                           "",
                           "tide_free",
                           3.986004415e14,
                           6378136.3,
                           2190,
                           2190};
    runtime::CatalogGravity file{"egm2008", info, std::filesystem::path(input.toStdWString())};
    std::vector<runtime::CatalogEphemeris> ephemerides;
    const auto catalog = qEnvironmentVariable("ORBITFORGE_STARTUP_EPHEMERIS_CATALOG");
    if (!catalog.isEmpty())
        ephemerides =
            runtime::DataCatalog(std::filesystem::path(catalog.toStdWString())).ephemerides();
    StartupResources cache;
    const auto before = private_bytes();
    qint64 peak = before;
    // Sample independently of GUI painting so a screenshot cannot hide a short
    // allocation peak. Only the sampler writes peak until it has been joined.
    std::jthread monitor([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            peak = std::max(peak, private_bytes());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    QTimer screenshot;
    QString captured_stage;
    QObject::connect(&screenshot, &QTimer::timeout, &splash, [&] {
        if (captured_stage != splash.message()) {
            captured_stage = splash.message();
            capture(splash, "full-default-loading");
        }
    });
    screenshot.start(100);
    QElapsedTimer clock;
    clock.start();
    ASSERT_TRUE(splash.prepare(startup_resource_steps(cache, {file}, ephemerides)));
    const auto elapsed = clock.elapsed(), retained = private_bytes();
    monitor.request_stop();
    monitor.join();
    peak = std::max(peak, retained);
    screenshot.stop();
    int loads = 0;
    const auto model = cache.gravity.at("egm2008");
    for (int i = 0; i < 10; ++i) {
        const auto next = prepare_resources(
            std::vector{file}, StartupResources::Gravity{}, runtime::gravity_reference,
            [](const auto &value) -> const auto & { return value.info(); },
            [&](const auto &) {
                ++loads;
                return model;
            },
            [] {}, cache.gravity);
        EXPECT_EQ(next.at("egm2008"), model);
        cache.remember(next, {});
    }
    EXPECT_EQ(loads, 0);
    if (!ephemerides.empty()) {
        const auto reused = prepare_resources(
            ephemerides, StartupResources::Ephemerides{}, runtime::ephemeris_reference,
            [](const auto &value) -> const auto & { return value.data_info(); },
            [&](const auto &) -> std::shared_ptr<const core::IThirdBodyEphemeris> {
                throw std::runtime_error("Warm ephemeris was reloaded");
            },
            [] {}, cache.ephemerides);
        EXPECT_EQ(reused.at("de440s"), cache.ephemerides.at("de440s"));
    }
    const auto after_reuse = private_bytes();
    EXPECT_LE(after_reuse - retained, 4 * 1024 * 1024);
    const QJsonObject report{{"warmup_ms", elapsed},
                             {"private_bytes_before", before},
                             {"private_bytes_retained", retained},
                             {"private_bytes_sampled_peak", peak},
                             {"private_bytes_after_ten_reuses", after_reuse},
                             {"repeat_loads", loads},
                             {"gravity_providers", static_cast<int>(cache.gravity.size())},
                             {"ephemeris_providers", static_cast<int>(cache.ephemerides.size())}};
    const auto output = qEnvironmentVariable("ORBITFORGE_STARTUP_CAPTURE_DIR");
    if (!output.isEmpty()) {
        QFile result(output + "/memory.json");
        ASSERT_TRUE(result.open(QIODevice::WriteOnly));
        result.write(QJsonDocument(report).toJson());
    }
    std::cout << QJsonDocument(report).toJson().constData();
    capture(splash, "full-default-ready");
}
} // namespace orbitforge::desktop
