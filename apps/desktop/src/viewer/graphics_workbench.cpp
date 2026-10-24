#include "graphics_workbench.hpp"
#include "../ui_text.hpp"
#include "application_update_ui.hpp"
#include "apply_timing.hpp"
#include "eop_catalog.hpp"
#include "epoch_editor.hpp"
#include "form_kit.hpp"
#include "ground_map.hpp"
#include "ground_station_editor.hpp"
#include "initial_data_setup.hpp"
#include "insert_object_dialog.hpp"
#include "mission_timeline.hpp"
#include "orbit_design_preview.hpp"
#include "orbit_editor.hpp"
#include "orbit_scene.hpp"
#include "project_scene.hpp"
#include "project_update.hpp"
#include "quantity_editor.hpp"
#include "report_creation.hpp"
#include "report_dialog.hpp"
#include "report_plot.hpp"
#include "report_selection.hpp"
#include "responsive_layout.hpp"
#include "satellite_creation.hpp"
#include "satellite_model.hpp"
#include "scenario_dialog.hpp"
#include "sensor_editor.hpp"
#include "startup_resources.hpp"
#include "startup_splash.hpp"
#include "view_navigation.hpp"
#include "welcome_page.hpp"
#include "workbench_profile.hpp"
#include "workbench_style.hpp"
#include "workbench_window.hpp"
#include "workspace.hpp"
#include <QUuid>
#include <numbers>
#include <orbelle/runtime/access.hpp>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/ocean_tides.hpp>
#include <orbelle/runtime/space_weather.hpp>
#ifdef ORBELLE_DESKTOP_BENCHMARK
#include "benchmark_gpu.hpp"
#include "benchmark_presentation.hpp"
#include "benchmark_run.hpp"
#include "benchmark_scene.hpp"
#endif
#include <QAction>
#include <QApplication>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QExposeEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QFormLayout>
#include <QFrame>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <orbelle/geo_rocky/globe.hpp>
#include <orbelle/render_vsg/compile_manager.hpp>
#include <orbelle/render_vsg/labels.hpp>
#include <orbelle/render_vsg/marker_layer.hpp>
#include <orbelle/render_vsg/sensor_layer.hpp>
#include <orbelle/render_vsg/swapchain_capture.hpp>
#include <orbelle/runtime/demo.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>
#include <tuple>
#include <unordered_map>
#include <vsg/all.h>
#include <vsgQt/Window.h>

namespace orbelle::desktop {
namespace {
constexpr std::array playback_rates{0.1,  0.5,  1.0,   2.0,   5.0,   10.0,
                                    20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
double adjacent_playback_rate(double current, bool faster) {
    if (faster) {
        const auto next = std::upper_bound(playback_rates.begin(), playback_rates.end(), current);
        return next == playback_rates.end() ? playback_rates.back() : *next;
    }
    const auto next = std::lower_bound(playback_rates.begin(), playback_rates.end(), current);
    return next == playback_rates.begin() ? playback_rates.front() : *std::prev(next);
}
QString display_tai(core::Epoch epoch, bool date = true) {
    // Display calendar only: TAI has fixed 86400-second days.
    const auto time = QDateTime(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC)
                          .addMSecs(epoch.tai_nanoseconds() / 1'000'000);
    return time.toString(date ? "yyyy-MM-dd  HH:mm:ss.zzz" : "HH:mm:ss");
}
struct ValidationLog {
    std::atomic_uint errors{0}, warnings{0};
    std::mutex mutex;
    QStringList messages;
    vsg::ref_ptr<vsg::Instance> instance;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    static VKAPI_ATTR VkBool32 VKAPI_CALL receive(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                  void *user) {
        auto &log = *static_cast<ValidationLog *>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            ++log.errors;
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            ++log.warnings;
        std::lock_guard lock(log.mutex);
        log.messages.push_back(QString::fromUtf8(data->pMessage));
        std::cerr << data->pMessage << '\n';
        return VK_FALSE;
    }
    void install(vsg::ref_ptr<vsg::Instance> value) {
        if (instance)
            return;
        instance = std::move(value);
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = receive;
        info.pUserData = this;
        const auto create = instance->getExtensions()->vkCreateDebugUtilsMessengerEXT;
        if (!create || create(instance->vk(), &info, nullptr, &messenger) != VK_SUCCESS) {
            throw std::runtime_error("Cannot install Vulkan validation reporting");
        }
    }
    ~ValidationLog() {
        if (messenger)
            instance->getExtensions()->vkDestroyDebugUtilsMessengerEXT(instance->vk(), messenger,
                                                                       nullptr);
    }
};

// Rebuild our rendering graphs when Qt recreates a native surface during docking.
class EmbeddedWindow final : public vsgQt::Window {
  public:
    EmbeddedWindow(vsg::ref_ptr<vsgQt::Viewer> viewer, vsg::ref_ptr<vsg::WindowTraits> traits)
        : vsgQt::Window(viewer, traits), owner_(viewer) {}
    std::function<void()> attached, detaching;
    std::function<void(QPointF)> pressed;
    std::function<void(const std::string &)> failed;
    std::function<void(const std::function<void()> &)> synchronize;
    void initializeWindow() override {
        if (windowAdapter)
            return;
        // vsgQt::cleanup clears its viewer on native surface destruction.
        viewer = owner_;
        vsgQt::Window::initializeWindow();
        if (!windowAdapter)
            throw std::runtime_error("Vulkan window creation failed");
        if (attached)
            attached();
    }

  protected:
    template <typename Function> void protect(Function function) {
        try {
            function();
        } catch (const std::exception &error) {
            if (failed)
                failed(error.what());
        } catch (const vsg::Exception &error) {
            if (failed)
                failed(error.message);
        }
    }
    void exposeEvent(QExposeEvent *event) override {
        protect([&] { vsgQt::Window::exposeEvent(event); });
    }
    void resizeEvent(QResizeEvent *event) override {
        protect([&] { exclusive([&] { vsgQt::Window::resizeEvent(event); }); });
    }
    void mousePressEvent(QMouseEvent *event) override {
        protect([&] {
            if (event->button() == Qt::LeftButton && pressed)
                pressed(event->position() * devicePixelRatio());
            vsgQt::Window::mousePressEvent(event);
        });
    }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::PlatformSurface) {
            const auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
            if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                if (!windowAdapter)
                    return QWindow::event(event);
                bool result = false;
                protect([&] {
                    exclusive([&] {
                        if (detaching)
                            protect(detaching);
                        // Upstream cleanup waits for the device too, so it shares this barrier.
                        result = vsgQt::Window::event(event);
                    });
                });
                return result;
            }
        }
        return vsgQt::Window::event(event);
    }

  private:
    void exclusive(const std::function<void()> &operation) {
        if (synchronize)
            synchronize(operation);
        else
            operation();
    }
    vsg::ref_ptr<vsgQt::Viewer> owner_;
};

// QToolBar recomputes its layout margins from style metrics on every style change. The
// timeline card must span the full toolbar to line up with the panels above, so the margins
// are cleared after each recomputation.
class FlushToolBar final : public QToolBar {
  public:
    using QToolBar::QToolBar;

  protected:
    bool event(QEvent *event) override {
        const bool handled = QToolBar::event(event);
        if ((event->type() == QEvent::StyleChange || event->type() == QEvent::Polish) && layout())
            layout()->setContentsMargins(0, 0, 0, 0);
        return handled;
    }
};

class Workbench final : public WorkbenchWindow {
    struct Station {
        application::ProjectGroundStation config;
        QTreeWidgetItem *item = nullptr;
        [[nodiscard]] QString name() const { return QString::fromStdString(config.name()); }
    };
    struct Satellite {
        std::shared_ptr<const OrbitScene> orbit;
        std::string display_name;
        [[nodiscard]] QString name() const {
            return QString::fromStdString(display_name.empty() ? orbit->input.object_id
                                                               : display_name);
        }
        QTreeWidgetItem *item = nullptr;
        vsg::ref_ptr<vsg::Group> model_host = vsg::Group::create();
        vsg::ref_ptr<vsg::MatrixTransform> model;
        int model_kind = 0;
        double model_diameter_km = 1000;
        QString model_path;
        DisplaySettings display;
        std::shared_ptr<render_vsg::TrackLayer> fixed_track, inertial_track;
        vsg::ref_ptr<vsg::Switch> tracks;
    };

  public:
    Workbench(QApplication &app, ValidationLog &log, const QString &smoke_dir, bool validation,
              const QString &benchmark_manifest, const QString &timing_dir, bool timing_probe,
              bool output_only_probe)
        : app_(app), log_(log), smoke_dir_(smoke_dir), validation_(validation),
          benchmarking_(!benchmark_manifest.isEmpty()), frames_(runtime::make_frame_provider()),
          viewer_(vsgQt::Viewer::create()),
          markers_(std::make_unique<render_vsg::MarkerLayer>(10000)),
          selection_(
              std::make_unique<render_vsg::MarkerLayer>(1, render_vsg::MarkerStyle::selection)) {
        try {
            const auto args = app.arguments();
            const auto profile_index = args.indexOf("--workbench-profile");
            if (profile_index >= 0) {
                if (profile_index + 1 >= args.size() || benchmarking_ || !smoke_dir.isEmpty())
                    throw std::invalid_argument(
                        "Use --workbench-profile <new directory> separately");
                profile_ = std::make_unique<WorkbenchProfile>(args[profile_index + 1]);
                const auto count_index = args.indexOf("--profile-satellites");
                if (count_index >= 0)
                    profile_count_ = args.value(count_index + 1).toInt();
                if (profile_count_ < 1 || profile_count_ > 128)
                    throw std::invalid_argument("Profile satellite count must be in [1, 128]");
            }
            playing_ = benchmarking_;
            const bool fixture_mode = benchmarking_ || profile_ || !smoke_dir_.isEmpty();
            std::vector<std::filesystem::path> bundled;
            if (fixture_mode) {
                test_resources_ = qEnvironmentVariable("ORBELLE_TEST_RESOURCES");
                if (test_resources_.isEmpty())
                    test_resources_ = QCoreApplication::applicationDirPath() + "/resources";
                const auto fixtures = std::filesystem::path(test_resources_.toStdWString());
                eop_ = runtime::load_eop_snapshot(fixtures / "eop/manifest.json");
                if (!benchmarking_)
                    bundled = {fixtures / "eop-current/manifest.json",
                               fixtures / "eop/manifest.json"};
            }
            if (!benchmarking_) {
                const auto configured_home = qEnvironmentVariable("ORBELLE_DATA_HOME");
                const auto normal_home =
                    configured_home.isEmpty()
                        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                              "/data"
                        : QDir(configured_home).absolutePath();
                const auto store = smoke_dir_.isEmpty() ? normal_home : smoke_dir_ + "/data-store";
                eop_catalog_ = std::make_unique<EopCatalog>(
                    std::filesystem::path(store.toStdWString()), bundled,
                    smoke_dir_.isEmpty() && configured_home.isEmpty()
                        ? std::filesystem::path((QStandardPaths::writableLocation(
                                                     QStandardPaths::AppLocalDataLocation) +
                                                 "/eop")
                                                    .toStdWString())
                        : std::filesystem::path{},
                    false);
                if (fixture_mode) {
                    eop_current_ = eop_catalog_->providers().at(0);
                    eop_ = eop_catalog_->providers().at(1);
                } else if (!eop_catalog_->providers().empty())
                    eop_ = eop_catalog_->providers().at(eop_catalog_->preferred_index());
            }
#ifdef ORBELLE_DESKTOP_BENCHMARK
            if (benchmarking_)
                benchmark_ = std::make_unique<BenchmarkScene>(benchmark_manifest, *frames_, *eop_);
            if (!timing_dir.isEmpty()) {
                run_ = std::make_unique<BenchmarkRun>(timing_dir, timing_probe, output_only_probe);
                playing_ = false;
            }
#else
            (void)timing_probe;
            (void)output_only_probe;
            (void)timing_dir;
            if (benchmarking_)
                throw std::runtime_error("This build does not enable graphics benchmarks");
#endif
            viewer_->requests.store(0);
            viewer_->continuousUpdate = false;
            hints_ = vsg::ResourceHints::create();
            // Background static uploads must finish before a task/window is replaced.
            hints_->dataTransferHint = vsg::COMPILE_TRAVERSAL_TRANSFER_DATA_AND_WAIT;
            const auto resources =
                std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                "resources";
            globe_ = geo_rocky::make_globe(viewer_, benchmarking_ ? std::filesystem::path{}
                                                                  : resources / "earth/earth.vrt");
            scene_ = vsg::Group::create();
            if (benchmarking_)
                scene_->addChild(globe_->node());
            else {
                earth_transform_ = vsg::MatrixTransform::create();
                earth_transform_->addChild(globe_->node());
                scene_->addChild(earth_transform_);
            }
            scene_->addChild(markers_->node());
            if (!benchmarking_) {
                model_group_ = vsg::Group::create();
                scene_->addChild(model_group_);
                // Separate window navigation from task commands. Both bars use Qt overflow;
                // width changes only hide decorative labels, never rebuild actions or editors.
                setContentsMargins(1, 1, 1, 0);
                auto *title_host = new QWidget;
                auto *title_host_layout = new QVBoxLayout(title_host);
                title_host_layout->setContentsMargins(0, 0, 0, 0);
                auto *title_bar = new QWidget;
                title_bar->setObjectName("titleBar");
                title_bar->setFixedHeight(36);
                title_host_layout->addWidget(title_bar);
                auto *title_layout = new QHBoxLayout(title_bar);
                title_layout->setContentsMargins(10, 0, 6, 0);
                title_layout->setSpacing(6);
                auto *symbol = new QLabel;
                symbol->setObjectName("brandMark");
                symbol->setPixmap(brand_mark(22, devicePixelRatioF()));
                title_layout->addWidget(symbol);
                auto *name = new QLabel(ui::utf8("brand.name"));
                name->setObjectName("brand");
                title_layout->addWidget(name);
                menu_bar_ = new QMenuBar;
                menu_bar_->setObjectName("titleMenus");
                menu_bar_->setNativeMenuBar(false);
                title_layout->addWidget(menu_bar_, 0, Qt::AlignVCenter);
                auto *drag_space = make_sakura_ribbon(title_bar);
                drag_space->setMinimumWidth(48);
                title_layout->addWidget(drag_space, 1);
                auto *chip = new QFrame;
                chip->setObjectName("workspaceChip");
                chip->setFixedHeight(24);
                auto *chip_layout = new QHBoxLayout(chip);
                chip_layout->setContentsMargins(8, 0, 8, 0);
                chip_layout->setSpacing(8);
                workspace_dot_ = new QLabel;
                workspace_dot_->setObjectName("workspaceDot");
                workspace_dot_->setFixedSize(6, 6);
                chip_layout->addWidget(workspace_dot_);
                workspace_label_ = new QLabel(ui::text("graphics_workbench.label"));
                workspace_label_->setObjectName("workspaceName");
                workspace_label_->setMaximumWidth(260);
                chip_layout->addWidget(workspace_label_);
                title_layout->addWidget(chip);
                title_layout->addStretch(1);
                commands_ = new QToolBar(ui::text("graphics_workbench.label_2"));
                commands_->setObjectName("commandBar");
                commands_->setMovable(false);
                commands_->setIconSize(QSize(16, 16));
                commands_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                title_host_layout->addWidget(commands_);
                new WidthObserver(commands_, [this, mode = WidthMode{}](int width) mutable {
                    commands_->setToolButtonStyle(mode.narrow(width, 660)
                                                      ? Qt::ToolButtonIconOnly
                                                      : Qt::ToolButtonTextBesideIcon);
                });
                install_title_bar(title_bar, title_layout);
                setMenuWidget(title_host);
                new WidthObserver(title_bar, [name, chip, wide = WidthMode{},
                                              roomy = WidthMode{}](int width) mutable {
                    name->setVisible(!wide.narrow(width, 760));
                    chip->setVisible(!roomy.narrow(width, 1000));
                });
                scene_menu_ = menu_bar_->addMenu(ui::text("common.scenario"));
                insert_menu_ = menu_bar_->addMenu(ui::text("graphics_workbench.menu_bar.add_menu"));
                view_menu_ = menu_bar_->addMenu(ui::text("graphics_workbench.menu_bar.add_menu_2"));
                time_menu_ = menu_bar_->addMenu(ui::text("common.time"));
                new_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::new_scenario),
                                                   ui::text("common.new_scenario"));
                new_action_->setShortcut(QKeySequence("Ctrl+N"));
                new_action_->setToolTip(ui::text("graphics_workbench.new_action.set_tool_tip"));
                connect(new_action_, &QAction::triggered, this, [this] { create_scenario(); });
                scene_menu_->addAction(new_action_);
                open_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::folder),
                                                    ui::text("common.open"));
                open_action_->setShortcut(QKeySequence::Open);
                open_action_->setObjectName("openProject");
                open_action_->setToolTip(ui::text("graphics_workbench.open_action.set_tool_tip"));
                connect(open_action_, &QAction::triggered, this, [this] { choose_project(); });
                scene_menu_->addAction(open_action_);
                save_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::save),
                                                    ui::text("common.save"));
                save_action_->setShortcut(QKeySequence::Save);
                save_action_->setObjectName("saveProject");
                save_action_->setToolTip(ui::text("graphics_workbench.save_action.set_tool_tip"));
                connect(save_action_, &QAction::triggered, this,
                        [this] { (void)save_project_dialog(false); });
                scene_menu_->addAction(save_action_);
                save_as_action_ =
                    scene_menu_->addAction(ui::text("graphics_workbench.scene_menu.add_action"));
                save_as_action_->setShortcut(QKeySequence::SaveAs);
                connect(save_as_action_, &QAction::triggered, this,
                        [this] { (void)save_project_dialog(true); });
                example_action_ =
                    scene_menu_->addAction(ui::text("graphics_workbench.scene_menu.add_action_2"));
                connect(example_action_, &QAction::triggered, this, [this] { open_example(); });
                auto *data_menu =
                    menu_bar_->addMenu(ui::text("graphics_workbench.menu_bar.add_menu_3"));
                eop_action_ =
                    data_menu->addAction(workbench_icon(WorkbenchIcon::database),
                                         ui::text("graphics_workbench.data_menu.add_action"));
                eop_action_->setObjectName("manageEopData");
                connect(eop_action_, &QAction::triggered, this, [this] { manage_eop_data(); });
                auto *analysis_menu =
                    menu_bar_->addMenu(ui::text("graphics_workbench.menu_bar.add_menu_4"));
                report_action_ = analysis_menu->addAction(
                    workbench_icon(WorkbenchIcon::properties),
                    ui::text("graphics_workbench.analysis_menu.add_action"));
                report_action_->setObjectName("openReports");
                connect(report_action_, &QAction::triggered, this, [this] { create_report(); });
                if (!profile_ && smoke_dir_.isEmpty()) {
                    application_update_ = new ApplicationUpdateUi(
                        QString::fromStdWString(
                            (eop_catalog_->store() / "desktop-preferences.ini").wstring()),
                        menu_bar_, this);
                    application_update_->restart_requested = [this] {
                        update_restart_ = true;
                        close();
                    };
                }
                scene_menu_->addSeparator();
                // Analysis and data were only reachable from menus; keep them one click away.
                commands_->addSeparator();
                report_action_->setToolTip(
                    ui::text("graphics_workbench.report_action.set_tool_tip"));
                commands_->addAction(report_action_);
                eop_action_->setToolTip(ui::text("graphics_workbench.eop_action.set_tool_tip"));
                commands_->addAction(eop_action_);
                // Inserting lives with the objects it creates: the explorer's add button,
                // the Insert menu and Ctrl+Shift+I.
                insert_action_ = new QAction(workbench_icon(WorkbenchIcon::add),
                                             ui::text("graphics_workbench.label_3"), this);
                insert_action_->setShortcut(QKeySequence("Ctrl+Shift+I"));
                insert_action_->setToolTip(
                    ui::text("graphics_workbench.insert_action.set_tool_tip"));
                connect(insert_action_, &QAction::triggered, this,
                        [this] { show_insert_objects(); });
                insert_menu_->addAction(insert_action_);
                insert_menu_->addSeparator();
                for (const auto type :
                     {InsertObjectType::satellite, InsertObjectType::ground_station,
                      InsertObjectType::sensor}) {
                    const bool satellite = type == InsertObjectType::satellite;
                    auto *action = insert_menu_->addAction(
                        workbench_icon(satellite ? WorkbenchIcon::satellite
                                       : type == InsertObjectType::sensor
                                           ? WorkbenchIcon::focus
                                           : WorkbenchIcon::ground_station),
                        ui::text(satellite ? "insert_object_dialog.quick_satellite"
                                 : type == InsertObjectType::sensor
                                     ? "insert_object_dialog.quick_sensor"
                                     : "insert_object_dialog.quick_station"));
                    action->setObjectName(satellite ? "insertSatellitePreferred"
                                          : type == InsertObjectType::sensor
                                              ? "insertSensorPreferred"
                                              : "insertStationPreferred");
                    action->setToolTip(ui::text("insert_object_dialog.quick_hint"));
                    quick_insert_actions_[static_cast<std::size_t>(type)] = action;
                    connect(action, &QAction::triggered, this, [this, type] {
                        if (scenario_ && !busy()) {
                            const auto parent = selected_
                                                    ? satellites_[*selected_].orbit->input.object_id
                                                    : std::string{};
                            if (type == InsertObjectType::sensor && parent.empty()) {
                                show_insert_objects();
                                insertion_dialog()->select_type(type);
                            } else
                                insert_object(
                                    {type, insertion_dialog()->default_method(type), parent});
                        }
                    });
                }
                commands_->insertAction(report_action_, insert_action_);
                if (auto *button =
                        qobject_cast<QToolButton *>(commands_->widgetForAction(insert_action_))) {
                    button->setObjectName("insertObjectButton");
                    button->setMenu(insert_menu_);
                    button->setPopupMode(QToolButton::MenuButtonPopup);
                }
            }
            if (benchmarking_) {
#ifdef ORBELLE_DESKTOP_BENCHMARK
                for (std::size_t i = 0; i < 100; ++i)
                    scene_->addChild(render_vsg::make_track(benchmark_->cache->track(i * 100)));
#endif
            } else {
                orbit_group_ = vsg::Group::create();
                orbit_switch_ = vsg::Switch::create();
                orbit_switch_->addChild(true, orbit_group_);
                scene_->addChild(orbit_switch_);
            }
            snapshot_ = sample_frame(core::Epoch{0});
            markers_->update(snapshot_);
            initial_position_ = snapshot_.markers.empty() ? std::array<double, 3>{7e6, 0, 0}
                                                          : snapshot_.markers.front().position_m;
            scene_->addChild(selection_->node());
            setWindowTitle(benchmarking_ ? ui::text("graphics_workbench.label_4")
                                         : ui::text("graphics_workbench.label_5"));
            resize_to_screen(1600, 960);
            auto *objects = new QTreeWidget;
            objects_ = objects;
            objects->setObjectName("sceneObjects");
            objects->setHeaderLabel(ui::text("common.scenario"));
            if (!benchmarking_)
                objects->setHeaderHidden(true);
            if (benchmarking_) {
                auto *scenario =
                    new QTreeWidgetItem(objects, {ui::text("graphics_workbench.scenario")});
                object_item_ =
                    new QTreeWidgetItem(scenario, {ui::text("graphics_workbench.label_6")});
            }
            objects->expandAll();
            if (!benchmarking_) {
                objects->setIconSize(QSize(18, 18));
                objects->setIndentation(16);
                objects->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                connect(objects, &QTreeWidget::currentItemChanged, this,
                        [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
                            set_selection(object_index(item), false);
                        });
            }
            auto *object_dock = new QDockWidget(ui::text("graphics_workbench.object_dock"), this);
            object_dock_ = object_dock;
            if (benchmarking_) {
                object_dock->setWidget(objects);
            } else {
                auto *explorer = new QWidget;
                explorer->setObjectName("objectExplorer");
                auto *layout = new QVBoxLayout(explorer);
                layout->setContentsMargins(8, 8, 8, 8);
                layout->setSpacing(6);
                auto *search = new QLineEdit;
                search->setObjectName("objectSearch");
                search->setPlaceholderText(
                    ui::text("graphics_workbench.search.set_placeholder_text"));
                search->addAction(workbench_icon(WorkbenchIcon::search),
                                  QLineEdit::LeadingPosition);
                search->setClearButtonEnabled(true);
                connect(search, &QLineEdit::textChanged, objects, [this] { filter_objects(); });
                auto *search_row = new QHBoxLayout;
                search_row->setSpacing(6);
                search_row->addWidget(search, 1);
                auto *add = new QToolButton;
                add->setObjectName("explorerInsert");
                add->setDefaultAction(insert_action_);
                add->setToolButtonStyle(Qt::ToolButtonIconOnly);
                add->setFixedSize(26, 26);
                add->setIconSize(QSize(16, 16));
                add->setAccessibleName(ui::text("graphics_workbench.add.set_accessible_name"));
                search_row->addWidget(add);
                layout->addLayout(search_row);
                layout->addWidget(objects, 1);
                object_dock->setWidget(explorer);
                style_dock(object_dock, WorkbenchIcon::orbit);
            }
            addDockWidget(Qt::LeftDockWidgetArea, object_dock);
            object_dock->setMaximumWidth(benchmarking_ ? 220 : 290);
            if (!benchmarking_)
                object_dock->setMinimumWidth(200);
            if (!benchmarking_ && runtime::two_body_available()) {
                editor_ = new OrbitEditor(OrbitInput{});
                editor_->data_catalog = eop_catalog_.get();
                editor_->gravity_models = [this] { return eop_catalog_->data().gravity_models(); };
                editor_->ephemerides = [this] { return eop_catalog_->data().ephemerides(); };
                editor_->manage_ephemerides = [this] {
                    show_data_center(editor_, *eop_catalog_, DataProduct::de440s);
                    return eop_catalog_->data().ephemerides();
                };
                editor_->transform_state = [this](core::CartesianState state, core::FrameId frame) {
                    if (state.frame == frame)
                        return state;
                    auto installed_data = eop_catalog_->providers();
                    installed_data.insert(installed_data.begin(), eop_);
                    for (const auto &source : installed_data) {
                        if (!source)
                            continue;
                        const auto &coverage = source->data_info();
                        if (!core::eop_covers(coverage, state.epoch))
                            continue;
                        const auto transform =
                            frames_->gcrf_to_itrf_state(state.epoch, source->at(state.epoch));
                        return frame == core::FrameId::itrf ? transform.apply(state)
                                                            : transform.inverse(state);
                    }
                    throw std::out_of_range(ui::utf8("graphics_workbench.validation"));
                };
                properties_dialog_ = new QDialog(this, Qt::Window);
                properties_dialog_->setAttribute(Qt::WA_QuitOnClose, false);
                properties_dialog_->setWindowTitle(
                    ui::text("graphics_workbench.properties_dialog.set_window_title"));
                properties_dialog_->resize(
                    980, std::min(660, screen()->availableGeometry().height() - 100));
                // The editor scrolls its content and pins actions to the available height.
                // A top-level dialog minimum would otherwise clip the embedded footer.
                properties_dialog_->setMinimumWidth(320);
                auto *dialog_layout = new QVBoxLayout(properties_dialog_);
                dialog_layout->setSizeConstraint(QLayout::SetNoConstraint);
                dialog_layout->setContentsMargins(0, 0, 0, 0);
                dialog_layout->setSpacing(0);
                inspector_pages_ = new QStackedWidget;
                inspector_pages_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
                inspector_pages_->addWidget(editor_);
                auto *empty = new QLabel(ui::text("graphics_workbench.empty"));
                empty->setObjectName("muted");
                empty->setAlignment(Qt::AlignCenter);
                empty->setWordWrap(true);
                inspector_pages_->addWidget(empty);
                dialog_layout->addWidget(inspector_pages_, 1);
                resizeDocks({object_dock}, {224}, Qt::Horizontal);
                editor_->apply = [this](OrbitInput input) {
                    start_orbit(std::move(input), editor_->object_name());
                };
                editor_->close_requested = [this] { properties_dialog_->close(); };
                editor_->display = [this](DisplaySettings settings) {
                    if (!editor_target_ || busy())
                        return;
                    auto &satellite = satellites_.at(*editor_target_);
                    (void)display_interval(*satellite.orbit, settings, snapshot_.epoch);
                    if (satellite.display != settings) {
                        satellite.display = settings;
                        satellite.item->setIcon(
                            0, workbench_icon(WorkbenchIcon::satellite,
                                              QColor::fromRgb(settings.line_color)));
                        mark_project_dirty();
                        update_satellite();
                    }
                    editor_->accept_display(settings);
                    statusBar()->showMessage(ui::text("graphics_workbench.label_7"), 5000);
                };
                editor_->cancel = [this] {
                    if (project_candidate_.valid() && project_orbit_edit_ && project_cancellation_)
                        project_cancellation_->store(true);
                    if (cancellation_)
                        cancellation_->store(true);
                };
                editor_->appearance = [this](int kind, const QString &path, double diameter) {
                    start_model(kind, path, diameter);
                };
                auto *properties = new QAction(workbench_icon(WorkbenchIcon::properties),
                                               ui::text("common.properties"), this);
                addAction(properties);
                properties_action_ = properties;
                properties->setShortcut(QKeySequence("Ctrl+I"));
                connect(properties, &QAction::triggered, this, [this] { open_properties(); });
                connect(objects, &QTreeWidget::itemDoubleClicked, this,
                        [this](QTreeWidgetItem *item, int) {
                            const auto index = object_index(item);
                            if (!index) {
                                if (item == scenario_item_)
                                    open_scenario_properties();
                                return;
                            }
                            set_selection(index, false);
                            open_properties();
                        });
                scene_menu_->addAction(properties);
                auto *object_menu = menu_bar_->insertMenu(
                    view_menu_->menuAction(), new QMenu(ui::text("common.objects"), this));
                auto *object_commands = object_menu->menu();
                object_commands->addAction(properties);
                rename_action_ = object_commands->addAction(
                    ui::text("graphics_workbench.object_commands.add_action"));
                rename_action_->setObjectName("renameObject");
                rename_action_->setShortcut(QKeySequence("F2"));
                connect(rename_action_, &QAction::triggered, this, [this] { rename_selected(); });
                duplicate_action_ = object_commands->addAction(
                    ui::text("graphics_workbench.object_commands.add_action_2"));
                duplicate_action_->setObjectName("duplicateObject");
                duplicate_action_->setShortcut(QKeySequence("Ctrl+D"));
                connect(duplicate_action_, &QAction::triggered, this,
                        [this] { duplicate_selected(); });
                delete_action_ = object_commands->addAction(
                    ui::text("graphics_workbench.object_commands.add_action_3"));
                delete_action_->setObjectName("deleteObject");
                delete_action_->setShortcut(QKeySequence::Delete);
                connect(delete_action_, &QAction::triggered, this, [this] { delete_selected(); });
                scene_menu_->addSeparator();
                auto *quit =
                    scene_menu_->addAction(ui::text("graphics_workbench.scene_menu.add_action_3"));
                connect(quit, &QAction::triggered, this, &QWidget::close);
                view_menu_->addAction(object_dock->toggleViewAction());
                show_orbit_status();
            }
            auto *toolbar = benchmarking_ ? new QToolBar(ui::text("common.time"), this)
                                          : new FlushToolBar(ui::text("common.time"), this);
            time_toolbar_ = toolbar;
            if (!benchmarking_)
                toolbar->setObjectName("timelineBar");
            addToolBar(benchmarking_ ? Qt::TopToolBarArea : Qt::BottomToolBarArea, toolbar);
            toolbar->setMovable(false);
            toolbar->setIconSize(QSize(18, 18));
            toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
#ifdef ORBELLE_DESKTOP_BENCHMARK
            if (run_)
                toolbar->setEnabled(false);
#endif
            auto *play = new QAction(play_icon(playing_),
                                     playing_ ? ui::text("graphics_workbench.play")
                                              : ui::text("graphics_workbench.play_2"),
                                     this);
            play_action_ = play;
            connect(play, &QAction::triggered, this, [this] { set_playing(!playing_); });
            if (benchmarking_) {
                toolbar->addAction(play);
                slider_ = new QSlider(Qt::Horizontal);
                slider_->setRange(0, 600);
                slider_->setMinimumWidth(300);
                toolbar->addWidget(slider_);
            } else {
                auto *timeline = new QWidget;
                timeline->setObjectName("timelineCard");
                timeline->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                // Keep the toolbar's cached single-row hint from blocking the narrow layout.
                timeline->setMinimumWidth(560);
                auto *layout = new QVBoxLayout(timeline);
                layout->setContentsMargins(1, 0, 1, 4);
                layout->setSpacing(0);
                auto *control_panel = new QWidget;
                control_panel->setObjectName("timelineControls");
                auto *control_rows = new QHBoxLayout(control_panel);
                control_rows->setContentsMargins(8, 3, 8, 3);
                control_rows->setSpacing(4);
                auto *transport = new QHBoxLayout;
                auto *controls = transport;
                controls->setContentsMargins(0, 0, 0, 0);
                controls->setSpacing(4);
                auto *heading = new QLabel(ui::text("graphics_workbench.heading"));
                heading->setObjectName("timelineTitle");
                heading->setMinimumWidth(60);
                controls->addWidget(heading);
                auto *start = new QAction(workbench_icon(WorkbenchIcon::first),
                                          ui::text("graphics_workbench.start"), this);
                auto *end = new QAction(workbench_icon(WorkbenchIcon::last),
                                        ui::text("graphics_workbench.end"), this);
                auto *slower = new QAction(workbench_icon(WorkbenchIcon::slower),
                                           ui::text("graphics_workbench.slower"), this);
                auto *faster = new QAction(workbench_icon(WorkbenchIcon::faster),
                                           ui::text("graphics_workbench.faster"), this);
                slower->setObjectName("slowerPlayback");
                faster->setObjectName("fasterPlayback");
                slower->setShortcut(QKeySequence("Ctrl+["));
                faster->setShortcut(QKeySequence("Ctrl+]"));
                start->setShortcut(QKeySequence("Ctrl+Home"));
                end->setShortcut(QKeySequence("Ctrl+End"));
                play->setShortcut(QKeySequence("Ctrl+Space"));
                connect(start, &QAction::triggered, this, [this] {
                    set_playing(false);
                    if (scenario_)
                        seek_epoch(scenario_->first());
                });
                connect(end, &QAction::triggered, this, [this] {
                    set_playing(false);
                    if (scenario_)
                        seek_epoch(scenario_->last());
                });
                auto *transport_group = new QFrame;
                transport_group->setObjectName("transportGroup");
                auto *transport_buttons = new QHBoxLayout(transport_group);
                transport_buttons->setContentsMargins(2, 2, 2, 2);
                transport_buttons->setSpacing(1);
                for (auto *action : {start, slower, play, faster, end}) {
                    auto *button = new QToolButton;
                    button->setDefaultAction(action);
                    button->setFixedSize(28, 28);
                    button->setIconSize(QSize(action == play ? 16 : 18, action == play ? 16 : 18));
                    if (action == play)
                        button->setObjectName("transportPlay");
                    button->setAccessibleName(action->text());
                    transport_buttons->addWidget(button);
                    time_menu_->addAction(action);
                }
                controls->addWidget(transport_group);
                time_menu_->addSeparator();
                auto *jump =
                    time_menu_->addAction(ui::text("graphics_workbench.time_menu.add_action"));
                jump->setShortcut(QKeySequence("Ctrl+T"));
                jump->setToolTip(ui::text("graphics_workbench.jump.set_tool_tip"));
                connect(jump, &QAction::triggered, this, [this] { choose_time(); });
                auto *speed = new QDoubleSpinBox;
                speed->setObjectName("playbackRate");
                speed->setButtonSymbols(QAbstractSpinBox::NoButtons);
                speed->setKeyboardTracking(false);
                speed->setDecimals(1);
                speed->setRange(playback_rates.front(), playback_rates.back());
                speed->setValue(playback_rate_);
                speed->setSuffix(QStringLiteral(" ×"));
                speed->setMinimumWidth(0);
                speed->setToolTip(ui::text("graphics_workbench.speed.set_tool_tip"));
                speed->setAccessibleName(ui::text("graphics_workbench.speed.set_accessible_name"));
                auto *rate_field = new QFrame;
                rate_field->setObjectName("playbackRateField");
                rate_field->setFixedWidth(110);
                auto *rate_layout = new QHBoxLayout(rate_field);
                rate_layout->setContentsMargins(1, 1, 1, 1);
                rate_layout->setSpacing(0);
                rate_layout->addWidget(speed, 1);
                auto *presets =
                    new QMenu(ui::text("graphics_workbench.speed.set_accessible_name"), this);
                presets->setObjectName("playbackRateMenu");
                for (const double rate : playback_rates) {
                    auto *action = presets->addAction(QString::number(rate, 'g', 5) + " ×");
                    action->setCheckable(true);
                    action->setData(rate);
                }
                const auto update_rate = [this, speed, slower, faster, presets] {
                    const double rate = speed->value();
                    playback_rate_ = rate;
                    slower->setEnabled(rate > playback_rates.front());
                    faster->setEnabled(rate < playback_rates.back());
                    slower->setToolTip(ui::text("graphics_workbench.slower.set_tool_tip")
                                           .arg(adjacent_playback_rate(rate, false), 0, 'g', 5));
                    faster->setToolTip(ui::text("graphics_workbench.faster.set_tool_tip")
                                           .arg(adjacent_playback_rate(rate, true), 0, 'g', 5));
                    for (auto *action : presets->actions())
                        action->setChecked(action->data().toDouble() == rate);
                };
                for (auto *action : presets->actions())
                    connect(action, &QAction::triggered, this, [speed, action, update_rate] {
                        speed->setValue(action->data().toDouble());
                        update_rate();
                    });
                connect(speed, &QDoubleSpinBox::valueChanged, this, update_rate);
                for (const auto &[action, increase] :
                     {std::pair{slower, false}, std::pair{faster, true}})
                    connect(action, &QAction::triggered, this, [speed, increase] {
                        speed->interpretText();
                        speed->setValue(adjacent_playback_rate(speed->value(), increase));
                    });
                connect(presets, &QMenu::aboutToShow, this, [presets, update_rate] {
                    update_rate();
                    for (auto *action : presets->actions())
                        if (action->isChecked())
                            presets->setActiveAction(action);
                });
                auto *preset_button = new QToolButton;
                preset_button->setObjectName("playbackRatePresets");
                preset_button->setIcon(workbench_icon(WorkbenchIcon::chevron_down));
                preset_button->setIconSize(QSize(14, 14));
                preset_button->setFixedSize(24, 26);
                preset_button->setPopupMode(QToolButton::InstantPopup);
                preset_button->setToolTip(
                    ui::text("graphics_workbench.preset_button.set_tool_tip"));
                preset_button->setAccessibleName(preset_button->toolTip());
                preset_button->setMenu(presets);
                rate_layout->addWidget(preset_button);
                controls->addWidget(rate_field);
                time_menu_->addMenu(presets);
                update_rate();
                timeline_ = new MissionTimeline;
                slider_ = timeline_;
                time_menu_->addSeparator();
                auto *object_interval =
                    time_menu_->addAction(ui::text("graphics_workbench.time_menu.add_action_2"));
                object_interval->setObjectName("showTimelineObject");
                object_interval->setCheckable(true);
                connect(object_interval, &QAction::toggled, this,
                        [this](bool show) { timeline_->set_show_object_interval(show); });
                connect(slider_, &QSlider::sliderPressed, this, [this] { set_playing(false); });
                timeline_->seek_requested = [this](qint64 tai_ms) {
                    if (!scenario_)
                        return;
                    set_playing(false);
                    const auto epoch = tai_ms <= scenario_->first().tai_nanoseconds() / 1'000'000
                                           ? scenario_->first()
                                       : tai_ms >= scenario_->last().tai_nanoseconds() / 1'000'000
                                           ? scenario_->last()
                                           : core::Epoch{tai_ms * 1'000'000};
                    seek_epoch(epoch);
                };
                auto *divider = new QFrame;
                divider->setObjectName("timelineDivider");
                divider->setFrameShape(QFrame::VLine);
                divider->setFixedHeight(18);
                controls->addWidget(divider);
                for (const auto &[icon, text, name, command] :
                     {std::tuple{WorkbenchIcon::zoom_out, ui::text("graphics_workbench.label_8"),
                                 "timelineZoomOut",
                                 std::function<void()>{[this] { timeline_->zoom(0.5); }}},
                      std::tuple{WorkbenchIcon::zoom_in, ui::text("graphics_workbench.label_9"),
                                 "timelineZoomIn",
                                 std::function<void()>{[this] { timeline_->zoom(2); }}},
                      std::tuple{WorkbenchIcon::focus, ui::text("graphics_workbench.label_10"),
                                 "timelineCenter",
                                 std::function<void()>{[this] { timeline_->center_current(); }}},
                      std::tuple{WorkbenchIcon::time_fit, ui::text("graphics_workbench.label_11"),
                                 "timelineFit",
                                 std::function<void()>{[this] { timeline_->reset_view(); }}}}) {
                    auto *button = new QToolButton;
                    button->setIcon(workbench_icon(icon));
                    button->setObjectName(name);
                    button->setToolTip(text);
                    button->setAccessibleName(text);
                    button->setFixedSize(28, 28);
                    button->setIconSize(QSize(18, 18));
                    connect(button, &QToolButton::clicked, this, command);
                    controls->addWidget(button);
                }
                controls->addStretch();
                control_rows->addLayout(transport, 1);
                controls = new QHBoxLayout;
                controls->setContentsMargins(0, 0, 0, 0);
                controls->setSpacing(4);
                controls->addStretch();
                time_label_ = new QToolButton;
                time_label_->setObjectName("timeReadout");
                time_label_->setDefaultAction(jump);
                time_label_->setToolButtonStyle(Qt::ToolButtonTextOnly);
                controls->addWidget(time_label_);
                auto *time_scale = new QComboBox;
                time_scale->setObjectName("timelineScale");
                time_scale->addItems({"UTC", "TAI", "TT"});
                time_scale->setAccessibleName(
                    ui::text("graphics_workbench.time_scale.set_accessible_name"));
                controls->addWidget(time_scale);
                connect(time_scale, &QComboBox::currentIndexChanged, this, [this](int index) {
                    display_scale_ = static_cast<CalendarScale>(index);
                    if (timeline_)
                        timeline_->set_scale(display_scale_);
                });
                auto *expand =
                    time_menu_->addAction(ui::text("graphics_workbench.time_menu.add_action_3"));
                expand->setObjectName("expandTimeline");
                expand->setCheckable(true);
                expand->setChecked(true);
                expand->setShortcut(QKeySequence("Ctrl+J"));
                auto *collapse = new QToolButton;
                collapse->setObjectName("collapseTimeline");
                collapse->setFixedSize(28, 28);
                collapse->setIconSize(QSize(16, 16));
                collapse->setIcon(workbench_icon(WorkbenchIcon::chevron_down));
                collapse->setToolTip(ui::text("graphics_workbench.collapse.set_tool_tip"));
                collapse->setAccessibleName(collapse->toolTip());
                connect(collapse, &QToolButton::clicked, expand, &QAction::toggle);
                connect(expand, &QAction::toggled, this, [this, collapse, timeline](bool expanded) {
                    timeline_->setVisible(expanded);
                    // QToolBar caches the widget action's size hint; refresh both layouts.
                    timeline->layout()->activate();
                    time_toolbar_->layout()->invalidate();
                    time_toolbar_->updateGeometry();
                    this->layout()->invalidate();
                    this->layout()->activate();
                    collapse->setIcon(workbench_icon(expanded ? WorkbenchIcon::chevron_down
                                                              : WorkbenchIcon::chevron_up));
                    collapse->setToolTip(
                        expanded ? ui::text("graphics_workbench.collapse.set_tool_tip")
                                 : ui::text("graphics_workbench.collapse.set_tool_tip_2"));
                    collapse->setAccessibleName(collapse->toolTip());
                });
                controls->addWidget(collapse);
                control_rows->addLayout(controls);
                layout->addWidget(control_panel);
                layout->addWidget(timeline_);
                // The gap above the card lives in a host widget: toolbar padding would also
                // inset the card horizontally and misalign it with the panels above.
                auto *timeline_host = new QWidget;
                timeline_host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                auto *host_layout = new QVBoxLayout(timeline_host);
                host_layout->setContentsMargins(0, 3, 0, 0);
                host_layout->addWidget(timeline);
                toolbar->addWidget(timeline_host);
                responsive_columns(timeline, control_rows, 1000);
                auto *credit = new QLabel(ui::utf8("brand.map_credit"));
                credit->setObjectName("hint");
                statusBar()->addPermanentWidget(credit);
                show_orbit_status();
            }
            connect(slider_, &QSlider::valueChanged, this, [this](int value) {
                seconds_ = benchmarking_ ? value : duration_seconds() * value / 10000.0;
                precise_epoch_.reset();
                requested_time_ = true;
                clock_.restart();
            });
            build_slot(false);
            build_slot(true);
            if (!benchmarking_) {
                ground_map_ = new GroundMap(QString::fromStdWString(
                    (resources / "earth/land_ocean_ice_2048.jpg").wstring()));
                ground_map_->select = [this](std::optional<std::size_t> index) {
                    set_selection(index);
                };
                map_panel_ = new QWidget;
                auto *map_layout = new QVBoxLayout(map_panel_);
                map_layout->setContentsMargins(0, 0, 0, 0);
                map_layout->setSpacing(0);
                map_layout->addWidget(make_view_controls(std::nullopt));
                map_layout->addWidget(ground_map_, 1);
                workspace_->add_view(map_panel_, ui::text("workspace.view_2d"),
                                     workbench_icon(WorkbenchIcon::map));
                map_action_ = new QAction(ui::text("workspace.view_2d"), this);
                map_action_->setIcon(workbench_icon(WorkbenchIcon::map));
                map_action_->setShortcut(QKeySequence("Ctrl+3"));
                view_menu_->addAction(map_action_);
                connect(map_action_, &QAction::triggered, this,
                        [this] { workspace_->select_view(1); });
                auto *three_d = view_menu_->addAction(workbench_icon(WorkbenchIcon::earth),
                                                      ui::text("workspace.view_3d"));
                three_d->setShortcut(QKeySequence("Ctrl+1"));
                connect(three_d, &QAction::triggered, this, [this] { workspace_->select_view(0); });
                fit_views();
                auto *fit = new QAction(workbench_icon(WorkbenchIcon::fit),
                                        ui::text("graphics_workbench.fit"), this);
                fit_action_ = fit;
                fit->setShortcut(QKeySequence("Ctrl+0"));
                connect(fit, &QAction::triggered, this, [this] { fit_views(); });
                auto *focus = new QAction(workbench_icon(WorkbenchIcon::focus),
                                          ui::text("graphics_workbench.focus"), this);
                focus_action_ = focus;
                focus->setShortcut(QKeySequence("Ctrl+L"));
                connect(focus, &QAction::triggered, this, [this] {
                    focus_satellite();
                    ground_map_->focus_selection();
                });
                view_menu_->addSeparator();
                view_menu_->addAction(fit);
                view_menu_->addAction(focus);
                objects->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(objects, &QWidget::customContextMenuRequested, this,
                        [this, objects, focus](const QPoint &point) {
                            auto *item = objects->itemAt(point);
                            const auto index = object_index(item);
                            QMenu menu(this);
                            if (index) {
                                objects->setCurrentItem(item);
                                set_selection(index, false);
                                if (properties_action_)
                                    menu.addAction(properties_action_);
                                menu.addAction(focus);
                                auto *report = menu.addAction(
                                    ui::text("graphics_workbench.analysis_menu.add_action"));
                                report->setObjectName("openObjectReport");
                                report->setEnabled(!busy());
                                const auto report_id =
                                    selected_sensor() ? selected_sensor()->object_id
                                    : selected_station_
                                        ? stations_.at(*selected_station_).config.object_id
                                        : satellites_.at(*selected_).orbit->input.object_id;
                                connect(report, &QAction::triggered, this,
                                        [this, report_id] { create_report(report_id); });
                                auto *access =
                                    menu.addAction(ui::text("graphics_workbench.access"));
                                access->setObjectName("openObjectAccess");
                                access->setEnabled(!busy() && !stations_.empty() &&
                                                   !satellites_.empty());
                                connect(access, &QAction::triggered, this, [this, report_id] {
                                    create_report(report_id,
                                                  application::ReportKind::access_intervals);
                                });
                                if (selected_ && !selected_sensor()) {
                                    auto *sensor =
                                        menu.addAction(ui::text("graphics_workbench.sensor"));
                                    sensor->setObjectName("insertSensor");
                                    sensor->setEnabled(!busy());
                                    connect(sensor, &QAction::triggered, this,
                                            [this] { edit_sensor(true); });
                                }
                                menu.addSeparator();
                                menu.addAction(rename_action_);
                                menu.addAction(duplicate_action_);
                                menu.addAction(delete_action_);
                            } else {
                                if (item && item == scenario_item_) {
                                    auto *properties =
                                        menu.addAction(workbench_icon(WorkbenchIcon::properties),
                                                       ui::text("common.properties"));
                                    properties->setEnabled(!busy());
                                    connect(properties, &QAction::triggered, this,
                                            [this] { open_scenario_properties(); });
                                    menu.addSeparator();
                                }
                                menu.addAction(new_action_);
                                menu.addAction(insert_action_);
                            }
                            menu.exec(objects->viewport()->mapToGlobal(point));
                        });
                slots_[1]->dock->hide();
                update_actions();
                set_selection(std::nullopt);
            }
            rebuild();
            connect(&timer_, &QTimer::timeout, this, [this] {
                try {
                    frame();
                } catch (const std::exception &error) {
                    fail(error.what());
                } catch (const vsg::Exception &error) {
                    fail(error.message);
                }
            });
            clock_.start();
            timer_.setTimerType(Qt::PreciseTimer);
            timer_.setInterval(16);
            if (!smoke_dir_.isEmpty()) {
                QDir().mkpath(smoke_dir_);
                // The complete interaction audit includes modal editors, reports,
                // persistence and two scientific propagation workflows.
                QTimer::singleShot(90000, this, [this] {
                    if (!finished_) {
                        const auto *modal = QApplication::activeModalWidget();
                        fail(QStringLiteral(
                                 "Rendering smoke test timed out in %1, stage %2; modal: %3")
                                 .arg(orbit_smoke_scenario())
                                 .arg(orbit_smoke_stage_)
                                 .arg(modal ? modal->objectName() : QStringLiteral("none"))
                                 .toStdString());
                    }
                });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~Workbench() override { shutdown(); }
    void start_rendering() {
        clock_.restart();
        timer_.start();
        if (application_update_)
            application_update_->schedule_startup_check();
        if (!benchmarking_ && !profile_ && smoke_dir_.isEmpty())
            QTimer::singleShot(3000, this, [] { acknowledge_update_startup(); });
    }
    bool initialize_data(StartupSplash *splash) {
        if (benchmarking_ || profile_)
            return true;
        if (!smoke_dir_.isEmpty()) {
            const auto fixture = qEnvironmentVariable("ORBELLE_STARTUP_GRAVITY_FILE");
            if (!fixture.isEmpty()) {
                // Explicit local smoke input: never download or touch the user's catalog.
                eop_catalog_->data().install_gravity(std::filesystem::path(fixture.toStdWString()),
                                                     "egm2008");
                QElapsedTimer elapsed;
                elapsed.start();
                if (!splash->prepare(startup_resource_steps(
                        startup_resources_, eop_catalog_->data().gravity_models(), {})))
                    return false;
                smoke_startup_gravity_ = startup_resources_.gravity.at("egm2008");
                smoke_gravity_id_ = "egm2008";
                interaction_report_.append(QJsonObject{{"kind", "startup-preloads-default-gravity"},
                                                       {"elapsed_ms", elapsed.elapsed()},
                                                       {"passed", true}});
                if (!splash->grab().save(smoke_dir_ + "/startup-ready.png"))
                    throw std::runtime_error("Cannot capture prepared startup");
            }
            return true;
        }
        for (;;) {
            const auto required = initial_data_products();
            if (!std::all_of(required.begin(), required.end(), [&](auto product) {
                    return initial_data_ready(*eop_catalog_, product);
                }))
                splash->hide();
            if (!prepare_initial_data(this, *eop_catalog_, required, {},
                                      [] { acknowledge_update_startup(); }))
                return false;
            try {
                const auto steps = startup_resource_steps(startup_resources_,
                                                          eop_catalog_->data().gravity_models(),
                                                          eop_catalog_->data().ephemerides());
                if (!splash->prepare(steps))
                    return false;
                break;
            } catch (const std::exception &error) {
                splash->hide();
                QMessageBox message(QMessageBox::Warning, ui::text("startup_splash.failed_title"),
                                    ui::text("startup_splash.failed"),
                                    QMessageBox::Retry | QMessageBox::Cancel, nullptr);
                message.setScreen(screen());
                message.setObjectName("startupPreparationError");
                message.setDetailedText(QString::fromUtf8(error.what()));
                message.button(QMessageBox::Retry)->setText(ui::text("startup_splash.retry"));
                message.button(QMessageBox::Cancel)->setText(ui::text("startup_splash.cancel"));
                auto *manage =
                    message.addButton(ui::text("startup_splash.manage"), QMessageBox::ActionRole);
                message.exec();
                if (message.clickedButton() == manage)
                    show_data_center(nullptr, *eop_catalog_);
                else if (message.standardButton(message.clickedButton()) != QMessageBox::Retry)
                    return false;
            }
        }
        if (eop_catalog_->providers().empty())
            throw std::runtime_error(
                "Initial data preparation did not install Earth orientation data");
        eop_ = eop_catalog_->providers().at(eop_catalog_->preferred_index());
        if (editor_)
            editor_->set_time_provider(eop_catalog_->time_provider());
        eop_catalog_->set_update_schedule_enabled(true);
        return true;
    }
    void closeEvent(QCloseEvent *event) override {
        if (update_handover_pending_) {
            event->ignore();
            return;
        }
        if (!update_handed_over_ && !benchmarking_ && smoke_dir_.isEmpty() && !profile_) {
            if (busy()) {
                if (project_candidate_.valid() && project_cancellation_)
                    project_cancellation_->store(true);
                event->ignore();
                update_restart_ = false;
                statusBar()->showMessage(ui::text("graphics_workbench.label_12"), 5000);
                return;
            }
            if (!confirm_replace_scenario()) {
                event->ignore();
                update_restart_ = false;
                return;
            }
        }
        if (update_restart_ && !update_handed_over_ && application_update_) {
            event->ignore();
            update_handover_pending_ = true;
            application_update_->launch_updater([this](bool ready, QString error) {
                update_handover_pending_ = false;
                update_handed_over_ = ready;
                update_restart_ = ready;
                if (ready)
                    close();
                else if (!error.isEmpty())
                    QMessageBox::warning(this, ui::text("updates.failed"),
                                         ui::text("updates.handoff_failed").arg(error));
            });
            return;
        }
        if (properties_dialog_)
            properties_dialog_->close();
        if (scenario_properties_)
            scenario_properties_->close();
        QMainWindow::closeEvent(event);
    }
    void shutdown() {
        if (stopped_)
            return;
        stopped_ = true;
        timer_.stop();
        if (cancellation_)
            cancellation_->store(true);
        if (display_cancellation_)
            display_cancellation_->store(true);
        if (project_cancellation_)
            project_cancellation_->store(true);
        if (project_candidate_.valid())
            project_candidate_.wait();
        if (candidate_.valid())
            candidate_.wait();
        if (display_candidate_.valid())
            display_candidate_.wait();
        if (model_candidate_.valid())
            model_candidate_.wait();
        if (profile_orbits_.valid())
            profile_orbits_.wait();
        finish_apply_timing("cancelled");
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_)
            run_->stop_inputs();
        if (timing_presentation_)
            timing_metadata_ = timing_presentation_->metadata();
        timing_presentation_.reset();
#endif
        // Wake compiler waiters (including the all-views-closed case) before joining jobs.
        if (viewer_)
            viewer_->status->set(false);
        if (globe_)
            globe_->stop_background_work();
        if (viewer_) {
            viewer_->deviceWaitIdle();
            smoke_render_tasks_.clear();
#ifdef ORBELLE_DESKTOP_BENCHMARK
            for (const auto &slot : slots_)
                if (slot->gpu)
                    try {
                        slot->gpu->drain_after_device_idle();
                    } catch (const std::exception &error) {
                        finished_ = false;
                        if (failure_.isEmpty())
                            failure_ = QString::fromUtf8(error.what());
                    }
#endif
            viewer_->recordAndSubmitTasks.clear();
            viewer_->presentations.clear();
            viewer_->compileManager = {};
            compiler_ = {};
            viewer_->getEventHandlers().clear();
        }
        for (auto &slot : slots_) {
            if (slot->view && globe_)
                globe_->release_view(*slot->view);
            slot->view = {};
            slot->graph = {};
            slot->render_graph = {};
            slot->capture = {};
#ifdef ORBELLE_DESKTOP_BENCHMARK
            slot->gpu.reset();
#endif
            slot->navigation.reset();
            slot->labels.reset();
            slot->station_markers.reset();
            slot->station_selection.reset();
            if (slot->window) {
                slot->window->attached = {};
                slot->window->detaching = {};
                slot->window->failed = {};
                slot->window->synchronize = {};
                slot->window->pressed = {};
            }
            delete slot->container;
            slot->container = nullptr;
            slot->window = nullptr;
        }
        if (scene_)
            scene_->children.clear();
        scene_ = {};
        orbit_group_ = {};
        smoke_old_track_ = {};
        model_group_ = {};
        model_node_ = {};
        for (auto &satellite : satellites_) {
            satellite.model_host->children.clear();
            satellite.model = {};
        }
        markers_.reset();
        selection_.reset();
#ifdef ORBELLE_DESKTOP_BENCHMARK
        // Font sharing caches also retain GPU objects. Release them before the
        // validation report, while preserving the CPU-only benchmark metadata.
        if (benchmark_)
            benchmark_->font = {};
#endif
        globe_.reset();
        viewer_ = {};
        shared_device_ref_ = {};
    }
    int report(int code) {
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_) {
            run_->finish(
                QJsonObject{{"benchmark", benchmark_->report},
                            {"presentation_timing", timing_metadata_},
                            {"gpu_timing",
                             QJsonObject {
                                 { "views",
                                   gpu_metadata_ }
                             }},
                            {"present_timing_capabilities", timing_capabilities_},
                            {"validation_enabled", validation_},
                            {"validation_errors", static_cast<qint64>(log_.errors.load())},
                            {"validation_warnings", static_cast<qint64>(log_.warnings.load())},
                            {"failure", failure_},
                            { "completed",
                              finished_ }});
            if (!finished_ || !failure_.isEmpty() || log_.errors.load())
                return 2;
        }
#endif
        if (!smoke_dir_.isEmpty()) {
            QJsonObject report{{"render_iterations", static_cast<qint64>(frame_count_)},
                               {"qt_version", QString::fromUtf8(qVersion())},
                               {"vsg_version", QString::fromUtf8(vsgGetVersionString())},
                               {"rocky_version", ORBELLE_ROCKY_VERSION},
                               {"vsgqt_version", ORBELLE_VSGQT_VERSION},
                               {"shared_device", shared_device_},
                               {"shared_scene", shared_scene_},
                               {"independent_cameras", independent_cameras_},
                               {"all_windows_closed_before_shutdown", all_windows_closed_},
                               {"vulkan_validation_enabled", validation_},
                               {"validation_errors", static_cast<qint64>(log_.errors.load())},
                               {"validation_warnings", static_cast<qint64>(log_.warnings.load())},
                               {"views", view_report_},
                               {"failure", failure_},
                               {"scope", "integration-smoke; not the 10k performance acceptance"}};
            report.insert("orbit_smoke_stage", orbit_smoke_stage_);
            report.insert("orbit_smoke_scenario", orbit_smoke_scenario());
            report.insert("orbit_smoke_scenarios", smoke_scenario_report_);
            QJsonArray messages;
            {
                std::lock_guard lock(log_.mutex);
                for (const auto &message : log_.messages)
                    messages.append(message);
            }
            report.insert("validation_messages", messages);
            if (eop_catalog_)
                report.insert("data_catalog_warnings",
                              QJsonArray::fromStringList(eop_catalog_->warnings()));
            if (!benchmarking_) {
                report.insert("ui_font_family", QFontInfo(font()).family());
                report.insert(
                    "ground_map",
                    QJsonObject{
                        {"projection",
                         "equirectangular; WGS84 ellipsoid; Rocky conversion; display only"},
                        {"tracks", static_cast<qint64>(ground_map_->track_count())},
                        {"epoch_tai_ns", QString::number(ground_map_->epoch().tai_nanoseconds())}});
            }
            if (scenario_) {
                QJsonArray objects;
                for (const auto &satellite : satellites_)
                    objects.append(QJsonObject{
                        {"id", QString::fromStdString(satellite.orbit->input.object_id)},
                        {"revision",
                         static_cast<qint64>(satellite.orbit->session.provenance().input_revision)},
                        {"model_kind", satellite.model_kind}});
                report.insert("scenario",
                              QJsonObject{{"name", QString::fromStdString(scenario_->name())},
                                          {"revision", static_cast<qint64>(scenario_->revision())},
                                          {"objects", objects}});
            }
            if (orbit_) {
                const auto &provenance = orbit_->track.samples().provenance();
                report.insert(
                    "orbit",
                    QJsonObject{
                        {"object_id", QString::fromStdString(provenance.object_id)},
                        {"input_revision", static_cast<qint64>(provenance.input_revision)},
                        {"algorithm", QString::fromStdString(provenance.algorithm)},
                        {"cache_nodes",
                         static_cast<qint64>(orbit_->track.samples().states().size())},
                        {"first_tai_ns", QString::number(orbit_->input.first.tai_nanoseconds())},
                        {"last_tai_ns", QString::number(orbit_->input.last.tai_nanoseconds())},
                        {"step_seconds", orbit_->input.step_seconds},
                        {"max_quarter_segment_check_error_m", orbit_->max_check_error_m},
                        {"max_inertial_check_error_m", orbit_->max_inertial_check_error_m},
                        {"preparation_seconds", orbit_->preparation_seconds},
                        {"display_frame", display_frame_ == core::FrameId::gcrf ? "GCRF" : "ITRF"},
                        {"min_radius_m", orbit_->min_radius_m},
                        {"max_radius_m", orbit_->max_radius_m},
                        {"eop_source_id", QString::fromStdString(orbit_->eop_source_id)},
                        {"display_model",
                         "GCRF and ITRF cubic Hermite; no extrapolation; display only"}});
                report.insert("interactions", interaction_report_);
                report.insert("imagery", "nasa-blue-marble-land-ocean-ice-2002-2048");
                report.insert("appearance", QJsonObject{{"kind", model_kind_},
                                                        {"diameter_km", model_diameter_km_}});
            }
#ifdef ORBELLE_DESKTOP_BENCHMARK
            if (benchmark_) {
                report.insert("benchmark", benchmark_->report);
                report.insert("interactions", interaction_report_);
                report.insert("scope",
                              "10k workload integration preflight; performance unmeasured");
            }
#endif
            QFile file(smoke_dir_ + "/report.json");
            if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0)
                return 3;
            if (!finished_ || !shared_device_ || !shared_scene_ || !independent_cameras_ ||
                !all_windows_closed_ || log_.errors.load() != 0 || !failure_.isEmpty())
                return 2;
        }
        return code;
    }

  private:
    bool event(QEvent *event) override {
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_ && event->type() == BenchmarkInputEvent::event_type()) {
            try {
                benchmark_input(*static_cast<BenchmarkInputEvent *>(event));
            } catch (const std::exception &error) {
                fail(error.what());
            }
            return true;
        }
#endif
        return QMainWindow::event(event);
    }
    void resize_to_screen(int width, int height) {
        const auto available = screen()->availableGeometry();
        resize(std::min(width, available.width() - 40), std::min(height, available.height() - 80));
        move(available.topLeft() + QPoint(20, 40));
    }
    struct Slot {
        ~Slot() {
            // Covers exceptions before this slot joins slots_. The container
            // owns its QWindow; normal shutdown clears both pointers first.
            if (container)
                delete container;
            else
                delete window;
        }
        EmbeddedWindow *window = nullptr;
        QWidget *container = nullptr;
        QDockWidget *dock = nullptr;
        vsg::ref_ptr<vsg::Camera> camera;
        vsg::ref_ptr<vsg::View> view;
        vsg::ref_ptr<vsg::CommandGraph> graph;
        vsg::ref_ptr<vsg::RenderGraph> render_graph;
        std::unique_ptr<ViewNavigation> navigation;
        vsg::ref_ptr<render_vsg::SwapchainCapture> capture;
#ifdef ORBELLE_DESKTOP_BENCHMARK
        std::shared_ptr<BenchmarkGpu> gpu;
#endif
        std::unique_ptr<render_vsg::LabelLayer> labels;
        std::unique_ptr<render_vsg::MarkerLayer> station_markers, station_selection;
        std::vector<render_vsg::ProjectedMarker> pick_candidates;
        QImage captured_image;
        std::uint64_t creations = 0, destructions = 0, presentations = 0;
    };
    visualization::RenderSnapshot sample_frame(core::Epoch epoch) const {
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (benchmark_)
            return benchmark_->cache->sample(epoch);
#endif
        visualization::RenderSnapshot result{epoch, display_frame_, core::OriginId::earth, {}};
        result.markers.reserve(satellites_.size() + stations_.size());
        std::optional<core::PositionRotation> rotation;
        for (const auto &satellite : satellites_) {
            if (cache_covers(*satellite.orbit, epoch, epoch)) {
                const auto &track = display_frame_ == core::FrameId::gcrf
                                        ? satellite.orbit->inertial_track
                                        : satellite.orbit->track;
                result.markers.push_back(
                    {track.samples().provenance().object_id, track.sample_state(epoch).position_m});
            } else {
                const auto native =
                    satellite.orbit->session.sample(std::array{epoch}).states().front();
                const auto state =
                    core::transform_state(native, display_frame_, *frames_, eop_->at(epoch));
                result.markers.push_back({satellite.orbit->input.object_id, state.position_m});
            }
        }
        for (const auto &station : stations_) {
            auto location = station.config.location;
            // A display-only surface clearance prevents z-fighting and keeps negative-height
            // stations visible on the reference globe. Numerical sessions retain exact height.
            location.height_m = std::max(location.height_m + location.height_above_ground_m, 100.0);
            location.height_above_ground_m = 0;
            auto position = core::ground_station_position(location);
            if (display_frame_ == core::FrameId::gcrf) {
                if (!rotation)
                    rotation = frames_->gcrf_to_itrf(epoch, eop_->at(epoch).sample);
                position = rotation->inverse_position(position);
            }
            result.markers.push_back({station.config.object_id, position});
        }
        return result;
    }
    double duration_seconds() const {
        return benchmarking_ ? 600
               : scenario_   ? scenario_->last().seconds_since(scenario_->first())
                             : 1;
    }
    bool busy() const {
        return candidate_.valid() || model_candidate_.valid() || project_candidate_.valid();
    }
    void update_actions() {
        if (benchmarking_)
            return;
        const bool active = scenario_.has_value();
        const bool idle = !busy();
        if (station_editor_)
            station_editor_->set_busy(!idle);
        if (sensor_editor_)
            sensor_editor_->setEnabled(idle);
        for (auto *action : {rename_action_, duplicate_action_, delete_action_})
            if (action)
                action->setEnabled(selected_marker_index().has_value() && idle);
        if (eop_action_)
            eop_action_->setEnabled(idle);
        if (report_action_)
            report_action_->setEnabled(active && idle);
        if (cancel_project_button_)
            cancel_project_button_->setVisible(project_candidate_.valid());
        if (scenario_properties_) {
            for (const auto *name :
                 {"scenarioSettingsFields", "applyScenarioSettings", "resetScenarioSettings"})
                if (auto *control = scenario_properties_->findChild<QWidget *>(name))
                    control->setEnabled(idle);
        }
        if (open_action_)
            open_action_->setEnabled(idle);
        if (save_action_)
            save_action_->setEnabled(active && idle);
        if (save_as_action_)
            save_as_action_->setEnabled(active && idle);
        if (new_action_)
            new_action_->setEnabled(idle);
        if (example_action_)
            example_action_->setEnabled(idle && runtime::two_body_available());
        if (insert_action_)
            insert_action_->setEnabled(active && idle && editor_);
        for (auto *action : quick_insert_actions_)
            if (action)
                action->setEnabled(active && idle && editor_);
        if (insert_dialog_) {
            insert_dialog_->set_busy(!active || !idle);
            if (active && idle && insert_dialog_->isVisible())
                insert_dialog_->set_parents(project_object_choices(project_snapshot(project_path_),
                                                                   {ObjectKind::satellite}));
        }
        if (insert_menu_)
            insert_menu_->setEnabled(active);
        if (time_toolbar_) {
            time_toolbar_->setEnabled(active);
            time_toolbar_->setVisible(active);
        }
        if (object_dock_) {
            if (!active)
                object_dock_->hide();
            object_dock_->toggleViewAction()->setEnabled(active);
        }
        if (time_menu_)
            time_menu_->setEnabled(active);
        if (fit_action_)
            fit_action_->setEnabled(active);
        if (aux_action_)
            aux_action_->setEnabled(active);
        if (map_action_)
            map_action_->setEnabled(active);
        if (properties_action_)
            properties_action_->setEnabled(
                idle && (selected_marker_index().has_value() ||
                         (scenario_ && objects_->currentItem() == scenario_item_)));
        if (focus_action_)
            focus_action_->setEnabled(selected_marker_index().has_value());
        for (auto *button : view_focus_buttons_)
            button->setEnabled(selected_marker_index().has_value());
    }
    std::optional<std::size_t> object_index(const QTreeWidgetItem *item) const {
        if (item && !item->data(0, Qt::UserRole + 17).toString().isEmpty())
            return object_index(item->parent());
        for (std::size_t i = 0; i < satellites_.size(); ++i)
            if (satellites_[i].item == item)
                return i;
        for (std::size_t i = 0; i < stations_.size(); ++i)
            if (stations_[i].item == item)
                return satellites_.size() + i;
        return std::nullopt;
    }
    void filter_objects() {
        const auto *search = findChild<QLineEdit *>("objectSearch");
        const auto text = search ? search->text() : QString{};
        for (auto &satellite : satellites_) {
            const bool parent_match = satellite.name().contains(text, Qt::CaseInsensitive);
            bool child_match = false;
            for (int i = 0; i < satellite.item->childCount(); ++i) {
                auto *child = satellite.item->child(i);
                const bool match = child->text(0).contains(text, Qt::CaseInsensitive);
                child->setHidden(!parent_match && !match);
                child_match = child_match || match;
            }
            satellite.item->setHidden(!parent_match && !child_match);
            if (child_match && !text.isEmpty()) {
                scenario_item_->setExpanded(true);
                satellite.item->setExpanded(true);
            }
        }
        for (auto &station : stations_)
            station.item->setHidden(!station.name().contains(text, Qt::CaseInsensitive));
    }
    const application::ProjectSensor *selected_sensor() const {
        if (!objects_ || !objects_->currentItem())
            return nullptr;
        const auto id =
            objects_->currentItem()->data(0, Qt::UserRole + 17).toString().toStdString();
        const auto found = std::ranges::find(sensors_, id, &application::ProjectSensor::object_id);
        return found == sensors_.end() ? nullptr : &*found;
    }
    void edit_sensor(bool creating, bool defaults = false, const std::string &parent_id = {}) {
        auto parent_index = selected_;
        if (!parent_id.empty()) {
            const auto found = std::ranges::find_if(satellites_, [&](const auto &satellite) {
                return satellite.orbit->input.object_id == parent_id;
            });
            if (found == satellites_.end())
                return;
            parent_index = static_cast<std::size_t>(found - satellites_.begin());
        }
        if (busy() || !parent_index)
            return;
        if (creating && !defaults && sensor_editor_bound_ && sensor_editor_creating_ &&
            sensor_editor_->bound_parent_id() ==
                satellites_[*parent_index].orbit->input.object_id) {
            workspace_->open(sensor_editor_, false);
            return;
        }
        if (!sensor_editor_) {
            sensor_editor_ = new SensorEditor(this);
            sensor_editor_->apply = [this](application::ProjectSensor sensor) {
                if (busy())
                    throw std::runtime_error(ui::utf8("graphics_workbench.validation_2"));
                auto p = project_snapshot(project_path_);
                const auto id = sensor.object_id;
                auto found = std::ranges::find(p.sensors, sensor.object_id,
                                               &application::ProjectSensor::object_id);
                if (found != p.sensors.end() && same_sensor_input(*found, sensor)) {
                    statusBar()->showMessage(ui::text("graphics_workbench.label_13"), 4000);
                    return;
                }
                if (found != p.sensors.end()) {
                    auto renamed = *found;
                    renamed.display_name = sensor.display_name;
                    if (same_sensor_input(renamed, sensor)) {
                        rename_object(id, sensor.name());
                        sensor_editor_->accept_sensor(sensor);
                        statusBar()->showMessage(ui::text("graphics_workbench.label_31"), 4000);
                        return;
                    }
                }
                if (found == p.sensors.end())
                    p.sensors.push_back(std::move(sensor));
                else
                    *found = std::move(sensor);
                application::validate_project(p);
                if (start_project(std::move(p), project_path_, true, selected_marker_index())) {
                    pending_project_sensor_selection_ = id;
                    pending_sensor_apply_ = id;
                }
            };
        }
        application::ProjectSensor sensor;
        if (creating) {
            sensor.object_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            sensor.display_name = "Sensor " + std::to_string(sensors_.size() + 1);
            sensor.parent_satellite_id = satellites_[*parent_index].orbit->input.object_id;
        } else {
            if (!selected_sensor())
                return;
            sensor = *selected_sensor();
        }
        if (!creating && sensor_editor_bound_ &&
            sensor_editor_->bound_object_id() == sensor.object_id) {
            workspace_->open(sensor_editor_, false);
            return;
        }
        if (sensor_editor_bound_ && sensor_editor_->has_unapplied_changes() &&
            !confirm_discard_property(sensor_editor_))
            return;
        sensor_editor_->set_sensor(std::move(sensor), project_snapshot(project_path_), creating);
        sensor_editor_bound_ = true;
        sensor_editor_creating_ = creating;
        if (defaults) {
            sensor_editor_->close();
            sensor_editor_->apply(sensor_editor_->sensor());
        } else
            workspace_->open(sensor_editor_, false);
    }
    QString object_pair_name(const std::vector<std::string> &ids) const {
        if (ids.size() != 2)
            return {};
        const auto choices = project_object_choices(project_snapshot(project_path_));
        const auto name = [&](const std::string &id) {
            const auto found = std::ranges::find(choices, id, &ObjectChoice::id);
            return found == choices.end() ? QString::fromStdString(id) : found->name;
        };
        return name(ids[0]) + QStringLiteral(" → ") + name(ids[1]);
    }
    void clear_access_display() {
        access_session_.reset();
        access_ids_.clear();
        access_hash_.clear();
        if (timeline_)
            timeline_->set_access_intervals({}, {});
        if (ground_map_)
            ground_map_->set_access_link({}, false);
    }
    void update_access_display() {
        if (!access_session_)
            return;
        const auto station = std::ranges::find_if(
            stations_, [&](const auto &s) { return s.config.object_id == access_ids_[0]; });
        auto parent = access_ids_[1];
        const auto sensor =
            std::ranges::find(sensors_, parent, &application::ProjectSensor::object_id);
        if (sensor != sensors_.end())
            parent = sensor->parent_satellite_id;
        const auto satellite = std::ranges::find_if(
            satellites_, [&](const auto &s) { return s.orbit->input.object_id == parent; });
        if (station == stations_.end() || satellite == satellites_.end()) {
            clear_access_display();
            return;
        }
        try {
            const bool visible = access_session_->at(snapshot_.epoch).visible;
            ground_map_->set_access_link(
                std::pair{satellites_.size() +
                              static_cast<std::size_t>(station - stations_.begin()),
                          static_cast<std::size_t>(satellite - satellites_.begin())},
                visible);
        } catch (const std::exception &error) {
            clear_access_display();
            statusBar()->showMessage(
                ui::text("graphics_workbench.label_14").arg(QString::fromUtf8(error.what())), 6000);
        }
    }
    std::optional<std::size_t> selected_marker_index() const {
        return selected_station_
                   ? std::optional<std::size_t>{satellites_.size() + *selected_station_}
                   : selected_;
    }
    QString selected_name() const {
        if (const auto *sensor = selected_sensor())
            return QString::fromStdString(sensor->name());
        return selected_           ? satellites_.at(*selected_).name()
               : selected_station_ ? stations_.at(*selected_station_).name()
                                   : QString{};
    }
    bool object_id_exists(const std::string &id) const {
        return std::ranges::any_of(sensors_, [&](const auto &s) { return s.object_id == id; }) ||
               std::any_of(satellites_.begin(), satellites_.end(),
                           [&](const auto &s) { return s.orbit->input.object_id == id; }) ||
               std::any_of(stations_.begin(), stations_.end(),
                           [&](const auto &s) { return s.config.object_id == id; });
    }
    void mark_project_dirty(bool dirty = true) {
        project_dirty_ = dirty;
        if (!access_hash_.empty() && scenario_ &&
            runtime::report_input_sha256(project_snapshot(project_path_)) != access_hash_)
            clear_access_display();
        if (report_dialog_ && scenario_)
            report_dialog_->update_project(project_snapshot(project_path_), active_time());
        if (report_creation_ && scenario_)
            report_creation_->update_project(project_snapshot(project_path_));
        if (scenario_) {
            const auto filename = project_path_.isEmpty() ? ui::text("graphics_workbench.filename")
                                                          : QFileInfo(project_path_).fileName();
            setWindowTitle(QString::fromStdString(scenario_->name()) + QStringLiteral(" · ") +
                           filename + ui::text("window.modified_title_suffix"));
        }
        setWindowModified(dirty);
    }
    application::Project project_snapshot(const QString &destination) const {
        if (!scenario_)
            throw application::ProjectError("No active project");
        application::Project project{scenario_->name(), scenario_->first(), scenario_->last()};
        project.current = snapshot_.epoch;
        project.earth_orientation = eop_->data_info();
        project.time_data = standalone_time_data_;
        project.analysis_record = analysis_record_;
        project.force_data = force_data_references(scenario_->data_lock());
        if (project.data_lock() != scenario_->data_lock())
            throw application::ProjectError(
                "Scenario data lock does not match its active providers");
        const QDir directory(QFileInfo(destination).absolutePath());
        for (const auto &satellite : satellites_) {
            application::ProjectSatellite object;
            object.orbit = satellite.orbit->input;
            object.display_name = satellite.display_name;
            object.display = satellite.display;
            object.model_kind = satellite.model_kind;
            object.model_diameter_m = satellite.model_diameter_km * 1000;
            if (object.model_kind == 2)
                object.model_path =
                    directory.relativeFilePath(QFileInfo(satellite.model_path).absoluteFilePath())
                        .toStdString();
            project.satellites.push_back(std::move(object));
        }
        for (const auto &station : stations_)
            project.ground_stations.push_back(station.config);
        project.reports = reports_;
        project.sensors = sensors_;
        return project;
    }
    std::vector<runtime::CatalogGravity>
    selected_gravity(const application::Project &project) const {
        std::vector<runtime::CatalogGravity> result;
        const auto installed = eop_catalog_->data().gravity_models();
        for (const auto &satellite : project.satellites) {
            const auto &input = satellite.orbit;
            if (!input.uses_hpop() || input.hpop.gravity_model.empty())
                continue;
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == input.hpop.gravity_model; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(), [&](const auto &v) {
                return v.selection == input.hpop.gravity_model;
            });
            if (found == installed.end())
                throw std::runtime_error(ui::utf8("graphics_workbench.validation_3") +
                                         input.hpop.gravity_model);
            result.push_back(*found);
        }
        return result;
    }
    std::vector<runtime::CatalogEphemeris>
    selected_ephemerides(const application::Project &project) const {
        std::vector<runtime::CatalogEphemeris> result;
        const auto installed = eop_catalog_->data().ephemerides();
        std::vector<std::string> ids;
        for (const auto &ref : project.force_data)
            if (ref.role.starts_with("ephemeris/"))
                ids.push_back(ref.role.substr(10));
        for (const auto &report : project.reports)
            if (application::report_is_lighting(report.query.kind))
                ids.push_back(report.query.lighting_ephemeris);
        for (const auto &satellite : project.satellites)
            if (satellite.orbit.uses_hpop() &&
                satellite.orbit.hpop_resource_options().uses_ephemeris())
                ids.push_back(satellite.orbit.hpop.ephemeris_model);
        for (const auto &id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == id; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(),
                                            [&](const auto &v) { return v.selection == id; });
            if (found == installed.end())
                throw std::runtime_error(ui::utf8("graphics_workbench.validation_4") + id);
            result.push_back(*found);
        }
        return result;
    }
    std::vector<runtime::CatalogSpaceWeather>
    selected_weather(const application::Project &project) const {
        std::vector<runtime::CatalogSpaceWeather> result;
        const auto installed = eop_catalog_->data().weather_sources();
        for (const auto &satellite : project.satellites) {
            const auto &input = satellite.orbit;
            const auto &id = input.hpop.drag.weather_model;
            if (!input.uses_hpop() || !input.hpop.drag.enabled || id.empty())
                continue;
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == id; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(),
                                            [&](const auto &v) { return v.selection == id; });
            if (found == installed.end())
                throw std::runtime_error(ui::utf8("graphics_workbench.validation_5") + id);
            result.push_back(*found);
        }
        return result;
    }
    std::vector<runtime::CatalogOceanTides>
    selected_ocean_tides(const application::Project &project) const {
        std::vector<runtime::CatalogOceanTides> result;
        const auto installed = eop_catalog_->data().ocean_tide_models();
        for (const auto &satellite : project.satellites) {
            const auto &input = satellite.orbit;
            const auto &id = input.hpop.ocean_tides.model;
            if (!input.uses_hpop() || !input.hpop.ocean_tides.active())
                continue;
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == id; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(),
                                            [&](const auto &v) { return v.selection == id; });
            if (found == installed.end())
                throw std::runtime_error(ui::utf8("graphics_workbench.validation_6") + id);
            result.push_back(*found);
        }
        return result;
    }
    application::DataLock selected_data_lock(const application::Project &project,
                                             const core::EopDataInfo &eop) const {
        const auto base = application::eop_data_lock(eop);
        std::vector<application::DataReference> refs(base.references().begin(),
                                                     base.references().end());
        for (const auto &model : selected_gravity(project))
            refs.push_back(runtime::gravity_reference(model.selection, model.info));
        for (const auto &model : selected_ephemerides(project))
            refs.push_back(runtime::ephemeris_reference(model.selection, model.info));
        for (const auto &model : selected_weather(project))
            refs.push_back(runtime::space_weather_reference(model.selection, model.info));
        for (const auto &model : selected_ocean_tides(project))
            refs.push_back(runtime::ocean_tide_reference(model.selection, model.info));
        return application::DataLock{std::move(refs)};
    }
    ReportInput report_input() const {
        auto resources = runtime::DataSnapshot::from_providers(active_time(), frames_, eop_);
        for (const auto &[id, model] : gravity_)
            resources = resources.with_gravity(id, model);
        for (const auto &[id, model] : ephemerides_)
            resources = resources.with_ephemeris(id, model);
        for (const auto &[id, model] : weather_)
            resources = resources.with_space_weather(id, model);
        for (const auto &[id, model] : ocean_tides_)
            resources = resources.with_ocean_tides(id, model);
        return {project_snapshot(project_path_), std::move(resources), orbit_sessions_};
    }
    void create_report(const std::string &context = {},
                       std::optional<application::ReportKind> preset = {}) {
        if (!scenario_ || busy() || report_creation_)
            return;
        if (report_dialog_ && report_dialog_->is_busy()) {
            workspace_->open(report_dialog_, false);
            return;
        }
        ReportCreationDialog chooser(project_snapshot(project_path_), context, preset, this);
        report_creation_ = &chooser;
        const bool accepted = chooser.exec() == QDialog::Accepted;
        report_creation_ = nullptr;
        if (!accepted || !scenario_ || busy())
            return;
        show_reports();
        report_dialog_->configure_report(chooser.definition());
    }
    void show_reports() {
        if (!scenario_ || busy())
            return;
        if (!report_dialog_) {
            report_dialog_ = new ReportDialog(
                project_snapshot(project_path_), active_time(),
                [this](const application::ReportQuery &query, ReportDialog::Ready done) {
                    try {
                        if (busy()) {
                            done({}, ui::text("graphics_workbench.label_15"));
                            return;
                        }
                        auto requested = project_snapshot(project_path_);
                        if (application::report_is_lighting(query.kind)) {
                            const auto role = "ephemeris/" + query.lighting_ephemeris;
                            if (std::none_of(requested.force_data.begin(),
                                             requested.force_data.end(),
                                             [&](const auto &ref) { return ref.role == role; })) {
                                const auto installed = eop_catalog_->data().ephemerides();
                                const auto found = std::find_if(
                                    installed.begin(), installed.end(), [&](const auto &v) {
                                        return v.selection == query.lighting_ephemeris;
                                    });
                                if (found == installed.end())
                                    throw std::runtime_error(
                                        ui::utf8("graphics_workbench.validation_7"));
                                requested.force_data.push_back(
                                    runtime::ephemeris_reference(found->selection, found->info));
                            }
                        }
                        const auto [data_first, data_last] =
                            application::calculation_data_interval(requested);
                        const auto current = data_for_interval(data_first, data_last);
                        if (scenario_->data_lock() !=
                            selected_data_lock(requested, current->data_info())) {
                            if (start_project(std::move(requested), project_path_, true,
                                              selected_marker_index()))
                                pending_report_input_ = std::move(done);
                            else
                                done({}, ui::text("graphics_workbench.label_16"));
                        } else
                            done(report_input(), {});
                    } catch (const std::exception &error) {
                        done({}, QString::fromUtf8(error.what()));
                    }
                },
                [this](std::vector<application::ReportDefinition> reports) {
                    if (busy())
                        throw std::runtime_error(ui::utf8("graphics_workbench.validation_8"));
                    auto project = project_snapshot(project_path_);
                    project.reports = reports;
                    application::validate_project(project);
                    reports_ = std::move(reports);
                    mark_project_dirty();
                },
                [this](core::Epoch epoch) {
                    set_playing(false);
                    seek_epoch(epoch);
                },
                this);
            report_dialog_
                ->result_published = [this](std::shared_ptr<const runtime::ReportResult> result,
                                            const runtime::Analysis &analysis) {
                clear_access_display();
                if (result->intersatellite_statistics) {
                    std::vector<std::pair<qint64, qint64>> windows;
                    for (const auto &w : result->intersatellite_statistics->windows)
                        windows.emplace_back(w.first.tai_nanoseconds() / 1'000'000,
                                             w.last.tai_nanoseconds() / 1'000'000);
                    const auto &ids = result->definition.query.object_ids;
                    access_hash_ = runtime::report_input_sha256(*analysis.snapshot().project);
                    access_ids_ = ids;
                    timeline_->set_access_intervals(std::move(windows), object_pair_name(ids));
                    return;
                }
                if (!application::report_is_access(result->definition.query.kind))
                    return;
                const auto &ids = result->definition.query.object_ids;
                // The map/timeline session uses the same confirmed resources as the report.
                const auto &data = analysis.data();
                access_session_.emplace(
                    runtime::ReportContext{*analysis.snapshot().project, data.time(), data.frames(),
                                           data.eop(), data.gravity_models(), data.ephemerides(),
                                           data.weather_sources(), data.ocean_tide_models()},
                    ids[0], ids[1]);
                access_hash_ = runtime::report_input_sha256(*analysis.snapshot().project);
                access_ids_ = ids;
                std::vector<std::pair<qint64, qint64>> windows;
                if (result->definition.query.kind == application::ReportKind::access_intervals) {
                    const auto &source =
                        result->source_table ? *result->source_table : result->table;
                    for (const auto &row : source.rows())
                        windows.emplace_back(
                            std::get<core::Epoch>(row[2]).tai_nanoseconds() / 1'000'000,
                            std::get<core::Epoch>(row[3]).tai_nanoseconds() / 1'000'000);
                    timeline_->set_access_intervals(std::move(windows), object_pair_name(ids));
                }
                update_access_display();
            };
        }
        workspace_->open(report_dialog_, false);
    }
    void close_reports() {
        clear_access_display();
        if (report_dialog_) {
            workspace_->close_page(report_dialog_);
            delete report_dialog_;
            report_dialog_ = nullptr;
        }
        reports_.clear();
    }
    void save_project_to(const QString &path) {
        const auto absolute = QFileInfo(path).absoluteFilePath();
        application::save_project(std::filesystem::path(absolute.toStdWString()),
                                  project_snapshot(absolute), true);
        project_path_ = absolute;
        mark_project_dirty(false);
        statusBar()->showMessage(ui::text("graphics_workbench.label_17").arg(project_path_), 6000);
    }
    bool save_project_dialog(bool save_as) {
        if (!scenario_ || busy())
            return false;
        QString path = project_path_;
        if (save_as || path.isEmpty()) {
            QFileDialog dialog(this, ui::text("graphics_workbench.path"));
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.setFileMode(QFileDialog::AnyFile);
            dialog.setNameFilter(ui::text("graphics_workbench.label_18"));
            dialog.setDefaultSuffix(QStringLiteral("orbelle"));
            dialog.selectFile(path.isEmpty() ? ui::text("graphics_workbench.label_19") : path);
            if (dialog.exec() != QDialog::Accepted)
                return false;
            path = dialog.selectedFiles().value(0);
        }
        try {
            save_project_to(path);
            return true;
        } catch (const std::exception &error) {
            QMessageBox::warning(this, ui::text("graphics_workbench.label_20"),
                                 QString::fromUtf8(error.what()));
            return false;
        }
    }
    bool scenario_has_draft() const {
        if (!scenario_editor_bound_ || !scenario_properties_ || !scenario_)
            return false;
        try {
            return scenario_properties_->findChild<QLineEdit *>("scenarioSettingsName")
                           ->text()
                           .trimmed()
                           .toStdString() != scenario_->name() ||
                   dynamic_cast<EpochEditor *>(
                       scenario_properties_->findChild<QWidget *>("scenarioSettingsStart"))
                           ->epoch() != scenario_->first() ||
                   dynamic_cast<EpochEditor *>(
                       scenario_properties_->findChild<QWidget *>("scenarioSettingsEnd"))
                           ->epoch() != scenario_->last();
        } catch (const std::exception &) {
            return true;
        }
    }
    std::vector<QDialog *> unapplied_properties() const {
        std::vector<QDialog *> drafts;
        if (orbit_editor_bound_ && editor_->has_unapplied_changes())
            drafts.push_back(properties_dialog_);
        if (station_editor_bound_ && station_editor_->has_unapplied_changes())
            drafts.push_back(station_editor_);
        if (sensor_editor_bound_ && sensor_editor_->has_unapplied_changes())
            drafts.push_back(sensor_editor_);
        if (scenario_has_draft())
            drafts.push_back(scenario_properties_);
        return drafts;
    }
    bool confirm_discard_property(QDialog *page) {
        QMessageBox question(QMessageBox::Question, ui::text("graphics_workbench.label_21"),
                             ui::text("graphics_workbench.label_22").arg(page->windowTitle()),
                             QMessageBox::Discard | QMessageBox::Cancel, this);
        question.setObjectName("discardPropertyDraft");
        question.button(QMessageBox::Discard)->setText(ui::text("graphics_workbench.label_23"));
        question.button(QMessageBox::Cancel)->setText(ui::text("graphics_workbench.label_24"));
        question.setDefaultButton(QMessageBox::Cancel);
        if (question.exec() == QMessageBox::Discard)
            return true;
        workspace_->open(page, page == scenario_properties_);
        return false;
    }
    bool confirm_replace_scenario() {
        if (!scenario_)
            return true;
        const auto drafts = unapplied_properties();
        if (!project_dirty_ && drafts.empty())
            return true;
        QString message;
        if (!drafts.empty()) {
            QStringList names;
            for (const auto *draft : drafts)
                names << draft->windowTitle();
            message = ui::text("graphics_workbench.label_25").arg(names.join("\n"));
        } else
            message = ui::text("graphics_workbench.label_26");
        const auto buttons = QMessageBox::Discard | QMessageBox::Cancel |
                             (project_dirty_ ? QMessageBox::Save : QMessageBox::NoButton);
        QMessageBox question(QMessageBox::Question, ui::text("graphics_workbench.label_27"),
                             message, buttons, this);
        question.setObjectName("leaveScenarioConfirmation");
        question.setDefaultButton(drafts.empty() ? QMessageBox::Save : QMessageBox::Cancel);
        if (!drafts.empty()) {
            question.button(QMessageBox::Cancel)->setText(ui::text("graphics_workbench.label_28"));
            question.button(QMessageBox::Discard)->setText(ui::text("graphics_workbench.label_29"));
            if (project_dirty_)
                question.button(QMessageBox::Save)
                    ->setText(ui::text("graphics_workbench.label_30"));
        }
        const auto answer = question.exec();
        if (answer == QMessageBox::Cancel && !drafts.empty())
            workspace_->open(drafts.front(), drafts.front() == scenario_properties_);
        if (answer == QMessageBox::Save)
            return save_project_dialog(false);
        return answer == QMessageBox::Discard;
    }
    void refresh_property_bindings() {
        if (orbit_editor_bound_) {
            editor_->set_time_provider(active_time());
            const auto found = std::ranges::find_if(satellites_, [this](const auto &satellite) {
                return satellite.orbit->input.object_id == editor_->bound_object_id();
            });
            if (found != satellites_.end()) {
                editor_target_ = static_cast<std::size_t>(found - satellites_.begin());
                editor_->set_create_mode(false);
                if (project_orbit_edit_)
                    editor_->accept_input(found->orbit->input);
                editor_->sync_name(found->name().toStdString());
                editor_->set_analysis_interval(scenario_->first(), scenario_->last());
                properties_dialog_->setWindowTitle(
                    found->name() +
                    ui::text("graphics_workbench.properties_dialog.set_window_title_2"));
            } else if (editor_target_) {
                properties_dialog_->close();
                orbit_editor_bound_ = false;
                editor_target_.reset();
            } else {
                editor_->set_analysis_interval(scenario_->first(), scenario_->last());
            }
        }
        if (station_editor_bound_) {
            const auto found = std::ranges::find_if(stations_, [this](const auto &station) {
                return station.config.object_id == station_editor_->bound_object_id();
            });
            if (found != stations_.end()) {
                if (pending_station_apply_ == found->config.object_id)
                    station_editor_->accept_station(found->config);
                else if (!station_editor_->has_unapplied_changes())
                    station_editor_->set_station(found->config, false);
                station_editor_->sync_name(found->config.name());
                station_editor_target_ = found->config.object_id;
                station_editor_->setWindowTitle(
                    found->name() + ui::text("graphics_workbench.station_editor.set_window_title"));
                station_editor_->show_result(pending_station_apply_ == found->config.object_id
                                                 ? ui::text("graphics_workbench.label_31")
                                                 : QString{});
            } else if (station_editor_target_) {
                station_editor_->close();
                station_editor_bound_ = false;
                station_editor_target_.reset();
            }
        }
        if (sensor_editor_bound_) {
            const auto found = std::ranges::find(sensors_, sensor_editor_->bound_object_id(),
                                                 &application::ProjectSensor::object_id);
            if (found != sensors_.end()) {
                if (pending_sensor_apply_ == found->object_id)
                    sensor_editor_->accept_sensor(*found);
                else if (!sensor_editor_->has_unapplied_changes())
                    sensor_editor_->set_sensor(*found, project_snapshot(project_path_), false);
                sensor_editor_->sync_name(found->name());
                sensor_editor_creating_ = false;
                sensor_editor_->setWindowTitle(
                    QString::fromStdString(found->name()) +
                    ui::text("graphics_workbench.sensor_editor.set_window_title"));
            } else if (!sensor_editor_creating_ ||
                       std::ranges::none_of(satellites_, [this](const auto &satellite) {
                           return satellite.orbit->input.object_id ==
                                  sensor_editor_->bound_parent_id();
                       })) {
                sensor_editor_->close();
                sensor_editor_bound_ = false;
            }
            if (sensor_editor_bound_)
                sensor_editor_->update_project(project_snapshot(project_path_));
        }
        if (scenario_editor_bound_) {
            for (const auto *id : {"scenarioSettingsStart", "scenarioSettingsEnd"})
                dynamic_cast<EpochEditor *>(scenario_properties_->findChild<QWidget *>(id))
                    ->set_time_provider(active_time());
            scenario_properties_->setWindowTitle(
                QString::fromStdString(scenario_->name()) +
                ui::text("graphics_workbench.scenario_properties.set_window_title"));
            scenario_properties_->findChild<QLabel *>("scenarioObjectCount")
                ->setText(QString::number(satellites_.size() + stations_.size()));
            scenario_properties_->findChild<QLineEdit *>("scenarioDataHash")
                ->setText(QString::fromStdString(scenario_->data_lock().sha256()));
            auto *caption = scenario_properties_->findChild<QLabel *>("scenarioEopData");
            caption->setText(QString::fromStdString(eop_->data_info().id) +
                             (eop_->data_info().interpolation.starts_with("finals2000a")
                                  ? ui::text("graphics_workbench.label_32")
                                  : QString{}));
            caption->setToolTip(QString::fromStdString(eop_->data_info().id));
            scenario_properties_->findChild<QLabel *>("scenarioExtrapolation")
                ->setText(extrapolation_description(eop_->data_info(), scenario_->first(),
                                                    scenario_->last()));
        }
    }
    void choose_project() {
        if (busy())
            return;
        const auto path =
            QFileDialog::getOpenFileName(this, ui::text("graphics_workbench.path_2"), project_path_,
                                         ui::text("graphics_workbench.label_18"));
        if (path.isEmpty())
            return;
        try {
            // Parse before prompting to replace; an invalid file cannot disturb the current
            // project.
            auto project = application::load_project(std::filesystem::path(path.toStdWString()));
            if (confirm_replace_scenario())
                start_project(std::move(project), path);
        } catch (const std::exception &error) {
            QMessageBox::warning(this, ui::text("graphics_workbench.label_33"),
                                 QString::fromUtf8(error.what()));
        }
    }
    void show_data_coverage(core::Epoch first, core::Epoch last) {
        const auto label = [this](core::Epoch epoch) {
            try {
                return desktop::calendar_text(epoch, CalendarScale::utc, true,
                                              eop_catalog_->time_provider().get()) +
                       QStringLiteral(" UTC");
            } catch (const std::out_of_range &) {
                return QString::number(epoch.tai_nanoseconds()) + QStringLiteral(" TAI ns");
            }
        };
        QString coverage;
        for (const auto index : eop_catalog_->current_indices()) {
            const auto &info = eop_catalog_->providers().at(index)->data_info();
            coverage += ui::text("graphics_workbench.info")
                            .arg(label(info.first_epoch), label(info.last_epoch))
                            .arg(eop_catalog_->extrapolation_days(index));
        }
        QMessageBox message(
            QMessageBox::Warning, ui::text("graphics_workbench.label_34"),
            ui::text("graphics_workbench.label_35").arg(label(first), label(last), coverage),
            QMessageBox::Cancel, this);
        message.setObjectName("dataCoverageDialog");
        auto *manage =
            message.addButton(ui::text("graphics_workbench.manage"), QMessageBox::ActionRole);
        message.exec();
        if (message.clickedButton() == manage)
            show_data_center(this, *eop_catalog_);
    }
    QString extrapolation_description(const core::EopDataInfo &info, core::Epoch first,
                                      core::Epoch last) const {
        const auto time = time_for(info.time_data);
        const auto calendar_text = [&](core::Epoch epoch, CalendarScale scale) {
            return desktop::calendar_text(epoch, scale, true, time.get());
        };
        QStringList ranges;
        if (first.tai_nanoseconds() < info.first_epoch.tai_nanoseconds())
            ranges.push_back(
                calendar_text(first, CalendarScale::utc) + QStringLiteral(" — ") +
                calendar_text(core::Epoch{std::min(last.tai_nanoseconds(),
                                                   info.first_epoch.tai_nanoseconds())},
                              CalendarScale::utc));
        if (last.tai_nanoseconds() > info.last_epoch.tai_nanoseconds())
            ranges.push_back(calendar_text(core::Epoch{std::max(first.tai_nanoseconds(),
                                                                info.last_epoch.tai_nanoseconds())},
                                           CalendarScale::utc) +
                             QStringLiteral(" — ") + calendar_text(last, CalendarScale::utc));
        return ranges.empty() ? ui::text("graphics_workbench.label_36")
                              : ui::text("graphics_workbench.label_37")
                                    .arg(info.extrapolation_days)
                                    .arg(ranges.join(QStringLiteral("\n")));
    }
    void finish_apply_timing(const QString &outcome) {
        if (auto timing = std::exchange(apply_timing_, {}))
            timing->finish(outcome);
    }
    bool start_project(application::Project project, const QString &path, bool editing = false,
                       std::optional<std::size_t> selection = std::nullopt,
                       std::optional<std::string> edited_object = std::nullopt,
                       std::optional<ScenarioSettings> settings = std::nullopt) {
        if (busy())
            throw std::logic_error("Finish the pending update before opening a project");
        const bool own_timing = editing && !apply_timing_;
        if (own_timing)
            apply_timing_ = ApplyTiming::begin("project_apply", ++apply_timing_sequence_);
        last_project_outcome_ = "rejected";
        const auto finish_request = qScopeGuard([&] {
            if (own_timing && apply_timing_ && !project_candidate_.valid()) {
                apply_timing_->requested();
                finish_apply_timing(last_project_outcome_.isEmpty() ? "rejected"
                                                                    : last_project_outcome_);
            }
        });
        const auto first = settings ? settings->first : project.first;
        const auto last = settings ? settings->last : project.last;
        // Check edits before asking to replace calculation data. The original lock
        // remains available for consent even when its coverage cannot span the new interval.
        if (settings)
            application::validate_project(application::Project{settings->name, first, last});
        auto data_input = project;
        data_input.first = first;
        data_input.last = last;
        const auto [data_first, data_last] = application::calculation_data_interval(data_input);
        EopCatalog::Provider next_data;
        try {
            next_data = data_for_interval(data_first, data_last);
        } catch (const std::out_of_range &) {
            last_project_outcome_ = "rejected";
            show_data_coverage(data_first, data_last);
            return false;
        }
        const auto gravity_files = selected_gravity(project);
        const auto ephemeris_files = selected_ephemerides(project);
        startup_resources_.reconcile(eop_catalog_->data().gravity_models(),
                                     eop_catalog_->data().ephemerides());
        const auto weather_files = selected_weather(project);
        const auto ocean_files = selected_ocean_tides(project);
        const auto selected_lock = selected_data_lock(project, next_data->data_info());
        const auto changes = calculation_data_changes(project.data_lock(), selected_lock, editing);
        if (!changes.empty()) {
            QStringList descriptions, details;
            for (const auto &change : changes) {
                const auto &ref = change.after ? *change.after : *change.before;
                const auto role = QString::fromStdString(ref.role);
                const auto label =
                    ref.role == "earth-orientation"      ? ui::text("graphics_workbench.label_38")
                    : ref.role == "utc-time"             ? ui::text("graphics_workbench.label_39")
                    : ref.role.starts_with("gravity/")   ? ui::text("graphics_workbench.label_40")
                    : ref.role.starts_with("ephemeris/") ? ui::text("graphics_workbench.label_41")
                    : ref.role.starts_with("space-weather/")
                        ? ui::text("graphics_workbench.label_42")
                    : ref.role.starts_with("ocean-tides/") ? ui::text("graphics_workbench.label_43")
                                                           : role;
                const auto old_id = change.before
                                        ? QString::fromStdString(change.before->snapshot_id)
                                        : ui::text("graphics_workbench.label_44");
                const auto new_id = change.after ? QString::fromStdString(change.after->snapshot_id)
                                                 : ui::text("graphics_workbench.new_id");
                descriptions << QStringLiteral("• %1：%2 → %3").arg(label, old_id, new_id);
                details << role;
                for (const auto &value : {change.before, change.after})
                    details << (value
                                    ? ui::text("graphics_workbench.label_45")
                                          .arg(
                                              QString::fromStdString(value->snapshot_id),
                                              QString::fromStdString(value->interpretation_version),
                                              QString::fromStdString(value->content_sha256),
                                              QString::fromStdString(value->configuration_sha256))
                                    : ui::text("graphics_workbench.label_44"));
            }
            QMessageBox message(
                QMessageBox::Question, ui::text("graphics_workbench.label_46"),
                ui::text("graphics_workbench.label_47")
                    .arg(descriptions.join('\n'),
                         extrapolation_description(next_data->data_info(), data_first, data_last)),
                QMessageBox::Yes | QMessageBox::Cancel, this);
            message.setObjectName("dataChangeDialog");
            message.button(QMessageBox::Yes)->setText(ui::text("graphics_workbench.label_48"));
            message.setDefaultButton(QMessageBox::Cancel);
            message.setDetailedText(details.join('\n'));
            if (message.exec() != QMessageBox::Yes) {
                last_project_outcome_ = "cancelled";
                return false;
            }
        }
        const auto recorded_data = project.data_lock();
        project.force_data.clear();
        for (const auto &file : gravity_files)
            project.force_data.push_back(runtime::gravity_reference(file.selection, file.info));
        for (const auto &file : ephemeris_files)
            project.force_data.push_back(runtime::ephemeris_reference(file.selection, file.info));
        for (const auto &file : weather_files)
            project.force_data.push_back(
                runtime::space_weather_reference(file.selection, file.info));
        for (const auto &file : ocean_files)
            project.force_data.push_back(runtime::ocean_tide_reference(file.selection, file.info));
        if (settings) {
            project.earth_orientation = next_data->data_info();
            project = project.with_settings(settings->name, first, last);
        }
        if (display_cancellation_)
            display_cancellation_->store(true);
        if (!editing && insert_dialog_)
            insert_dialog_->hide();
        project_edit_ = editing;
        project_settings_edit_ = settings.has_value();
        project_orbit_edit_ =
            edited_object && std::any_of(project.satellites.begin(), project.satellites.end(),
                                         [&](const auto &satellite) {
                                             return satellite.orbit.object_id == *edited_object;
                                         });
        pending_project_selection_ = selection;
        pending_station_apply_.reset();
        pending_sensor_apply_.reset();
        pending_project_sensor_selection_.reset();
        if (editing && selection == selected_marker_index())
            if (const auto *sensor = selected_sensor())
                pending_project_sensor_selection_ = sensor->object_id;
        pending_project_path_ = editing ? project_path_ : QFileInfo(path).absoluteFilePath();
        if (!cancel_project_button_) {
            cancel_project_button_ =
                new QPushButton(ui::text("graphics_workbench.label_49"), statusBar());
            statusBar()->addPermanentWidget(cancel_project_button_);
            connect(cancel_project_button_, &QPushButton::clicked, this, [this] {
                if (project_cancellation_)
                    project_cancellation_->store(true);
            });
        }
        cancel_project_button_->setText(
            editing ? ui::text("graphics_workbench.cancel_project_button.set_text")
                    : ui::text("graphics_workbench.label_49"));
        project_cancellation_ = std::make_shared<std::atomic_bool>(false);
        last_project_outcome_.clear();
        project_recorded_data_ = recorded_data;
        // Capture exactly the data the user checked/approved, even if a download
        // completes while the confirmation dialog is open.
        std::vector<EopCatalog::Provider> installed_data{next_data};
        const auto retained = editing ? scenario_ : std::nullopt;
        const auto previous_project =
            editing ? std::optional{project_snapshot(project_path_)} : std::nullopt;
        const auto calculation_time = time_for(next_data->data_info().time_data);
        const auto sessions =
            editing ? orbit_sessions_->fork() : std::make_shared<runtime::OrbitSessionCache>();
        project_progress_ = std::make_shared<ProjectProgress>();
        project_progress_->count = project.satellites.size();
        if (apply_timing_) {
            apply_timing_->route = "project";
            apply_timing_->asynchronous = true;
            apply_timing_->requested();
        }
        project_candidate_ = std::async(
            std::launch::async,
            [this, project = std::move(project),
             source = std::filesystem::path(pending_project_path_.toStdWString()), installed_data,
             retained, previous_project, edited_object, calculation_time, sessions, gravity_files,
             ephemeris_files, weather_files, ocean_files, retained_gravity = gravity_,
             retained_ephemerides = ephemerides_, retained_weather = weather_,
             retained_ocean_tides = ocean_tides_, cancel = project_cancellation_,
             progress = project_progress_, warmed = startup_resources_,
             timing = apply_timing_]() mutable {
                const ApplyWorkerTiming worker_timing(timing);
                StageTiming resource_timing(timing ? &timing->resources_ms : nullptr);
                const auto checkpoint = [cancel] {
                    if (*cancel)
                        throw OrbitCancelled{};
                };
                const auto info = [](const auto &model) -> const auto & { return model.info(); };
                const auto gravity = prepare_resources(
                    gravity_files, retained_gravity, runtime::gravity_reference, info,
                    [&](const auto &file) {
                        return runtime::load_gravity(file.file, file.info.source_url, checkpoint);
                    },
                    checkpoint, warmed.gravity);
                progress->phase = ProjectProgress::Phase::ocean;
                const auto ocean_tides = prepare_resources(
                    ocean_files, retained_ocean_tides, runtime::ocean_tide_reference, info,
                    [&](const auto &file) {
                        return runtime::load_ocean_tides(file.file, file.info.source_url,
                                                         checkpoint);
                    },
                    checkpoint);
                progress->phase = ProjectProgress::Phase::weather;
                const auto weather = prepare_resources(
                    weather_files, retained_weather, runtime::space_weather_reference, info,
                    [&](const auto &file) {
                        return runtime::load_space_weather(file.file, file.info.source_url,
                                                           checkpoint);
                    },
                    checkpoint);
                progress->phase = ProjectProgress::Phase::ephemeris;
                const auto ephemerides = prepare_resources(
                    ephemeris_files, retained_ephemerides, runtime::ephemeris_reference,
                    [](const auto &model) -> const auto & { return model.data_info(); },
                    [&](const auto &file) {
                        return runtime::make_third_body_ephemeris(runtime::load_spice_snapshot(
                            file.manifest,
                            file.manifest.parent_path().parent_path().parent_path() / "runtime"));
                    },
                    checkpoint, warmed.ephemerides);
                progress->phase = ProjectProgress::Phase::orbits;
                resource_timing.stop();
                return prepare_project_scene(
                    std::move(project), source, frames_, installed_data, *cancel, retained,
                    edited_object, calculation_time, gravity, ephemerides, weather, ocean_tides,
                    previous_project, sessions,
                    [progress](std::size_t index) { progress->index = index; },
                    timing ? &timing->preparation : nullptr);
            });
        update_actions();
        statusBar()->showMessage(editing ? ui::text("graphics_workbench.label_50")
                                         : ui::text("graphics_workbench.label_51"));
        return true;
    }
    void poll_project() {
        if (!project_candidate_.valid())
            return;
        if (project_candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            QString status;
            if (project_cancellation_->load()) {
                status = ui::text("graphics_workbench.preparation.cancelling");
            } else {
                switch (project_progress_->phase.load()) {
                case ProjectProgress::Phase::gravity:
                    status = ui::text("graphics_workbench.preparation.gravity");
                    break;
                case ProjectProgress::Phase::ocean:
                    status = ui::text("graphics_workbench.preparation.ocean");
                    break;
                case ProjectProgress::Phase::weather:
                    status = ui::text("graphics_workbench.preparation.weather");
                    break;
                case ProjectProgress::Phase::ephemeris:
                    status = ui::text("graphics_workbench.preparation.ephemeris");
                    break;
                case ProjectProgress::Phase::orbits:
                    status = project_progress_->count
                                 ? ui::text("graphics_workbench.preparation.orbits")
                                       .arg(project_progress_->index.load() + 1)
                                       .arg(project_progress_->count)
                                 : ui::text("graphics_workbench.preparation.scene");
                    break;
                }
            }
            statusBar()->showMessage(status);
            return;
        }
        if (apply_timing_)
            apply_timing_->ready();
        try {
            const StageTiming publish_timing(apply_timing_ ? &apply_timing_->publish_ms : nullptr);
            auto candidate = project_candidate_.get();
            if (project_cancellation_->load())
                throw OrbitCancelled{};
            // Catalog reads can fail: finish cache preparation before publishing
            // any part of the new scene. Moving the prepared maps cannot reload data.
            auto next_startup_resources = startup_resources_;
            next_startup_resources.remember(candidate->gravity, candidate->ephemerides);
            next_startup_resources.reconcile(eop_catalog_->data().gravity_models(),
                                             eop_catalog_->data().ephemerides());
            const auto current =
                project_edit_
                    ? core::Epoch{std::clamp(
                          (!playing_ && precise_epoch_ ? *precise_epoch_ : snapshot_.epoch)
                              .tai_nanoseconds(),
                          candidate->project.first.tai_nanoseconds(),
                          candidate->project.last.tai_nanoseconds())}
                    : candidate->project.current;
            const bool root_expanded = !project_edit_ || scenario_item_->isExpanded();
            std::unordered_map<std::string, bool> expanded;
            if (project_edit_)
                for (const auto &satellite : satellites_)
                    if (satellite.item->childCount() > 0)
                        expanded.emplace(satellite.orbit->input.object_id,
                                         satellite.item->isExpanded());
            std::vector<Satellite> satellites;
            std::vector<Station> stations;
            auto root = std::make_unique<QTreeWidgetItem>(
                QStringList{QString::fromStdString(candidate->project.name)});
            root->setIcon(0, workbench_icon(WorkbenchIcon::earth, theme::color(theme::accent)));
            auto tracks = vsg::Group::create(), models = vsg::Group::create();
            for (std::size_t i = 0; i < candidate->orbits.size(); ++i) {
                Satellite satellite;
                const auto &config = candidate->project.satellites[i];
                satellite.orbit = candidate->orbits[i];
                satellite.display_name = config.display_name;
                satellite.display = config.display;
                satellite.model_kind = config.model_kind;
                satellite.model_diameter_km = config.model_diameter_m / 1000;
                satellite.model_path = QString::fromStdString(config.model_path);
                satellite.model = candidate->models[i];
                if (satellite.model)
                    satellite.model_host->addChild(satellite.model);
                models->addChild(satellite.model_host);
                satellite.fixed_track =
                    std::make_shared<render_vsg::TrackLayer>(satellite.orbit->track.nodes());
                satellite.inertial_track = std::make_shared<render_vsg::TrackLayer>(
                    satellite.orbit->inertial_track.nodes());
                satellite.tracks = vsg::Switch::create();
                satellite.tracks->addChild(true, satellite.inertial_track->node());
                satellite.tracks->addChild(true, satellite.fixed_track->node());
                tracks->addChild(satellite.tracks);
                satellite.item = new QTreeWidgetItem(root.get(), {satellite.name()});
                satellite.item->setToolTip(
                    0, ui::text("common.object_id_hint")
                           .arg(QString::fromStdString(config.orbit.object_id)));
                satellite.item->setIcon(0,
                                        workbench_icon(WorkbenchIcon::satellite,
                                                       QColor::fromRgb(config.display.line_color)));
                satellites.push_back(std::move(satellite));
            }
            for (const auto &sensor : candidate->project.sensors) {
                auto parent = std::ranges::find_if(satellites, [&](const auto &sat) {
                    return sat.orbit->input.object_id == sensor.parent_satellite_id;
                });
                auto *item =
                    new QTreeWidgetItem(parent->item, {QString::fromStdString(sensor.name())});
                item->setData(0, Qt::UserRole + 17, QString::fromStdString(sensor.object_id));
                item->setIcon(0, workbench_icon(WorkbenchIcon::focus, theme::color(theme::sakura)));
                const std::array<const char *, 4> pattern_names{
                    "sensor.geometry.simple", "sensor.geometry.complex",
                    "sensor.geometry.rectangular", "sensor.geometry.custom"};
                item->setToolTip(0, ui::text("sensor.geometry.tooltip")
                                        .arg(ui::text(pattern_names.at(
                                            static_cast<std::size_t>(sensor.pattern.type))))
                                        .arg(QString::fromStdString(sensor.object_id)));
            }
            for (const auto &config : candidate->project.ground_stations) {
                Station station{config};
                station.item = new QTreeWidgetItem(root.get(), {station.name()});
                station.item->setIcon(
                    0, workbench_icon(WorkbenchIcon::ground_station, theme::color(theme::success)));
                station.item->setToolTip(0, ui::text("graphics_workbench.item.set_tool_tip_2")
                                                .arg(QString::fromStdString(config.object_id)));
                stations.push_back(std::move(station));
            }
            const auto previous_tracks = orbit_group_->children,
                       previous_models = model_group_->children;
            begin_change();
            try {
                orbit_group_->children = tracks->children;
                model_group_->children = models->children;
                rebuild();
            } catch (...) {
                orbit_group_->children = previous_tracks;
                model_group_->children = previous_models;
                rebuild();
                throw;
            }
            if (properties_dialog_ && !project_edit_)
                properties_dialog_->close();
            if (scenario_properties_ && !project_edit_)
                scenario_properties_->close();
            satellites_ = std::move(satellites);
            stations_ = std::move(stations);
            sensors_ = candidate->project.sensors;
            clear_access_display();
            if (sensor_editor_ && !project_edit_)
                sensor_editor_->close();
            if (station_editor_ && !project_edit_)
                station_editor_->close();
            scenario_ = std::move(candidate->scenario);
            orbit_sessions_ = std::move(candidate->sessions);
            eop_ = candidate->eop;
            gravity_ = candidate->gravity;
            ephemerides_ = candidate->ephemerides;
            weather_ = candidate->weather;
            ocean_tides_ = candidate->ocean_tides;
            startup_resources_ = std::move(next_startup_resources);
            if (!project_edit_)
                close_reports();
            reports_ = candidate->project.reports;
            standalone_time_data_ = candidate->project.time_data;
            analysis_record_ = candidate->project.analysis_record;
            time_label_epoch_.reset();
            sun_epoch_.reset();
            if (!project_edit_) {
                editor_target_.reset();
                orbit_editor_bound_ = station_editor_bound_ = sensor_editor_bound_ =
                    scenario_editor_bound_ = false;
            }
            pending_orbit_target_.reset();
            {
                const QSignalBlocker block(objects_);
                objects_->clear();
                scenario_item_ = root.release();
                objects_->addTopLevelItem(scenario_item_);
                // Expansion and visibility only take effect after items belong to the tree.
                scenario_item_->setExpanded(root_expanded);
                for (const auto &satellite : satellites_) {
                    const auto previous = expanded.find(satellite.orbit->input.object_id);
                    satellite.item->setExpanded(satellite.item->childCount() > 0 &&
                                                (previous == expanded.end() || previous->second));
                }
                filter_objects();
            }
            orbit_ = satellites_.empty() ? nullptr : satellites_.front().orbit;
            object_item_ = satellites_.empty() ? nullptr : satellites_.front().item;
            model_node_ = satellites_.empty() ? vsg::ref_ptr<vsg::MatrixTransform>{}
                                              : satellites_.front().model;
            model_kind_ = satellites_.empty() ? 0 : satellites_.front().model_kind;
            model_diameter_km_ = satellites_.empty() ? 1000 : satellites_.front().model_diameter_km;
            ground_map_->publish_tracks(std::move(candidate->ground_tracks));
            ground_map_->set_ground_stations(candidate->project.ground_stations);
            if (!project_edit_)
                ground_map_->reset_view();
            set_workspace_name(QString::fromStdString(scenario_->name()));
            set_selection(project_edit_ ? pending_project_selection_ : std::nullopt);
            if (pending_project_sensor_selection_)
                for (const auto &satellite : satellites_)
                    for (int i = 0; i < satellite.item->childCount(); ++i) {
                        auto *item = satellite.item->child(i);
                        if (item->data(0, Qt::UserRole + 17).toString().toStdString() !=
                            *pending_project_sensor_selection_)
                            continue;
                        objects_->setCurrentItem(item);
                        if (!satellite.item->isHidden() && !item->isHidden()) {
                            scenario_item_->setExpanded(true);
                            satellite.item->setExpanded(true);
                            objects_->scrollToItem(item);
                        }
                    }
            if (!project_edit_)
                set_playing(false);
            seek_epoch(current);
            snapshot_ = sample_frame(current);
            satellite_state_dirty_ = true;
            update_satellite();
            selection_->update({snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}});
            for (auto &slot : slots_)
                slot->pick_candidates.clear();
            workspace_pages_->setCurrentIndex(0);
            if (!project_edit_)
                workspace_->select_view(0);
            object_dock_->show();
            if (!project_edit_)
                fit_pending_ = true;
            show_timeline_status();
            if (timeline_ && !project_edit_)
                timeline_->reset_view();
            project_path_ = pending_project_path_;
            if (project_edit_)
                refresh_property_bindings();
            mark_project_dirty(project_edit_ ||
                               candidate->project.data_lock() != project_recorded_data_);
            last_project_outcome_ = "opened";
            statusBar()->showMessage(
                project_edit_ ? ui::text("graphics_workbench.label_52")
                              : ui::text("graphics_workbench.label_53").arg(project_path_),
                6000);
        } catch (const OrbitCancelled &) {
            last_project_outcome_ = "cancelled";
            statusBar()->showMessage(ui::text("graphics_workbench.label_54"), 5000);
        } catch (const std::exception &error) {
            last_project_outcome_ = "rejected";
            const auto title = project_edit_ ? ui::text("graphics_workbench.title")
                                             : ui::text("graphics_workbench.title_2");
            statusBar()->showMessage(title + QStringLiteral("：") +
                                     QString::fromUtf8(error.what()));
            // Stop the measurement before waiting for the error dialog to be dismissed.
            finish_apply_timing(last_project_outcome_);
            if (smoke_dir_.isEmpty())
                QMessageBox::warning(this, title, QString::fromUtf8(error.what()));
        } catch (const vsg::Exception &error) {
            last_project_outcome_ = "rejected";
            statusBar()->showMessage(
                ui::text("graphics_workbench.label_55").arg(QString::fromStdString(error.message)));
        }
        finish_apply_timing(last_project_outcome_ == "opened" ? "applied" : last_project_outcome_);
        if (project_orbit_edit_) {
            editor_->set_busy(false);
            last_orbit_outcome_ =
                last_project_outcome_ == "opened" ? "applied" : last_project_outcome_;
            if (last_project_outcome_ == "opened")
                editor_->show_result(ui::text("graphics_workbench.orbit_applied"));
            else if (last_project_outcome_ == "cancelled")
                editor_->show_result(ui::text("graphics_workbench.label_54"));
            else
                editor_->show_result(ui::text("graphics_workbench.label_56"), true);
            if (insert_defaults_pending_ && last_orbit_outcome_ == "rejected")
                show_properties_dialog();
            insert_defaults_pending_ = false;
        }
        if (project_settings_edit_ && scenario_properties_)
            if (auto *result =
                    scenario_properties_->findChild<QLabel *>("scenarioSettingsResult")) {
                result->setText(last_project_outcome_ == "opened"
                                    ? ui::text("graphics_workbench.scenario_applied")
                                : last_project_outcome_ == "cancelled"
                                    ? ui::text("graphics_workbench.result.set_text")
                                    : ui::text("graphics_workbench.label_57"));
                result->show();
            }
        if (station_editor_ && pending_station_apply_ && last_project_outcome_ != "opened")
            station_editor_->show_result(last_project_outcome_ == "cancelled"
                                             ? ui::text("graphics_workbench.label_54")
                                             : ui::text("graphics_workbench.label_58"));
        if (pending_report_input_) {
            auto done = std::move(pending_report_input_);
            pending_report_input_ = {};
            if (last_project_outcome_ == "opened")
                done(report_input(), {});
            else
                done({}, ui::text("graphics_workbench.label_59"));
        }
        finish_insertion(last_project_outcome_ == "opened", last_project_outcome_ == "cancelled");
        update_actions();
    }
    void rename_object(const std::string &id, const std::string &name) {
        if (name.empty())
            throw std::invalid_argument(ui::utf8("common.object_name_required"));
        // Validate the complete candidate before touching the live scene. Identity,
        // sessions, graphics nodes and report queries remain bound to the same ID.
        auto candidate = project_snapshot(project_path_);
        bool changed = false, found = false;
        const auto rename = [&](auto &object) {
            found = true;
            changed = object.name() != name;
            object.display_name = name;
        };
        for (auto &satellite : candidate.satellites)
            if (satellite.orbit.object_id == id)
                rename(satellite);
        for (auto &station : candidate.ground_stations)
            if (station.object_id == id)
                rename(station);
        for (auto &sensor : candidate.sensors)
            if (sensor.object_id == id)
                rename(sensor);
        if (!found)
            throw std::invalid_argument("Object not found: " + id);
        application::validate_project(candidate);
        if (!changed)
            return;
        for (std::size_t i = 0; i < satellites_.size(); ++i) {
            auto &satellite = satellites_[i];
            if (satellite.orbit->input.object_id != id)
                continue;
            satellite.display_name = name;
            satellite.item->setText(0, satellite.name());
            ground_map_->set_name(i, satellite.name());
        }
        for (std::size_t i = 0; i < stations_.size(); ++i) {
            auto &station = stations_[i];
            if (station.config.object_id != id)
                continue;
            station.config.display_name = name;
            station.item->setText(0, station.name());
            ground_map_->set_station_name(i, station.config.object_id, station.name());
        }
        for (auto &sensor : sensors_)
            if (sensor.object_id == id) {
                sensor.display_name = name;
                for (auto &satellite : satellites_)
                    for (int i = 0; i < satellite.item->childCount(); ++i) {
                        auto *item = satellite.item->child(i);
                        if (item->data(0, Qt::UserRole + 17).toString().toStdString() == id)
                            item->setText(0, QString::fromStdString(name));
                    }
            }
        filter_objects();
        // External renames update only the name baseline, preserving other drafts,
        // units, page selection and any independently edited name.
        if (orbit_editor_bound_ && editor_target_) {
            const auto &satellite = satellites_[*editor_target_];
            editor_->sync_name(satellite.name().toStdString());
            properties_dialog_->setWindowTitle(
                satellite.name() +
                ui::text("graphics_workbench.properties_dialog.set_window_title_2"));
        }
        if (station_editor_bound_ && station_editor_target_)
            for (const auto &station : stations_)
                if (station.config.object_id == *station_editor_target_) {
                    station_editor_->sync_name(station.config.name());
                    station_editor_->setWindowTitle(
                        station.name() +
                        ui::text("graphics_workbench.station_editor.set_window_title"));
                }
        if (sensor_editor_bound_) {
            for (const auto &sensor : sensors_)
                if (sensor.object_id == sensor_editor_->bound_object_id()) {
                    sensor_editor_->sync_name(sensor.name());
                    sensor_editor_->setWindowTitle(
                        QString::fromStdString(sensor.name()) +
                        ui::text("graphics_workbench.sensor_editor.set_window_title"));
                }
            sensor_editor_->update_project(candidate);
        }
        if (access_ids_.size() == 2)
            timeline_->set_access_label(object_pair_name(access_ids_));
        show_timeline_status();
        mark_project_dirty();
    }
    void rename_selected_to(const QString &name) {
        if (busy())
            return;
        if (const auto *sensor = selected_sensor())
            rename_object(sensor->object_id, name.trimmed().toStdString());
        else if (selected_station_)
            rename_object(stations_[*selected_station_].config.object_id,
                          name.trimmed().toStdString());
        else if (selected_)
            rename_object(satellites_[*selected_].orbit->input.object_id,
                          name.trimmed().toStdString());
    }
    void rename_selected() {
        if (busy() || !selected_marker_index())
            return;
        bool accepted = false;
        const auto name = QInputDialog::getText(this, ui::text("graphics_workbench.name"),
                                                ui::text("common.object_name"), QLineEdit::Normal,
                                                selected_name(), &accepted);
        if (accepted) {
            try {
                rename_selected_to(name);
            } catch (const std::exception &error) {
                QMessageBox::warning(this, ui::text("graphics_workbench.label_60"),
                                     QString::fromUtf8(error.what()));
            }
        }
    }
    void duplicate_selected() {
        if (busy() || !selected_marker_index())
            return;
        try {
            if (selected_sensor()) {
                auto p = project_snapshot(project_path_);
                auto s = *selected_sensor();
                s.display_name = s.name() + ui::utf8("graphics_workbench.s");
                s.object_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                const auto id = s.object_id;
                p.sensors.push_back(std::move(s));
                if (start_project(std::move(p), project_path_, true, selected_marker_index()))
                    pending_project_sensor_selection_ = id;
                return;
            }
            const auto project = project_snapshot(project_path_);
            if (selected_station_) {
                const auto &source = project.ground_stations.at(*selected_station_);
                std::string id;
                unsigned suffix = 1;
                do {
                    id = source.object_id + "-copy-" + std::to_string(suffix++);
                } while (object_id_exists(id));
                start_project(
                    project.with_ground_station_copy(
                        source.object_id, id, source.name() + ui::utf8("graphics_workbench.s")),
                    project_path_, true, satellites_.size() + stations_.size());
                return;
            }
            const auto &source = project.satellites.at(*selected_);
            std::string id;
            unsigned suffix = 1;
            do {
                id = source.orbit.object_id + "-copy-" + std::to_string(suffix++);
            } while (object_id_exists(id));
            start_project(
                project.with_satellite_copy(source.orbit.object_id, id,
                                            source.name() + ui::utf8("graphics_workbench.s")),
                project_path_, true, satellites_.size());
        } catch (const std::exception &error) {
            QMessageBox::warning(this, ui::text("graphics_workbench.label_61"),
                                 QString::fromUtf8(error.what()));
        }
    }
    void delete_selected() {
        if (busy() || !selected_marker_index())
            return;
        const auto index = *selected_marker_index();
        if (QMessageBox::question(this, ui::text("graphics_workbench.index"),
                                  ui::text("graphics_workbench.index_2").arg(selected_name()),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        try {
            const auto current = project_snapshot(project_path_);
            if (selected_sensor()) {
                auto p = current;
                const auto id = selected_sensor()->object_id;
                std::erase_if(p.sensors, [&](const auto &s) { return s.object_id == id; });
                start_project(std::move(p), project_path_, true, selected_marker_index());
                return;
            }
            auto project =
                selected_station_
                    ? current.without_ground_station(
                          stations_.at(*selected_station_).config.object_id)
                    : current.without_satellite(satellites_.at(index).orbit->input.object_id);
            const auto count = project.satellites.size() + project.ground_stations.size();
            const auto selection = count == 0
                                       ? std::optional<std::size_t>{}
                                       : std::optional<std::size_t>{std::min(index, count - 1)};
            start_project(std::move(project), project_path_, true, selection);
        } catch (const std::exception &error) {
            QMessageBox::warning(this, ui::text("graphics_workbench.label_62"),
                                 QString::fromUtf8(error.what()));
        }
    }
    void manage_eop_data() {
        if (busy())
            return;
        show_data_center(this, *eop_catalog_);
    }
    void create_scenario() {
        if (busy())
            return;
        const auto &coverage =
            eop_catalog_->providers().at(eop_catalog_->preferred_index())->data_info();
        try {
            auto next = new_scenario_dialog(
                this, coverage.first_epoch, coverage.last_epoch,
                [this](core::Epoch first, core::Epoch last) {
                    (void)data_for_interval(first, last);
                },
                eop_catalog_->time_provider());
            if (next && confirm_replace_scenario())
                activate_scenario(std::move(*next));
        } catch (const core::TimeDataRangeError &) {
            QMessageBox::warning(this, ui::text("graphics_workbench.label_63"),
                                 ui::text("graphics_workbench.label_64"));
        }
    }
    std::shared_ptr<const core::IEarthOrientationProvider>
    data_for_interval(core::Epoch first, core::Epoch last) const {
        return eop_catalog_->for_interval(first, last);
    }
    std::shared_ptr<const core::ITimeProvider> time_for(const core::TimeDataInfo &info) const {
        if (active_time_ && active_time_->data_info().sha256 == info.sha256)
            return active_time_;
        if (eop_catalog_ && eop_catalog_->time_provider()->data_info().sha256 == info.sha256)
            return eop_catalog_->time_provider();
        auto builtin = runtime::make_builtin_time_provider();
        if (builtin->data_info().sha256 == info.sha256)
            return builtin;
        return runtime::load_time_snapshot(eop_catalog_->data().time_file(info.sha256));
    }
    std::shared_ptr<const core::ITimeProvider> active_time() const {
        if (!eop_)
            return eop_catalog_ ? eop_catalog_->time_provider()
                                : runtime::make_builtin_time_provider();
        active_time_ = time_for(eop_->data_info().time_data);
        return active_time_;
    }
    QString calendar_text(core::Epoch epoch, CalendarScale scale, bool date = true) const {
        return desktop::calendar_text(epoch, scale, date, active_time().get());
    }
    void activate_scenario(application::Scenario next) {
        if (busy())
            throw std::logic_error(
                "Finish the pending object update before replacing the scenario");
        const auto next_eop = data_for_interval(next.first(), next.last());
        if (display_cancellation_)
            display_cancellation_->store(true);
        if (!next.objects().empty())
            throw std::invalid_argument("Create an empty scenario before inserting objects");
        const auto old_tracks = orbit_group_->children;
        const auto old_models = model_group_->children;
        begin_change();
        try {
            orbit_group_->children.clear();
            model_group_->children.clear();
            rebuild();
        } catch (...) {
            orbit_group_->children = old_tracks;
            model_group_->children = old_models;
            rebuild();
            throw;
        }
        if (properties_dialog_)
            properties_dialog_->close();
        if (insert_dialog_)
            insert_dialog_->hide();
        satellites_.clear();
        stations_.clear();
        sensors_.clear();
        clear_access_display();
        if (sensor_editor_)
            sensor_editor_->close();
        close_reports();
        if (station_editor_)
            station_editor_->close();
        if (ground_map_) {
            ground_map_->set_orbits({});
            ground_map_->set_ground_stations({});
            ground_map_->reset_view();
        }
        orbit_.reset();
        model_node_ = {};
        editor_target_.reset();
        orbit_editor_bound_ = station_editor_bound_ = sensor_editor_bound_ =
            scenario_editor_bound_ = false;
        model_kind_ = 0;
        model_diameter_km_ = 1000;
        const QSignalBlocker block(objects_);
        objects_->clear();
        object_item_ = nullptr;
        scenario_ = std::move(next);
        orbit_sessions_ = std::make_shared<runtime::OrbitSessionCache>();
        pending_orbit_sessions_.reset();
        eop_ = next_eop;
        gravity_.clear();
        ephemerides_.clear();
        weather_.clear();
        ocean_tides_.clear();
        time_label_epoch_.reset();
        scenario_ = scenario_->with_data_lock(application::eop_data_lock(eop_->data_info()));
        scenario_item_ = new QTreeWidgetItem(objects_, {QString::fromStdString(scenario_->name())});
        scenario_item_->setIcon(0,
                                workbench_icon(WorkbenchIcon::earth, theme::color(theme::accent)));
        scenario_item_->setToolTip(
            0, ui::text("graphics_workbench.scenario_item.set_tool_tip")
                   .arg(display_tai(scenario_->first()), display_tai(scenario_->last())));
        scenario_item_->setExpanded(true);
        set_workspace_name(QString::fromStdString(scenario_->name()));
        setWindowTitle(QString::fromStdString(scenario_->name()) + ui::text("window.title_suffix"));
        project_path_.clear();
        standalone_time_data_.reset();
        analysis_record_.reset();
        mark_project_dirty();
        set_selection(std::nullopt);
        set_playing(false);
        seek_epoch(scenario_->first());
        snapshot_ = sample_frame(scenario_->first());
        update_satellite();
        selection_->update(snapshot_);
        for (auto &slot : slots_)
            slot->pick_candidates.clear();
        workspace_pages_->setCurrentIndex(0);
        if (scenario_properties_)
            scenario_properties_->close();
        workspace_->select_view(0);
        object_dock_->show();
        fit_pending_ = true;
        update_actions();
        show_orbit_status();
        if (timeline_)
            timeline_->reset_view();
    }
    void open_example() {
        if (busy() || !confirm_replace_scenario())
            return;
        activate_scenario(application::Scenario("TwoBodyExample", core::Epoch{0},
                                                core::Epoch{7'200'000'000'000}));
        begin_insert_satellite();
        editor_->submit();
        properties_dialog_->close();
    }
    void show_properties_dialog() { workspace_->open(properties_dialog_, false); }
    InsertObjectDialog *insertion_dialog() {
        if (!insert_dialog_) {
            insert_dialog_ = new InsertObjectDialog(
                QString::fromStdWString(
                    (eop_catalog_->store() / "desktop-preferences.ini").wstring()),
                this);
            insert_dialog_->insert = [this](InsertObjectRequest request) {
                insert_object(request);
            };
        }
        return insert_dialog_;
    }
    void show_insert_objects() {
        if (!scenario_ || !editor_ || busy())
            return;
        auto *dialog = insertion_dialog();
        dialog->set_scenario(QString::fromStdString(scenario_->name()));
        dialog->set_parents(
            project_object_choices(project_snapshot(project_path_), {ObjectKind::satellite}),
            selected_ ? satellites_[*selected_].orbit->input.object_id : std::string{});
        const auto selected = dialog->selection();
        dialog->select_type(selected ? selected->type : InsertObjectType::satellite);
        dialog->set_busy(false);
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    }
    void insert_object(InsertObjectRequest request) {
        if (!scenario_ || !editor_ || busy())
            return;
        if (request.type == InsertObjectType::sensor &&
            std::none_of(satellites_.begin(), satellites_.end(), [&](const auto &satellite) {
                return satellite.orbit->input.object_id == request.parent_id;
            })) {
            show_insert_objects();
            insertion_dialog()->select_type(InsertObjectType::sensor);
            insertion_dialog()->show_result(ui::text("insert_object_dialog.parent_missing"));
            return;
        }
        if (request.method != InsertObjectMethod::defaults)
            insertion_dialog()->hide();
        std::optional<OrbitInput> seed;
        if (request.type == InsertObjectType::satellite) {
            if (request.method == InsertObjectMethod::orbit_wizard) {
                OrbitWizardDialog wizard(scenario_->first(), this);
                wizard.set_preview_context(ground_map_->imagery(), frames_, eop_);
                if (wizard.exec() != QDialog::Accepted)
                    return;
                seed = wizard.input();
            } else if (request.method == InsertObjectMethod::tle_file) {
                const auto path = QFileDialog::getOpenFileName(this, ui::text("orbit_editor.path"),
                                                               {}, ui::text("orbit_editor.path_2"));
                if (path.isEmpty())
                    return;
                try {
                    seed = OrbitInput{};
                    seed->propagator = OrbitPropagator::sgp4;
                    seed->tle = read_single_tle_file(path);
                    const auto elements = core::parse_tle(seed->tle);
                    (void)eop_catalog_->time_provider()->from_utc(elements.epoch_utc);
                } catch (const std::exception &error) {
                    show_insert_objects();
                    insertion_dialog()->show_result(QString::fromUtf8(error.what()));
                    return;
                }
            }
        }
        try {
            switch (request.type) {
            case InsertObjectType::satellite:
                begin_insert_satellite(request.method, seed);
                break;
            case InsertObjectType::ground_station:
                begin_insert_station(request.method);
                break;
            case InsertObjectType::sensor:
                if (request.parent_id.empty())
                    return;
                edit_sensor(true, request.method == InsertObjectMethod::defaults,
                            request.parent_id);
                break;
            }
        } catch (const std::exception &error) {
            show_insert_objects();
            insertion_dialog()->show_result(QString::fromUtf8(error.what()));
            return;
        }
        if (request.method == InsertObjectMethod::defaults) {
            insertion_pending_ = busy();
            insertion_dialog()->show_result(ui::text(insertion_pending_
                                                         ? "insert_object_dialog.pending"
                                                         : "insert_object_dialog.not_inserted"));
            update_actions();
        }
    }
    void finish_insertion(bool success, bool cancelled) {
        if (!insertion_pending_)
            return;
        insertion_pending_ = false;
        insertion_dialog()->show_result(ui::text(success     ? "insert_object_dialog.completed"
                                                 : cancelled ? "insert_object_dialog.cancelled"
                                                             : "insert_object_dialog.failed"));
    }
    void begin_insert_satellite(InsertObjectMethod method = InsertObjectMethod::define_properties,
                                const std::optional<OrbitInput> &seed = {}) {
        if (!scenario_ || !editor_ || busy())
            return;
        if (method == InsertObjectMethod::define_properties && orbit_editor_bound_ &&
            !editor_target_) {
            show_properties_dialog();
            return;
        }
        if (orbit_editor_bound_ && editor_->has_unapplied_changes() &&
            !confirm_discard_property(properties_dialog_))
            return;
        if (satellites_.size() + stations_.size() >= 10000) {
            QMessageBox::information(this, ui::text("graphics_workbench.label_65"),
                                     ui::text("graphics_workbench.label_66"));
            return;
        }
        OrbitInput input = seed.value_or(OrbitInput{});
        std::size_t suffix = satellites_.size() + 1;
        do {
            input.object_id = "Satellite" + std::to_string(suffix++);
        } while (object_id_exists(input.object_id));
        input.first = scenario_->first();
        input.last = scenario_->last();
        input.initial.epoch = scenario_->first();
        editor_target_.reset();
        editor_->set_time_provider(eop_catalog_->time_provider());
        editor_->set_input(input);
        editor_->set_name(input.object_id);
        if (method == InsertObjectMethod::orbit_wizard)
            editor_->set_state_mode(1);
        orbit_editor_bound_ = true;
        editor_->set_create_mode(true);
        editor_->set_appearance(0, {}, 1000);
        editor_->set_display({});
        editor_->show_appearance_result({});
        editor_->setToolTip({});
        editor_->show_result({});
        inspector_pages_->setCurrentIndex(0);
        properties_dialog_->setWindowTitle(
            ui::text("graphics_workbench.properties_dialog.set_window_title_3") +
            QString::fromStdString(scenario_->name()));
        insert_defaults_pending_ = method == InsertObjectMethod::defaults;
        if (insert_defaults_pending_) {
            properties_dialog_->close();
            start_orbit(std::move(input), editor_->object_name());
        } else {
            show_properties_dialog();
        }
    }
    bool configure_station_editor(const application::ProjectGroundStation &station, bool creating) {
        if (!station_editor_) {
            station_editor_ = new GroundStationEditor(this);
            station_editor_->apply = [this](application::ProjectGroundStation value) {
                if (busy() || !scenario_)
                    return;
                const auto id = value.object_id;
                auto project = project_snapshot(project_path_);
                auto index = stations_.size();
                std::optional<std::string> edited;
                if (station_editor_target_) {
                    const auto found = std::find_if(
                        project.ground_stations.begin(), project.ground_stations.end(),
                        [&](const auto &s) { return s.object_id == *station_editor_target_; });
                    if (found == project.ground_stations.end())
                        throw std::invalid_argument(ui::utf8("graphics_workbench.validation_10"));
                    index = static_cast<std::size_t>(found - project.ground_stations.begin());
                    if (same_station_input(*found, value)) {
                        station_editor_->show_result(ui::text("graphics_workbench.label_67"));
                        return;
                    }
                    if (found->location == value.location &&
                        found->minimum_elevation_rad == value.minimum_elevation_rad) {
                        *found = value;
                        application::validate_project(project);
                        rename_object(id, value.name());
                        stations_.at(index).config = value;
                        mark_project_dirty();
                        station_editor_->accept_station(value);
                        station_editor_->show_result(ui::text("graphics_workbench.label_31"));
                        return;
                    }
                    if (found->location != value.location)
                        edited = value.object_id;
                    *found = std::move(value);
                } else {
                    project.ground_stations.push_back(std::move(value));
                }
                application::validate_project(project);
                if (start_project(std::move(project), project_path_, true,
                                  satellites_.size() + index, edited)) {
                    pending_station_apply_ = id;
                    station_editor_->show_result(ui::text("graphics_workbench.label_68"));
                } else
                    station_editor_->show_result(ui::text("graphics_workbench.label_69"));
            };
        }
        if (!creating && station_editor_bound_ && station_editor_target_ == station.object_id)
            return true;
        if (station_editor_bound_ && station_editor_->has_unapplied_changes() &&
            !confirm_discard_property(station_editor_))
            return false;
        station_editor_target_ =
            creating ? std::nullopt : std::optional<std::string>{station.object_id};
        station_editor_->set_station(station, creating);
        station_editor_bound_ = true;
        return true;
    }
    void begin_insert_station(InsertObjectMethod method = InsertObjectMethod::define_properties) {
        if (!scenario_ || busy())
            return;
        if (method == InsertObjectMethod::define_properties && station_editor_bound_ &&
            !station_editor_target_) {
            workspace_->open(station_editor_, false);
            return;
        }
        if (satellites_.size() + stations_.size() >= 10000) {
            QMessageBox::information(this, ui::text("graphics_workbench.label_65"),
                                     ui::text("graphics_workbench.label_66"));
            return;
        }
        application::ProjectGroundStation station;
        auto suffix = stations_.size() + 1;
        do {
            station.object_id = "GroundStation" + std::to_string(suffix++);
        } while (object_id_exists(station.object_id));
        station.display_name = ui::utf8("graphics_workbench.label_70") + std::to_string(suffix - 1);
        if (!configure_station_editor(station, true))
            return;
        if (method == InsertObjectMethod::defaults) {
            station_editor_->close();
            station_editor_->apply(station);
        } else
            workspace_->open(station_editor_, false);
    }
    void set_playing(bool value) {
        playing_ = value;
        if (value)
            precise_epoch_.reset();
        if (play_action_) {
            play_action_->setText(value ? ui::text("graphics_workbench.play")
                                        : ui::text("graphics_workbench.play_2"));
            play_action_->setIcon(play_icon(value));
        }
        clock_.restart();
    }
    void seek_epoch(core::Epoch epoch) {
        if (!scenario_ || epoch.tai_nanoseconds() < scenario_->first().tai_nanoseconds() ||
            epoch.tai_nanoseconds() > scenario_->last().tai_nanoseconds())
            throw std::out_of_range("Requested display epoch lies outside the analysis interval");
        seconds_ = epoch.seconds_since(scenario_->first());
        precise_epoch_ = epoch;
        requested_time_ = true;
        const QSignalBlocker block(slider_);
        slider_->setValue(qRound(seconds_ / duration_seconds() * 10000));
        if (timeline_)
            timeline_->set_current_time(epoch.tai_nanoseconds() / 1'000'000, true);
        clock_.restart();
    }
    void choose_time() {
        if (!scenario_)
            return;
        const bool resume = playing_;
        set_playing(false);
        QDialog dialog(this);
        dialog.setObjectName("jumpTimeDialog");
        dialog.setWindowTitle(ui::text("graphics_workbench.label_71"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *time = new EpochEditor(nullptr, active_time());
        time->set_scale(display_scale_);
        time->set_range(scenario_->first(), scenario_->last());
        time->set_epoch(snapshot_.epoch);
        time->setMinimumWidth(380);
        layout->addWidget(time);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new QPushButton(ui::text("common.cancel"));
        auto *apply = new QPushButton(ui::text("graphics_workbench.apply"));
        apply->setObjectName("applyOrbit");
        buttons->addWidget(cancel);
        buttons->addWidget(apply);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
        connect(apply, &QPushButton::clicked, &dialog, [&dialog, time] {
            try {
                (void)time->epoch();
                dialog.accept();
            } catch (const std::exception &error) {
                QMessageBox::information(&dialog, ui::text("graphics_workbench.label_72"),
                                         QString::fromUtf8(error.what()));
            }
        });
        if (dialog.exec() == QDialog::Accepted) {
            const auto epoch = time->epoch();
            if (scenario_ && epoch.tai_nanoseconds() >= scenario_->first().tai_nanoseconds() &&
                epoch.tai_nanoseconds() <= scenario_->last().tai_nanoseconds())
                seek_epoch(epoch);
            else
                QMessageBox::information(this, ui::text("graphics_workbench.label_73"),
                                         ui::text("graphics_workbench.label_74"));
        } else
            set_playing(resume);
    }
    void open_properties() {
        if (selected_sensor() && !busy()) {
            edit_sensor(false);
            return;
        }
        if (selected_station_ && !busy()) {
            if (configure_station_editor(stations_.at(*selected_station_).config, false))
                workspace_->open(station_editor_, false);
            return;
        }
        if (!selected_ && scenario_ && objects_->currentItem() == scenario_item_) {
            open_scenario_properties();
            return;
        }
        if (!properties_dialog_ || !selected_ || busy())
            return;
        if (orbit_editor_bound_ && editor_target_ &&
            editor_->bound_object_id() == satellites_.at(*selected_).orbit->input.object_id) {
            show_properties_dialog();
            return;
        }
        if (orbit_editor_bound_ && editor_->has_unapplied_changes() &&
            !confirm_discard_property(properties_dialog_))
            return;
        editor_target_ = selected_;
        const auto &satellite = satellites_.at(*editor_target_);
        editor_->set_time_provider(eop_catalog_->time_provider());
        editor_->set_input(satellite.orbit->input);
        editor_->set_name(satellite.name().toStdString());
        orbit_editor_bound_ = true;
        editor_->set_create_mode(false);
        editor_->set_appearance(satellite.model_kind, satellite.model_path,
                                satellite.model_diameter_km);
        editor_->set_display(satellite.display);
        editor_->show_appearance_result({});
        inspector_pages_->setCurrentIndex(0);
        properties_dialog_->setWindowTitle(
            satellite.name() + ui::text("graphics_workbench.properties_dialog.set_window_title_2"));
        show_orbit_status();
        show_properties_dialog();
    }
    bool apply_scenario_settings(const ScenarioSettings &settings) {
        if (!scenario_ || busy())
            return false;
        if (settings.first != scenario_->first() || settings.last != scenario_->last())
            return start_project(project_snapshot(project_path_), project_path_, true,
                                 selected_marker_index(), std::nullopt, settings);
        const auto checked = project_snapshot(project_path_)
                                 .with_settings(settings.name, settings.first, settings.last);
        if (checked.name == scenario_->name())
            return false;
        // A rename keeps the current calculation, playback and display caches intact.
        scenario_ = scenario_->with_settings(checked.name, checked.first, checked.last);
        const auto name = QString::fromStdString(checked.name);
        scenario_item_->setText(0, name);
        set_workspace_name(name);
        if (scenario_properties_)
            scenario_properties_->setWindowTitle(
                name + ui::text("graphics_workbench.scenario_properties.set_window_title"));
        show_timeline_status();
        mark_project_dirty();
        return true;
    }
    void open_scenario_properties() {
        if (!scenario_ || busy())
            return;
        if (!scenario_properties_)
            scenario_properties_ = new QDialog(this);
        auto &dialog = *scenario_properties_;
        if (scenario_editor_bound_) {
            if (auto *count = dialog.findChild<QLabel *>("scenarioObjectCount"))
                count->setText(QString::number(satellites_.size() + stations_.size()));
            workspace_->open(&dialog);
            return;
        }
        if (auto *previous = dialog.layout()) {
            while (auto *item = previous->takeAt(0)) {
                if (item->widget())
                    delete item->widget();
                if (item->layout()) {
                    while (auto *field = item->layout()->takeAt(0)) {
                        delete field->widget();
                        delete field;
                    }
                }
                delete item;
            }
            delete previous;
        }
        dialog.setObjectName("scenarioProperties");
        scenario_editor_bound_ = true;
        dialog.setWindowTitle(QString::fromStdString(scenario_->name()) +
                              ui::text("graphics_workbench.scenario_properties.set_window_title"));
        dialog.setMinimumWidth(620);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(theme::space::page);
        layout->setSpacing(16);
        auto *title = new QLabel(ui::text("graphics_workbench.title_3"));
        title->setObjectName("panelTitle");
        layout->addWidget(title);
        auto *description = new QLabel(ui::text("graphics_workbench.description"));
        description->setObjectName("dialogDescription");
        description->setWordWrap(true);
        layout->addWidget(description);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *body = new QWidget;
        auto *body_layout = new QVBoxLayout(body);
        body_layout->setContentsMargins(0, 0, 8, 0);
        body_layout->setSpacing(0);
        scroll->setWidget(body);
        layout->addWidget(scroll, 1);
        // Editable settings first, then read-only facts, then the applied data record.
        const auto group = [](const QString &heading) {
            auto *frame = new FormGroup(heading);
            frame->setMaximumWidth(880);
            return std::pair{frame, frame->form()};
        };
        auto [section, form] = group(ui::text("graphics_workbench.label_75"));
        auto *name = new QLineEdit(QString::fromStdString(scenario_->name()));
        name->setObjectName("scenarioSettingsName");
        name->setMaximumWidth(380);
        form->addRow(ui::text("graphics_workbench.form.add_row"), name);
        auto *first = new EpochEditor(nullptr, eop_catalog_->time_provider());
        auto *last = new EpochEditor(nullptr, eop_catalog_->time_provider());
        first->setObjectName("scenarioSettingsStart");
        last->setObjectName("scenarioSettingsEnd");
        first->set_epoch(scenario_->first());
        last->set_epoch(scenario_->last());
        for (auto *field : {first, last})
            field->setMaximumWidth(380);
        form->addRow(ui::text("graphics_workbench.form.add_row_2"), first);
        form->addRow(ui::text("graphics_workbench.form.add_row_3"), last);
        section->add_help(ui::text("graphics_workbench.section.add_help"));
        auto [facts, facts_form] = group(ui::text("graphics_workbench.label_76"));
        facts_form->addRow(ui::text("graphics_workbench.facts_form.add_row"),
                           new QLabel(ui::text("graphics_workbench.facts_form.add_row_2")));
        auto *count = new QLabel(QString::number(satellites_.size() + stations_.size()));
        count->setObjectName("scenarioObjectCount");
        facts_form->addRow(ui::text("graphics_workbench.label_65"), count);
        auto *duration = new QLabel;
        duration->setObjectName("scenarioSettingsDuration");
        facts_form->addRow(ui::text("graphics_workbench.facts_form.add_row_3"), duration);
        auto *fields = new QWidget;
        fields->setObjectName("scenarioSettingsFields");
        auto *fields_layout = new QVBoxLayout(fields);
        fields_layout->setContentsMargins(0, 0, 0, 0);
        fields_layout->setSpacing(0);
        fields_layout->addWidget(section);
        fields_layout->addWidget(facts);
        body_layout->addWidget(fields);
        auto [data_section, data_form] = group(ui::text("graphics_workbench.label_77"));
        form = data_form;
        body_layout->addWidget(data_section);
        auto *eop_caption = new QLabel(QString::fromStdString(eop_->data_info().id));
        eop_caption->setObjectName("scenarioEopData");
        if (eop_->data_info().interpolation.starts_with("finals2000a"))
            eop_caption->setText(eop_caption->text() + ui::text("graphics_workbench.label_32"));
        eop_caption->setTextFormat(Qt::PlainText);
        eop_caption->setWordWrap(true);
        eop_caption->setToolTip(QString::fromStdString(eop_->data_info().id));
        form->addRow(ui::text("graphics_workbench.form.add_row_4"), eop_caption);
        auto *extrapolated = new QLabel(
            extrapolation_description(eop_->data_info(), scenario_->first(), scenario_->last()));
        extrapolated->setObjectName("scenarioExtrapolation");
        extrapolated->setWordWrap(true);
        form->addRow(ui::text("graphics_workbench.form.add_row_5"), extrapolated);
        // The 64-digit hash reads best on its own full-width row under its caption.
        auto *locked = new QLineEdit(QString::fromStdString(scenario_->data_lock().sha256()));
        locked->setObjectName("scenarioDataHash");
        locked->setReadOnly(true);
        locked->setAccessibleName(ui::text("graphics_workbench.locked.set_accessible_name"));
        auto *locked_caption =
            new QLabel(ui::text("graphics_workbench.locked.set_accessible_name"));
        locked_caption->setBuddy(locked);
        form->addRow(locked_caption);
        form->addRow(locked);
        data_section->add_help(ui::text("graphics_workbench.data_section.add_help"));
        align_form_labels(body);
        body_layout->addStretch();
        auto *result = new QLabel;
        result->setObjectName("scenarioSettingsResult");
        result->setWordWrap(true);
        result->hide();
        layout->addWidget(result);
        auto *buttons = new QHBoxLayout;
        auto *reset = new QPushButton(ui::text("graphics_workbench.reset"));
        reset->setObjectName("resetScenarioSettings");
        reset->setAutoDefault(false);
        buttons->addWidget(reset);
        buttons->addStretch();
        auto *close = new QPushButton(ui::text("common.close"));
        close->setAutoDefault(false);
        buttons->addWidget(close);
        auto *apply = new QPushButton(ui::text("common.apply"));
        apply->setObjectName("applyScenarioSettings");
        apply->setToolTip(ui::text("graphics_workbench.apply.set_tool_tip"));
        apply->setProperty("primary", true);
        apply->setDefault(true);
        buttons->addWidget(apply);
        layout->addLayout(buttons);
        const auto preview = [first, last, duration] {
            try {
                const auto seconds = last->epoch().seconds_since(first->epoch());
                duration->setText(seconds > 0 ? QString::number(seconds / 3600, 'g', 10) +
                                                    ui::text("graphics_workbench.duration.set_text")
                                              : ui::text("graphics_workbench.label_78"));
            } catch (const std::exception &) {
                duration->setText(ui::text("graphics_workbench.duration.set_text_2"));
            }
        };
        for (auto *control : {first, last})
            connect(control->findChild<QLineEdit *>("epochValue"), &QLineEdit::textChanged,
                    duration, preview, Qt::QueuedConnection);
        preview();
        connect(reset, &QPushButton::clicked, &dialog, [this, name, first, last, result, preview] {
            name->setText(QString::fromStdString(scenario_->name()));
            first->set_epoch(scenario_->first());
            last->set_epoch(scenario_->last());
            preview();
            result->hide();
        });
        connect(apply, &QPushButton::clicked, &dialog, [this, name, first, last, result] {
            try {
                if (name->text().trimmed().isEmpty())
                    throw std::invalid_argument(ui::utf8("graphics_workbench.validation_11"));
                const ScenarioSettings settings{name->text().trimmed().toStdString(),
                                                first->epoch(), last->epoch()};
                if (settings.last.tai_nanoseconds() <= settings.first.tai_nanoseconds())
                    throw std::invalid_argument(ui::utf8("graphics_workbench.validation_12"));
                const bool unchanged = settings.name == scenario_->name() &&
                                       settings.first == scenario_->first() &&
                                       settings.last == scenario_->last();
                if (unchanged)
                    result->setText(ui::text("graphics_workbench.result.set_text_2"));
                else if (apply_scenario_settings(settings))
                    result->setText(busy() ? ui::text("graphics_workbench.result.set_text_3")
                                           : ui::text("graphics_workbench.result.set_text_4"));
                else
                    result->setText(ui::text("graphics_workbench.label_69"));
            } catch (const std::exception &error) {
                result->setText(QString::fromUtf8(error.what()));
            }
            result->show();
        });
        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
        workspace_->open(&dialog);
    }
    void set_selection(std::optional<std::size_t> selection, bool synchronize_tree = true) {
        if (benchmarking_) {
            selected_ = selection;
            return;
        }
        if (selection && *selection >= satellites_.size() + stations_.size())
            selection.reset();
        selected_ = selection && *selection < satellites_.size() ? selection : std::nullopt;
        selected_station_ = selection && *selection >= satellites_.size()
                                ? std::optional<std::size_t>{*selection - satellites_.size()}
                                : std::nullopt;
        if (ground_map_)
            ground_map_->set_selection(selection);
        if (synchronize_tree && objects_) {
            const QSignalBlocker block(objects_);
            objects_->clearSelection();
            auto *item = selected_           ? satellites_.at(*selected_).item
                         : selected_station_ ? stations_.at(*selected_station_).item
                                             : nullptr;
            objects_->setCurrentItem(item);
            if (item)
                objects_->scrollToItem(item);
        }
        update_actions();
        show_timeline_status();
    }
    void update_satellite() {
        const ProfileScope profile("satellite_update_ms");
        if (benchmarking_) {
            markers_->update(snapshot_);
            return;
        }
        if (!sun_epoch_ || *sun_epoch_ != snapshot_.epoch) {
            const auto gcrf = runtime::display_sun_direction_gcrf(snapshot_.epoch);
            display_to_itrf_ =
                frames_->gcrf_to_itrf(snapshot_.epoch, eop_->at(snapshot_.epoch).sample);
            sun_direction_ = display_to_itrf_->apply_position(gcrf);
            sun_epoch_ = snapshot_.epoch;
            if (ground_map_)
                ground_map_->set_sun_direction(sun_direction_);
        }
        vsg::dmat4 earth_rotation;
        if (display_frame_ == core::FrameId::gcrf)
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t col = 0; col < 3; ++col)
                    earth_rotation[col][row] =
                        display_to_itrf_->matrix[col][row]; // R transpose, column-major VSG.
        earth_transform_->matrix = earth_rotation;
        visualization::RenderSnapshot fixed{
            snapshot_.epoch, core::FrameId::itrf, core::OriginId::earth, {}};
        visualization::RenderSnapshot points{
            snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}};
        std::vector<std::uint32_t> point_colors;
        fixed.markers.reserve(satellites_.size() + stations_.size());
        points.markers.reserve(satellites_.size());
        point_colors.reserve(satellites_.size());
        const auto format = slots_.empty() || !slots_.front()->window->windowAdapter
                                ? VK_FORMAT_B8G8R8A8_SRGB
                                : slots_.front()->window->windowAdapter->surfaceFormat().format;
        const bool srgb = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
        for (std::size_t i = 0; i < satellites_.size(); ++i) {
            const auto &satellite = satellites_[i];
            const auto &marker = snapshot_.markers.at(i);
            if (cache_covers(*satellite.orbit, snapshot_.epoch, snapshot_.epoch))
                fixed.markers.push_back(
                    {satellite.orbit->track.samples().provenance().object_id,
                     satellite.orbit->track.sample_state(snapshot_.epoch).position_m});
            else
                fixed.markers.push_back(
                    {marker.object_id, display_frame_ == core::FrameId::gcrf
                                           ? display_to_itrf_->apply_position(marker.position_m)
                                           : marker.position_m});
            const auto [first, last] =
                display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
            satellite.fixed_track->set_interval(first, last);
            satellite.inertial_track->set_interval(first, last);
            for (const auto &track : {satellite.fixed_track, satellite.inertial_track})
                track->set_appearance(satellite.display.line_color, satellite.display.line_width,
                                      static_cast<unsigned>(satellite.display.line_style),
                                      devicePixelRatioF(), srgb);
            satellite.tracks->setAllChildren(false);
            if (satellite.display.track_3d)
                satellite.tracks->setSingleChildOn(display_frame_ == core::FrameId::gcrf ? 0 : 1);
            if (ground_map_)
                ground_map_->set_display(i, satellite.display, first, last);
            if (satellite.model) {
                const auto &r = marker.position_m;
                satellite.model->matrix = vsg::translate(r[0], r[1], r[2]) * earth_rotation *
                                          vsg::scale(satellite.model_diameter_km * 1000);
            } else {
                points.markers.push_back(marker);
                point_colors.push_back(satellite.display.line_color);
            }
        }
        markers_->update(points, point_colors, srgb);
        std::vector<visualization::SensorMesh> sensor_meshes, sensor_footprints;
        std::map<std::string, visualization::SensorView> next_sensor_views;
        for (const auto &sensor : sensors_) {
            if (!sensor.display.show_2d && !sensor.display.show_3d)
                continue;
            const auto sat =
                std::find_if(satellites_.begin(), satellites_.end(), [&](const auto &s) {
                    return s.orbit->input.object_id == sensor.parent_satellite_id;
                });
            if (sat == satellites_.end() ||
                !cache_covers(*sat->orbit, snapshot_.epoch, snapshot_.epoch))
                continue;
            const auto state = sat->orbit->track.sample_state(snapshot_.epoch);
            core::SensorVector direction;
            for (std::size_t k = 0; k < 3; ++k)
                direction[k] = -state.position_m[k];
            if (sensor.pointing == application::SensorPointing::targeted) {
                const auto station =
                    std::find_if(stations_.begin(), stations_.end(), [&](const auto &s) {
                        return s.config.object_id == sensor.target_station_id;
                    });
                if (station == stations_.end())
                    continue;
                const auto target = core::ground_station_position(station->config.location);
                for (std::size_t k = 0; k < 3; ++k)
                    direction[k] += target[k];
            }
            auto it = sensor_views_.find(sensor.object_id);
            const auto config = sensor_view_patterns_.find(sensor.object_id);
            if (it == sensor_views_.end() || config == sensor_view_patterns_.end() ||
                config->second != sensor.pattern) {
                sensor_views_.erase(sensor.object_id);
                it = sensor_views_
                         .emplace(sensor.object_id, visualization::SensorView(sensor.pattern))
                         .first;
            }
            auto mesh = it->second.project(state.position_m,
                                           core::sensor_axes(state.velocity_mps, direction,
                                                             sensor.tilt_x_rad, sensor.tilt_y_rad,
                                                             sensor.roll_rad),
                                           sensor.display.space_range_m);
            mesh.color = sensor.display.color;
            mesh.opacity = sensor.display.opacity;
            mesh.fill = sensor.display.fill;
            mesh.show_space_extension = sensor.display.show_space_extension;
            if (sensor.display.show_2d)
                sensor_footprints.push_back(mesh);
            if (sensor.display.show_3d) {
                mesh.ground.clear();
                sensor_meshes.push_back(std::move(mesh));
            }
            next_sensor_views.emplace(sensor.object_id, std::move(it->second));
        }
        sensor_views_ = std::move(next_sensor_views);
        sensor_view_patterns_.clear();
        for (const auto &sensor : sensors_)
            sensor_view_patterns_.emplace(sensor.object_id, sensor.pattern);
        const auto required = render_vsg::SensorLayer::required(sensor_meshes);
        if (required && (!sensor_layer_ || sensor_layer_->capacity() < required)) {
            auto layer = std::make_unique<render_vsg::SensorLayer>(
                std::max<std::size_t>(1024, required * 2));
            layer->update(sensor_meshes, srgb, devicePixelRatioF());
            publish_static_node(layer->node(), [&] {
                if (sensor_layer_) {
                    auto &children = earth_transform_->children;
                    std::erase(children, sensor_layer_->node());
                }
                earth_transform_->addChild(layer->node());
                sensor_layer_ = std::move(layer);
            });
        } else if (sensor_layer_)
            sensor_layer_->update(sensor_meshes, srgb, devicePixelRatioF());
        if (ground_map_)
            ground_map_->set_sensor_footprints(sensor_footprints);
        for (std::size_t i = 0; i < stations_.size(); ++i) {
            const auto &station = stations_[i].config;
            fixed.markers.push_back(
                {station.object_id, core::ground_station_position(station.location)});
        }
        if (ground_map_)
            ground_map_->set_snapshot(fixed);
        update_access_display();
        globe_->set_illumination(sun_direction_, lighting_action_->isChecked());
        satellite_state_dirty_ = false;
    }
    void start_model(int kind, const QString &path, double diameter_km) {
        if (busy() || !editor_target_)
            return;
        if (kind < 0 || kind > 2 || !std::isfinite(diameter_km) || diameter_km <= 0 ||
            (kind == 2 && path.isEmpty())) {
            editor_->show_appearance_result(ui::text("graphics_workbench.label_79"), true);
            return;
        }
        pending_model_kind_ = kind;
        pending_model_target_ = *editor_target_;
        pending_model_path_ = path;
        pending_model_diameter_km_ = diameter_km;
        model_candidate_ = std::async(std::launch::async, [kind, path] {
            if (kind == 0)
                return vsg::ref_ptr<vsg::MatrixTransform>{};
            auto placement = vsg::MatrixTransform::create();
            placement->addChild(make_satellite_model(
                kind == 1 ? std::filesystem::path{} : std::filesystem::path(path.toStdWString())));
            return placement;
        });
        editor_->set_appearance_busy(true);
        update_actions();
    }
    void poll_model() {
        if (!model_candidate_.valid() ||
            model_candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        editor_->set_appearance_busy(false);
        try {
            auto candidate = model_candidate_.get();
            auto &satellite = satellites_.at(pending_model_target_);
            const auto previous = satellite.model_host->children;
            begin_change();
            try {
                satellite.model_host->children.clear();
                if (candidate)
                    satellite.model_host->addChild(candidate);
                rebuild();
            } catch (...) {
                satellite.model_host->children = previous;
                try {
                    rebuild();
                } catch (...) {
                    fail("Cannot restore the previous satellite appearance");
                    return;
                }
                throw;
            }
            satellite.model = std::move(candidate);
            satellite.model_kind = pending_model_kind_;
            satellite.model_diameter_km = pending_model_diameter_km_;
            satellite.model_path = pending_model_path_;
            mark_project_dirty();
            if (pending_model_target_ == 0) {
                model_node_ = satellite.model;
                model_kind_ = satellite.model_kind;
                model_diameter_km_ = satellite.model_diameter_km;
            }
            last_model_outcome_ = "applied";
            editor_->accept_appearance(satellite.model_kind, satellite.model_path,
                                       satellite.model_diameter_km);
            update_satellite();
            editor_->show_appearance_result(ui::text("graphics_workbench.label_80"));
        } catch (const std::exception &error) {
            editor_->show_appearance_result(
                ui::text("graphics_workbench.label_81") + QString::fromUtf8(error.what()), true);
            last_model_outcome_ = "rejected";
        } catch (const vsg::Exception &error) {
            editor_->show_appearance_result(ui::text("graphics_workbench.label_82") +
                                                QString::fromStdString(error.message),
                                            true);
            last_model_outcome_ = "rejected";
        }
        update_actions();
    }
    void show_timeline_status() {
        if (timeline_ && scenario_) {
            timeline_->set_time_provider(active_time());
            const auto first_ms = scenario_->first().tai_nanoseconds() / 1'000'000;
            // The ruler resolves milliseconds; keep a drawable span for a valid
            // sub-millisecond scenario. Seeks still clamp to the exact Epoch bounds.
            const auto last_ms =
                std::max(first_ms + 1, scenario_->last().tai_nanoseconds() / 1'000'000);
            timeline_->set_interval(first_ms, last_ms, QString::fromStdString(scenario_->name()),
                                    selected_name());
        }
    }
    void show_orbit_status() {
        show_timeline_status();
        if (!editor_ || !editor_target_ || busy())
            return;
        editor_->show_result({});
    }
    void start_orbit(OrbitInput input, const std::string &name) {
        if (busy() || !scenario_)
            return;
        apply_timing_ = ApplyTiming::begin("orbit_apply", ++apply_timing_sequence_);
        last_orbit_outcome_ = "rejected";
        const auto finish_request = qScopeGuard([&] {
            if (apply_timing_ && !candidate_.valid() && !project_candidate_.valid()) {
                apply_timing_->requested();
                finish_apply_timing(last_orbit_outcome_.isEmpty() ? "rejected"
                                                                  : last_orbit_outcome_);
            }
        });
        pending_orbit_target_ = editor_target_;
        pending_orbit_name_ = name;
        input.first = scenario_->first();
        input.last = scenario_->last();
        if (editor_target_)
            input.object_id = satellites_.at(*editor_target_).orbit->input.object_id;
        else if (object_id_exists(input.object_id)) {
            last_orbit_outcome_ = "rejected";
            editor_->show_result(ui::text("graphics_workbench.label_83"), true);
            return;
        }
        if (editor_target_ &&
            same_orbit_input(input, satellites_.at(*editor_target_).orbit->input)) {
            const bool renamed = satellites_[*editor_target_].name().toStdString() != name;
            rename_object(input.object_id, name);
            last_orbit_outcome_ = renamed ? "applied" : "unchanged";
            editor_->accept_input(input);
            editor_->set_name(name);
            editor_->show_result(
                ui::text(renamed ? "graphics_workbench.label_31" : "graphics_workbench.label_13"));
            return;
        }
        if (display_cancellation_)
            display_cancellation_->store(true);
        auto data_first = input.first;
        auto data_last = input.last;
        try {
            auto data_input = project_snapshot(project_path_);
            if (editor_target_) {
                data_input.satellites.at(*editor_target_).orbit = input;
                data_input.satellites.at(*editor_target_).display_name = name;
            } else {
                application::ProjectSatellite added;
                added.orbit = input;
                added.display_name = name;
                data_input.satellites.push_back(std::move(added));
            }
            if (name.empty())
                throw std::invalid_argument(ui::utf8("common.object_name_required"));
            application::validate_project(data_input);
            std::tie(data_first, data_last) = application::calculation_data_interval(data_input);
            const auto current_data = data_for_interval(data_first, data_last);
            if (!application::matching_project_data(current_data->data_info(), eop_->data_info()) ||
                !gravity_.empty() || !ephemerides_.empty() || !weather_.empty() ||
                !ocean_tides_.empty() ||
                (input.uses_hpop() &&
                 (!input.hpop.gravity_model.empty() ||
                  input.hpop_resource_options().uses_ephemeris() ||
                  input.hpop.ocean_tides.active() ||
                  (input.hpop.drag.enabled && !input.hpop.drag.weather_model.empty())))) {
                // Publish all tracks and their provider together, never mix EOP revisions.
                auto project = std::move(data_input);
                const auto id = input.object_id;
                if (start_project(std::move(project), project_path_, true,
                                  editor_target_.value_or(satellites_.size()), id)) {
                    last_orbit_outcome_.clear();
                    editor_->set_busy(true);
                } else {
                    last_orbit_outcome_ = last_project_outcome_;
                    editor_->show_result(ui::text("graphics_workbench.label_84"));
                }
                return;
            }
        } catch (const std::out_of_range &) {
            last_orbit_outcome_ = "rejected";
            show_data_coverage(data_first, data_last);
            editor_->show_result(ui::text("graphics_workbench.label_85"), true);
            return;
        } catch (const std::exception &error) {
            last_orbit_outcome_ = "rejected";
            editor_->show_result(QString::fromUtf8(error.what()), true);
            return;
        }
        cancellation_ = std::make_shared<std::atomic_bool>(false);
        last_orbit_outcome_.clear();
        const auto previous = editor_target_ ? satellites_.at(*editor_target_).orbit : nullptr;
        const bool reuse =
            previous && can_reuse_orbit_session(previous->input, scenario_->data_lock(), input,
                                                scenario_->data_lock());
        const auto settings =
            editor_target_ ? satellites_.at(*editor_target_).display : DisplaySettings{};
        const auto cache = display_cache_interval(input, settings, snapshot_.epoch);
        const auto eop = eop_;
        const auto calculation_time = time_for(eop->data_info().time_data);
        pending_orbit_sessions_ = orbit_sessions_->fork();
        if (apply_timing_) {
            apply_timing_->route = "single_orbit";
            apply_timing_->asynchronous = true;
            apply_timing_->requested();
        }
        candidate_ = std::async(std::launch::async, [this, input = std::move(input), previous,
                                                     reuse, cache, eop, calculation_time,
                                                     sessions = pending_orbit_sessions_,
                                                     cancel = cancellation_,
                                                     timing = apply_timing_] {
            const ApplyWorkerTiming worker_timing(timing);
            StageTiming session_timing(timing ? &timing->preparation.session_ms : nullptr);
            if (*cancel)
                throw OrbitCancelled{};
            if (!reuse && previous &&
                previous->session.provenance().input_revision ==
                    std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("Object input revision exhausted");
            if (timing) {
                timing->preparation.sessions_acquired += !reuse;
                timing->preparation.sessions_reused += reuse;
            }
            auto session =
                reuse ? previous->session
                      : sessions->acquire(
                            input, previous ? previous->session.provenance().input_revision + 1 : 1,
                            calculation_time.get(), {frames_, eop});
            session_timing.stop();
            return prepare_orbit_scene(input, std::move(session), *frames_, *eop, *cancel, cache,
                                       timing ? &timing->preparation : nullptr);
        });
        editor_->set_busy(true);
        update_actions();
    }
    void poll_orbit() {
        if (!candidate_.valid() ||
            candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        editor_->set_busy(false);
        if (apply_timing_)
            apply_timing_->ready();
        try {
            StageTiming publish_timing(apply_timing_ ? &apply_timing_->publish_ms : nullptr);
            auto candidate = candidate_.get();
            if (cancellation_->load())
                throw OrbitCancelled{};
            const bool first_object = satellites_.empty() && stations_.empty();
            const auto current = !playing_ && precise_epoch_ ? *precise_epoch_ : snapshot_.epoch;
            const bool reused_session =
                pending_orbit_target_ && candidate->session.provenance().input_revision ==
                                             satellites_.at(*pending_orbit_target_)
                                                 .orbit->session.provenance()
                                                 .input_revision;
            auto next_scenario =
                reused_session ? *scenario_ : scenario_->with_object(candidate->session);
            auto ground_tracks = GroundMap::prepare_tracks({candidate});
            auto fixed_track = std::make_shared<render_vsg::TrackLayer>(candidate->track.nodes());
            auto inertial_track =
                std::make_shared<render_vsg::TrackLayer>(candidate->inertial_track.nodes());
            auto track = vsg::Switch::create();
            track->addChild(true, inertial_track->node());
            track->addChild(true, fixed_track->node());
            auto model_host = vsg::Group::create();
            const auto previous = orbit_group_->children;
            const auto previous_models = model_group_->children;
            try {
                publish_static_node(track, [&] {
                    if (pending_orbit_target_)
                        orbit_group_->children.at(*pending_orbit_target_) = track;
                    else {
                        orbit_group_->addChild(track);
                        model_group_->addChild(model_host);
                    }
                });
            } catch (...) {
                orbit_group_->children = previous;
                model_group_->children = previous_models;
                try {
                    rebuild();
                } catch (...) {
                    fail("Cannot restore the previous orbit after scene compilation failed");
                    publish_timing.stop();
                    finish_apply_timing("rejected");
                    return;
                }
                throw;
            }
            std::size_t index = pending_orbit_target_.value_or(satellites_.size());
            if (pending_orbit_target_)
                satellites_[index].orbit = std::move(candidate);
            else {
                Satellite satellite;
                satellite.orbit = std::move(candidate);
                satellite.model_host = model_host;
                satellite.item = new QTreeWidgetItem(
                    scenario_item_, {QString::fromStdString(satellite.orbit->input.object_id)});
                satellite.item->setIcon(
                    0, workbench_icon(WorkbenchIcon::satellite,
                                      QColor::fromRgb(satellite.display.line_color)));
                satellite.item->setToolTip(0, ui::text("graphics_workbench.item.set_tool_tip_3"));
                satellites_.push_back(std::move(satellite));
                scenario_item_->setExpanded(true);
            }
            scenario_ = std::move(next_scenario);
            orbit_sessions_ = std::move(pending_orbit_sessions_);
            satellites_[index].display_name = pending_orbit_name_;
            satellites_[index].item->setText(0, satellites_[index].name());
            filter_objects();
            mark_project_dirty();
            if (pending_orbit_target_)
                ground_map_->replace_track(index, std::move(ground_tracks.front()));
            else
                ground_map_->append_track(std::move(ground_tracks.front()));
            ground_map_->set_name(index, satellites_[index].name());
            satellites_[index].fixed_track = std::move(fixed_track);
            satellites_[index].inertial_track = std::move(inertial_track);
            satellites_[index].tracks = track;
            editor_target_ = index;
            editor_->accept_input(satellites_[index].orbit->input);
            editor_->set_name(satellites_[index].name().toStdString());
            editor_->set_create_mode(false);
            properties_dialog_->setWindowTitle(
                satellites_[index].name() +
                ui::text("graphics_workbench.properties_dialog.set_window_title_2"));
            orbit_ = satellites_.front().orbit;
            object_item_ = satellites_.front().item;
            snapshot_ = sample_frame(current);
            satellite_state_dirty_ = true;
            update_satellite();
            set_selection(index);
            selection_->update({snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}});
            for (auto &slot : slots_)
                slot->pick_candidates.clear();
            seek_epoch(current);
            initial_position_ = snapshot_.markers.front().position_m;
            if (first_object) {
                fit_views();
                fit_pending_ = true;
            }
            show_orbit_status();
            editor_->show_result(ui::text("graphics_workbench.orbit_applied"));
            last_orbit_outcome_ = "applied";
        } catch (const OrbitCancelled &) {
            editor_->show_result(ui::text("graphics_workbench.label_54"));
            last_orbit_outcome_ = "cancelled";
        } catch (const std::exception &error) {
            editor_->show_result(
                ui::text("graphics_workbench.label_86") + QString::fromUtf8(error.what()), true);
            last_orbit_outcome_ = "rejected";
        } catch (const vsg::Exception &error) {
            editor_->show_result(ui::text("graphics_workbench.label_87") +
                                     QString::fromStdString(error.message),
                                 true);
            last_orbit_outcome_ = "rejected";
        }
        finish_apply_timing(last_orbit_outcome_);
        if (insert_defaults_pending_ && last_orbit_outcome_ == "rejected")
            show_properties_dialog();
        insert_defaults_pending_ = false;
        pending_orbit_sessions_.reset();
        finish_insertion(last_orbit_outcome_ == "applied", last_orbit_outcome_ == "cancelled");
        update_actions();
    }
    void request_display_cache() {
        if (benchmarking_ || busy() || display_candidate_.valid() || !scenario_)
            return;
        for (std::size_t i = 0; i < satellites_.size(); ++i) {
            const auto &satellite = satellites_[i];
            if (!satellite.display.track_2d && !satellite.display.track_3d)
                continue;
            const auto wanted =
                display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
            if (cache_covers(*satellite.orbit, wanted.first, wanted.second))
                continue;
            if (failed_display_source_ == satellite.orbit &&
                failed_display_settings_ == satellite.display)
                continue;
            display_source_ = satellite.orbit;
            display_target_ = i;
            display_settings_ = satellite.display;
            display_cancellation_ = std::make_shared<std::atomic_bool>(false);
            const auto interval =
                display_cache_interval(satellite.orbit->input, satellite.display, snapshot_.epoch);
            display_candidate_ =
                std::async(std::launch::async, [this, source = display_source_, eop = eop_,
                                                interval, cancel = display_cancellation_] {
                    return prepare_orbit_scene(source->input, source->session, *frames_, *eop,
                                               *cancel, interval);
                });
            statusBar()->showMessage(ui::text("graphics_workbench.label_88"));
            display_status_pending_ = true;
            display_status_connection_ = connect(
                statusBar(), &QStatusBar::messageChanged, this,
                [this] { display_status_pending_ = false; }, Qt::SingleShotConnection);
            break;
        }
    }
    void poll_display_cache() {
        if (!display_candidate_.valid() ||
            display_candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        // A cancelled/superseded display job must release its own pending hint,
        // without clearing a newer message from another operation.
        disconnect(display_status_connection_);
        if (display_status_pending_)
            statusBar()->clearMessage();
        display_status_pending_ = false;
        try {
            const ProfileScope profile("cache_publish_ms");
            auto candidate = display_candidate_.get();
            if (display_target_ >= satellites_.size() ||
                satellites_[display_target_].orbit != display_source_ ||
                satellites_[display_target_].display != display_settings_ ||
                display_cancellation_->load())
                return;
            const auto wanted = display_interval(*candidate, display_settings_, snapshot_.epoch);
            if (!cache_covers(*candidate, wanted.first, wanted.second))
                return; // A later seek superseded this request.
            auto fixed = std::make_shared<render_vsg::TrackLayer>(candidate->track.nodes());
            auto inertial =
                std::make_shared<render_vsg::TrackLayer>(candidate->inertial_track.nodes());
            auto tracks = vsg::Switch::create();
            tracks->addChild(true, inertial->node());
            tracks->addChild(true, fixed->node());
            auto prepared_map = GroundMap::prepare_tracks({candidate});
            auto map_track = std::move(prepared_map.front());
            const auto previous = orbit_group_->children.at(display_target_);
            try {
                publish_static_node(tracks,
                                    [&] { orbit_group_->children.at(display_target_) = tracks; });
            } catch (...) {
                orbit_group_->children.at(display_target_) = previous;
                rebuild();
                throw;
            }
            auto &satellite = satellites_[display_target_];
            satellite.orbit = std::move(candidate);
            satellite.fixed_track = std::move(fixed);
            satellite.inertial_track = std::move(inertial);
            satellite.tracks = tracks;
            ground_map_->replace_track(display_target_, std::move(map_track));
            ground_map_->set_name(display_target_, satellite.name());
            satellite_state_dirty_ = true;
            orbit_ = satellites_.front().orbit;
            ++display_cache_publications_;
            statusBar()->showMessage(ui::text("graphics_workbench.label_89"), 2000);
        } catch (const OrbitCancelled &) {
        } catch (const std::exception &error) {
            failed_display_source_ = display_source_;
            failed_display_settings_ = display_settings_;
            statusBar()->showMessage(ui::text("graphics_workbench.label_90") +
                                     QString::fromUtf8(error.what()));
        } catch (const vsg::Exception &error) {
            failed_display_source_ = display_source_;
            failed_display_settings_ = display_settings_;
            statusBar()->showMessage(ui::text("graphics_workbench.label_90") +
                                     QString::fromStdString(error.message));
        }
    }
    void fit_views(std::optional<std::size_t> target = std::nullopt) {
        if (benchmarking_ || !scenario_)
            return;
        double bound = 6378137.0;
        for (const auto &satellite : satellites_)
            bound = std::max(bound, satellite.orbit->max_radius_m);
        bound *= 1.15;
        const auto r = snapshot_.markers.empty()
                           ? std::array<double, 3>{-3e6, 5e6, 2e6}
                           : snapshot_.markers.at(selected_marker_index().value_or(0)).position_m;
        const auto direction = vsg::normalize(vsg::dvec3(r[0], r[1], r[2]));
        const auto reference_up =
            std::abs(direction.z) > 0.9 ? vsg::dvec3(0, 1, 0) : vsg::dvec3(0, 0, 1);
        const auto side = vsg::normalize(vsg::cross(reference_up, direction));
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (target && *target != i)
                continue;
            auto &slot = *slots_[i];
            auto *perspective =
                dynamic_cast<vsg::Perspective *>(slot.camera->projectionMatrix.get());
            const double half_fov = std::atan(std::tan(35.0 * std::acos(-1.0) / 360.0) *
                                              std::min(1.0, perspective->aspectRatio));
            const double distance = bound / std::sin(half_fov);
            const auto view_direction = vsg::normalize(direction + side * (i ? 0.22 : -0.22));
            auto *look = dynamic_cast<vsg::LookAt *>(slot.camera->viewMatrix.get());
            look->eye = view_direction * distance;
            look->center = vsg::dvec3(0, 0, 0);
            look->up = vsg::normalize(vsg::cross(view_direction, side));
            if (slot.navigation)
                slot.navigation->set_viewpoint(
                    vsg::LookAt::create(look->eye, look->center, look->up));
            perspective->nearDistance = 6378.137;
            perspective->farDistance = distance + bound * 20;
        }
    }
    void focus_satellite(std::optional<std::size_t> target = std::nullopt) {
        if (!selected_marker_index())
            return;
        const auto &r = snapshot_.markers.at(*selected_marker_index()).position_m;
        const vsg::dvec3 centre(r[0], r[1], r[2]);
        const double distance = selected_ && satellites_.at(*selected_).model
                                    ? satellites_.at(*selected_).model_diameter_km * 3000
                                    : 1e6;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (target && *target != i)
                continue;
            auto &slot = slots_[i];
            auto *look = dynamic_cast<vsg::LookAt *>(slot->camera->viewMatrix.get());
            const auto direction = vsg::normalize(selected_station_ ? centre : look->eye - centre);
            const auto up = selected_station_ ? (std::abs(direction.z) > 0.9 ? vsg::dvec3(0, 1, 0)
                                                                             : vsg::dvec3(0, 0, 1))
                                              : look->up;
            const auto viewpoint = vsg::LookAt::create(centre + direction * distance, centre, up);
            if (slot->navigation)
                slot->navigation->set_viewpoint(viewpoint);
            else {
                look->eye = viewpoint->eye;
                look->center = centre;
                look->up = up;
            }
            auto *perspective =
                dynamic_cast<vsg::Perspective *>(slot->camera->projectionMatrix.get());
            perspective->nearDistance = std::max(0.01, distance * 1e-4);
        }
    }
    void fail(const std::string &message) {
        failure_ = QString::fromStdString(message);
        std::cerr << "Graphics failure: " << message << '\n';
        timer_.stop();
        app_.exit(1);
        QTimer::singleShot(0, this, [this] { app_.exit(1); });
    }
    void smoke_set_view_layout(int mode) {
        workspace_->dock_page(view_panel_);
        workspace_->dock_page(map_panel_);
        workspace_->select_view(0);
        if (mode == 2)
            workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
        else {
            workspace_->merge_page(map_panel_, view_panel_);
            workspace_->select_view(mode);
        }
    }
    QTabBar *smoke_tabs(const QString &title) const {
        for (auto *tabs : workspace_->findChildren<QTabBar *>())
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == title)
                    return tabs;
        throw std::runtime_error("Workspace page is missing");
    }
    void smoke_drag_page(const QString &title, const QPoint &global) {
        auto *tabs = smoke_tabs(title);
        int index = 0;
        while (tabs->tabText(index) != title)
            ++index;
        const auto start = tabs->tabRect(index).center();
        QMouseEvent press(QEvent::MouseButtonPress, start, tabs->mapToGlobal(start), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, tabs->mapFromGlobal(global), global, Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, tabs->mapFromGlobal(global), global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tabs, &press);
        QCoreApplication::sendEvent(tabs, &move);
        QCoreApplication::sendEvent(tabs, &release);
    }
    void zoom_view(std::size_t index, double factor) {
        auto &slot = *slots_.at(index);
        if (slot.navigation)
            slot.navigation->zoom(1 / factor);
    }
    void change_display_frame(int index) {
        if (!scenario_ || !display_to_itrf_)
            return;
        const auto next = index == 0 ? core::FrameId::gcrf : core::FrameId::itrf;
        if (display_frame_ == next)
            return;
        const auto convert = [&](vsg::dvec3 value) {
            const std::array<double, 3> input{value.x, value.y, value.z};
            const auto p = next == core::FrameId::itrf ? display_to_itrf_->apply_position(input)
                                                       : display_to_itrf_->inverse_position(input);
            return vsg::dvec3(p[0], p[1], p[2]);
        };
        for (auto &slot : slots_) {
            auto *look = dynamic_cast<vsg::LookAt *>(slot->camera->viewMatrix.get());
            const auto viewpoint =
                vsg::LookAt::create(convert(look->eye), convert(look->center), convert(look->up));
            if (slot->navigation)
                slot->navigation->set_viewpoint(viewpoint);
            else
                slot->camera->viewMatrix = viewpoint;
        }
        display_frame_ = next;
        for (auto *choice : frame_choices_) {
            const QSignalBlocker blocker(choice);
            choice->setCurrentIndex(index);
        }
        snapshot_ = sample_frame(snapshot_.epoch);
        update_satellite();
    }
    // The workbench play button is an ice-filled circle; the benchmark bar keeps plain icons.
    [[nodiscard]] QIcon play_icon(bool playing) const {
        return workbench_icon(playing ? WorkbenchIcon::pause : WorkbenchIcon::play,
                              theme::color(benchmarking_ ? theme::text_soft : theme::on_accent));
    }
    void set_workspace_name(const QString &name) {
        const bool extrapolated = eop_->data_info().extrapolation_days > 0;
        workspace_label_->setText(
            name +
            (extrapolated ? ui::text("graphics_workbench.workspace_label.set_text") : QString{}));
        workspace_label_->setToolTip(workspace_label_->text());
        workspace_dot_->setProperty("active", true);
        workspace_dot_->setProperty("warning", extrapolated);
        workspace_dot_->style()->unpolish(workspace_dot_);
        workspace_dot_->style()->polish(workspace_dot_);
    }
    QWidget *make_view_controls(std::optional<std::size_t> slot) {
        auto *header = new QWidget;
        header->setObjectName("viewHeading");
        header->setMinimumHeight(34);
        header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *rows = new QHBoxLayout(header);
        rows->setContentsMargins(12, 4, 8, 4);
        rows->setSpacing(4);
        auto *bar = new QHBoxLayout;
        bar->setContentsMargins(0, 0, 0, 0);
        bar->setSpacing(4);
        auto *title =
            new QLabel(slot ? ui::text("body.earth") : ui::text("graphics_workbench.title_4"));
        title->setObjectName("viewTitle");
        bar->addWidget(title);
        if (slot) {
            auto *choice = new QComboBox;
            choice->setObjectName("viewFrame");
            choice->setAccessibleName(ui::text("graphics_workbench.choice.set_accessible_name"));
            choice->addItems(
                {ui::text("graphics_workbench.label_91"), ui::text("graphics_workbench.label_92")});
            choice->setToolTip(ui::text("graphics_workbench.choice.set_tool_tip"));
            choice->setCurrentIndex(display_frame_ == core::FrameId::gcrf ? 0 : 1);
            connect(choice, &QComboBox::currentIndexChanged, this,
                    [this](int index) { change_display_frame(index); });
            frame_choices_.push_back(choice);
            bar->addWidget(choice);
        }
        rows->addLayout(bar);
        bar = new QHBoxLayout;
        bar->setContentsMargins(0, 0, 0, 0);
        bar->setSpacing(4);
        rows->addLayout(bar, 1);
        auto *tools = new QFrame;
        tools->setObjectName("viewTools");
        auto *tool_layout = new QHBoxLayout(tools);
        tool_layout->setContentsMargins(2, 1, 2, 1);
        tool_layout->setSpacing(1);
        bar->addWidget(tools);
        const auto button = [&](WorkbenchIcon icon, const QString &text, auto command) {
            auto *control = new QToolButton;
            control->setIcon(workbench_icon(icon));
            control->setIconSize(QSize(16, 16));
            control->setFixedSize(26, 26);
            control->setToolTip(text);
            control->setAccessibleName(text);
            connect(control, &QToolButton::clicked, this, command);
            tool_layout->addWidget(control);
            return control;
        };
        button(WorkbenchIcon::fit,
               slot ? ui::text("graphics_workbench.label_93")
                    : ui::text("graphics_workbench.label_94"),
               [this, slot] {
                   if (slot)
                       fit_views(slot);
                   else
                       ground_map_->reset_view();
               });
        view_focus_buttons_.push_back(
            button(WorkbenchIcon::focus, ui::text("graphics_workbench.label_95"), [this, slot] {
                if (slot)
                    focus_satellite(slot);
                else
                    ground_map_->focus_selection();
            }));
        for (const auto &[icon, factor, text] :
             {std::tuple{WorkbenchIcon::zoom_in, 1.3, ui::text("graphics_workbench.label_96")},
              std::tuple{WorkbenchIcon::zoom_out, 1 / 1.3,
                         ui::text("graphics_workbench.label_97")}})
            button(icon, text + (factor > 1 ? QStringLiteral(" · +") : QStringLiteral(" · −")),
                   [this, slot, factor] {
                       if (slot)
                           zoom_view(*slot, 1 / factor);
                       else
                           ground_map_->zoom_centered(factor);
                   });
        bar->addStretch();
        auto *help = info_marker(header);
        help->setAccessibleName(ui::text("graphics_workbench.navigation_help"));
        append_help(help,
                    ui::text(slot ? "graphics_workbench.help" : "ground_map.navigation_help"));
        bar->addWidget(help);
        auto *settings = new QMenu(header);
        if (!lighting_action_) {
            lighting_action_ = new QAction(ui::text("graphics_workbench.settings"), this);
            lighting_action_->setCheckable(true);
            lighting_action_->setChecked(true);
            connect(lighting_action_, &QAction::toggled, this, [this](bool enabled) {
                if (ground_map_)
                    ground_map_->set_lighting(enabled);
                satellite_state_dirty_ = true;
            });
            tracks_action_ = new QAction(ui::text("graphics_workbench.label_98"), this);
            tracks_action_->setCheckable(true);
            tracks_action_->setChecked(true);
            connect(tracks_action_, &QAction::toggled, this,
                    [this](bool enabled) { orbit_switch_->setAllChildren(enabled); });
        }
        settings->addAction(lighting_action_);
        settings->addSeparator();
        if (slot) {
            settings->addAction(tracks_action_);
        } else {
            for (const auto &[text, method] :
                 {std::pair{ui::text("graphics_workbench.label_99"), &GroundMap::set_grid_visible},
                  std::pair{ui::text("graphics_workbench.label_100"),
                            &GroundMap::set_tracks_visible},
                  std::pair{ui::text("graphics_workbench.label_101"),
                            &GroundMap::set_labels_visible},
                  std::pair{ui::text("graphics_workbench.label_102"),
                            &GroundMap::set_sun_visible}}) {
                auto *action = settings->addAction(text);
                action->setCheckable(true);
                action->setChecked(true);
                connect(action, &QAction::toggled, ground_map_, method);
            }
        }
        auto *display = new QToolButton;
        display->setObjectName(slot ? "display3D" : "display2D");
        display->setIcon(workbench_icon(WorkbenchIcon::properties));
        display->setIconSize(QSize(18, 18));
        display->setToolTip(ui::text("graphics_workbench.display.set_tool_tip"));
        display->setAccessibleName(ui::text("graphics_workbench.display.set_tool_tip"));
        display->setFixedSize(30, 28);
        display->setMenu(settings);
        display->setPopupMode(QToolButton::InstantPopup);
        bar->addWidget(display);
        responsive_columns(header, rows, 420);
        return header;
    }
    void build_slot(bool secondary) {
        auto slot = std::make_unique<Slot>();
        auto *current = slot.get();
        auto traits = vsg::WindowTraits::create();
        traits->width = 640;
        traits->height = 640;
        traits->deviceTypePreferences = {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU};
        globe_->prepare_window(*traits, validation_);
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_)
            prepare_present_timing(*traits);
#endif
        if (!smoke_dir_.isEmpty())
            traits->swapchainPreferences.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        traits->device = shared_device_ref_;
        current->window = new EmbeddedWindow(viewer_, traits);
        current->container = QWidget::createWindowContainer(current->window);
        current->container->setMinimumSize(64, 64);
        current->container->setFocusPolicy(Qt::StrongFocus);
        if (!benchmarking_) {
            current->container->setAccessibleName(
                ui::text("graphics_workbench.container.set_accessible_name"));
            const auto help = ui::text("graphics_workbench.help");
            current->container->setAccessibleDescription(help);
        }
        if (benchmarking_) {
            const auto ratio = current->window->devicePixelRatio();
            const int width = qRound(1280.0 / ratio), height = qRound(720.0 / ratio);
            if (qRound(width * ratio) != 1280 || qRound(height * ratio) != 720)
                throw std::runtime_error("This DPI cannot represent a 1280x720 native viewport; "
                                         "choose a compatible QT_SCALE_FACTOR");
            current->container->setFixedSize(width, height);
        }
        if (secondary) {
            current->dock = new QDockWidget(ui::text("graphics_workbench.label_103"), this);
            current->dock->setWidget(current->container);
            addDockWidget(Qt::RightDockWidgetArea, current->dock);
            if (benchmarking_) {
                resizeDocks({current->dock}, {600}, Qt::Horizontal);
                auto *action = menuBar()->addAction(ui::text("graphics_workbench.action"));
                connect(action, &QAction::triggered, current->dock, &QWidget::show);
            } else {
                style_dock(current->dock, WorkbenchIcon::views);
                auto *action = current->dock->toggleViewAction();
                aux_action_ = action;
                action->setIcon(workbench_icon(WorkbenchIcon::views));
                action->setText(ui::text("graphics_workbench.action.set_text"));
                action->setShortcut(QKeySequence("Ctrl+2"));
                view_menu_->addAction(action);
                auto *panel = new QWidget;
                auto *layout = new QVBoxLayout(panel);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(0);
                layout->addWidget(make_view_controls(1));
                layout->addWidget(current->container, 1);
                current->dock->setWidget(panel);
                connect(current->dock, &QDockWidget::visibilityChanged, this,
                        [this](bool) { fit_pending_ = true; });
            }
        } else {
            if (benchmarking_) {
                setCentralWidget(current->container);
            } else {
                auto *panel = new QWidget;
                auto *layout = new QVBoxLayout(panel);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(0);
                layout->addWidget(make_view_controls(0));
                layout->addWidget(current->container, 1);
                view_panel_ = panel;
                workspace_ = new Workspace;
                workspace_->add_view(panel, ui::text("workspace.view_3d"),
                                     workbench_icon(WorkbenchIcon::earth));
                workspace_pages_ = new QStackedWidget;
                workspace_pages_->addWidget(workspace_);
                auto *welcome = make_welcome_page(new_action_, open_action_, example_action_);
                workspace_pages_->addWidget(welcome);
                workspace_pages_->setCurrentWidget(welcome);
                setCentralWidget(workspace_pages_);
            }
        }
        constexpr double radius = 6378137.0;
        // Two independent cameras that both face the fixed demo's visible hemisphere.
        const double angle =
            std::atan2(initial_position_[1], initial_position_[0]) + (secondary ? 0.35 : -0.35);
        const vsg::dvec3 look(6 * radius * std::cos(angle), 6 * radius * std::sin(angle),
                              2.6 * radius);
        current->camera =
            vsg::Camera::create(vsg::Perspective::create(35.0, 1.0, radius * 0.001, radius * 20),
                                vsg::LookAt::create(look, vsg::dvec3(0, 0, 0), vsg::dvec3(0, 0, 1)),
                                vsg::ViewportState::create(0, 0, 640, 640));
        slots_.push_back(std::move(slot));
        current->window->attached = [this, current] { attach(*current); };
        current->window->detaching = [this, current] { detach(*current); };
        current->window->failed = [this](const std::string &message) { fail(message); };
        current->window->synchronize = [this](const std::function<void()> &operation) {
            run_exclusive(operation);
        };
        current->window->pressed = [this, current](QPointF point) {
#ifdef ORBELLE_DESKTOP_BENCHMARK
            if (run_ && !injecting_input_)
                return;
#endif
            set_selection(
                render_vsg::pick_marker(current->pick_candidates, point.x(), point.y(), 8));
        };
        current->window->initializeWindow();
    }
    void attach(Slot &slot) {
        begin_change();
        auto adapter = slot.window->windowAdapter;
        viewer_->addWindow(adapter);
        if (validation_)
            log_.install(adapter->getOrCreateInstance());
        // Validation belongs to the instance. Vulkan no longer permits device layers.
        adapter->traits()->requestedLayers.clear();
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_) {
            const auto capabilities = present_timing_capabilities(*adapter);
            timing_capabilities_.append(capabilities);
            if (!capabilities["present_id2_feature"].toBool() ||
                !capabilities["present_timing_feature"].toBool() ||
                !capabilities["present_id2_surface"].toBool() ||
                !capabilities["present_timing_surface"].toBool() ||
                !capabilities["first_pixel_out"].toBool())
                throw std::runtime_error("This window cannot provide first-pixel-output timing");
        }
#endif
        // Realize the shared device before preparing the second window's traits.
        auto device = adapter->getOrCreateDevice();
        if (shared_device_ref_ && shared_device_ref_ != device)
            throw std::runtime_error("A recreated view changed the shared Vulkan device");
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_ && !shared_device_ref_)
            run_->configure_video_memory(*device->getPhysicalDevice());
#endif
        shared_device_ref_ = device;
        slot.window->traits->device = device;
        if (!smoke_dir_.isEmpty())
            slot.capture = render_vsg::SwapchainCapture::create(adapter);
        const auto extent = adapter->extent2D();
        slot.camera->viewportState = vsg::ViewportState::create(0, 0, extent.width, extent.height);
        auto *perspective = dynamic_cast<vsg::Perspective *>(slot.camera->projectionMatrix.get());
        perspective->aspectRatio = static_cast<double>(extent.width) / std::max(1u, extent.height);
        // Keep the logical view alive across surface changes: Rocky may still hold
        // its raw identity in a queued CompileResult until the next update.
        if (!slot.view) {
            slot.view = vsg::View::create(slot.camera, scene_);
            if (!benchmarking_) {
                slot.station_markers = std::make_unique<render_vsg::MarkerLayer>(
                    10000, render_vsg::MarkerStyle::ground_station);
                slot.station_selection = std::make_unique<render_vsg::MarkerLayer>(
                    1, render_vsg::MarkerStyle::ground_selection);
                slot.view->addChild(slot.station_markers->node());
                slot.view->addChild(slot.station_selection->node());
            }
#ifdef ORBELLE_DESKTOP_BENCHMARK
            if (benchmark_) {
                slot.labels = std::make_unique<render_vsg::LabelLayer>(200, benchmark_->font);
                slot.view->addChild(slot.labels->node());
            }
#endif
        }
        globe_->prepare_view(*slot.view);
        auto render_graph = vsg::RenderGraph::create(adapter, slot.view);
        slot.render_graph = render_graph;
        render_graph->setClearValues(benchmarking_
                                         ? VkClearColorValue{{0.018f, 0.025f, 0.045f, 1.0f}}
                                         : VkClearColorValue{{0.0003f, 0.0006f, 0.0012f, 1.0f}});
        slot.graph = vsg::CommandGraph::create(adapter);
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_) {
            if (slot.gpu)
                throw std::runtime_error("A GPU-timed view was recreated; this run is invalid");
            const auto position =
                std::find_if(slots_.begin(), slots_.end(),
                             [&](const auto &value) { return value.get() == &slot; });
            slot.gpu = std::make_shared<BenchmarkGpu>(
                device, static_cast<std::uint32_t>(slot.graph->queueFamily),
                static_cast<int>(std::distance(slots_.begin(), position)), *run_);
            gpu_metadata_.append(slot.gpu->metadata());
            slot.graph->addChild(slot.gpu->command(true));
        }
#endif
        slot.graph->addChild(render_graph);
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (slot.gpu)
            slot.graph->addChild(slot.gpu->command(false));
#endif
        if (slot.capture)
            slot.graph->addChild(slot.capture);
        if (!benchmarking_) {
            slot.navigation = std::make_unique<ViewNavigation>(*slot.window, slot.camera);
            slot.navigation->select = [&slot](QPointF point) {
                if (slot.window->pressed)
                    slot.window->pressed(point * slot.window->devicePixelRatio());
            };
            slot.navigation->fit = [this, &slot] {
                const auto position =
                    std::find_if(slots_.begin(), slots_.end(),
                                 [&slot](const auto &value) { return value.get() == &slot; });
                fit_views(static_cast<std::size_t>(std::distance(slots_.begin(), position)));
            };
        }
        ++slot.creations;
        if (timer_.isActive())
            rebuild();
    }
    void detach(Slot &slot) {
        if (!slot.graph || stopped_)
            return;
        begin_change();
        globe_->release_view(*slot.view);
        slot.navigation.reset();
        slot.graph = {};
        slot.render_graph = {};
        slot.pick_candidates.clear();
        ++slot.destructions;
        rebuild();
    }
    void begin_change() {
        if (compiler_)
            compiler_->begin_change(*viewer_);
        viewer_->deviceWaitIdle();
    }
    void publish_static_node(vsg::ref_ptr<vsg::Node> node, const std::function<void()> &publish) {
        // Track buffers are static. Compile only the replacement, retaining the current
        // render tasks, swapchains and all other satellites. An idle barrier protects
        // resources referenced by previous submissions before the old node is released.
        if (compiler_ && !viewer_->recordAndSubmitTasks.empty()) {
            const auto compiled = compiler_->compile(node);
            if (!compiled)
                throw std::runtime_error("Track compilation failed: " + compiled.message);
            run_exclusive([&] {
                viewer_->deviceWaitIdle();
                if (compiled.requiresViewerUpdate(viewer_.get()))
                    vsg::updateViewer(*viewer_, compiled);
                publish();
            });
        } else {
            begin_change();
            publish();
            rebuild();
        }
    }
    void run_exclusive(const std::function<void()> &operation) {
        if (compiler_)
            compiler_->run_exclusive(operation);
        else
            operation();
    }
    void rebuild() {
        begin_change();
        vsg::CommandGraphs graphs;
        for (const auto &slot : slots_)
            if (slot->graph)
                graphs.push_back(slot->graph);
        viewer_->assignRecordAndSubmitTaskAndPresentation(graphs);
        for (const auto &task : viewer_->recordAndSubmitTasks) {
            // A new upload must wait before COPY, rather than at VSG's default BOTTOM_OF_PIPE.
            task->earlyTransferConsumerCompletedSemaphore->pipelineStageFlags() =
                VK_PIPELINE_STAGE_TRANSFER_BIT;
            task->lateTransferConsumerCompletedSemaphore->pipelineStageFlags() =
                VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        if (!graphs.empty()) {
            if (!compiler_) {
                compiler_ = render_vsg::CompileManager::create(*viewer_, hints_);
                viewer_->compileManager = compiler_;
                compiler_->begin_change(*viewer_);
            }
            const auto result = viewer_->compile(hints_);
            if (!result)
                throw std::runtime_error("VSG scene compilation failed: " + result.message);
            compiler_->end_change(*viewer_);
        }
    }
    bool profile_step() {
        if (!profile_)
            return true;
        if (!profile_started_) {
            profile_started_ = true;
            const auto time = runtime::make_builtin_time_provider();
            const auto first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
            const auto last = time->from_utc({2026, 10, 10, 0, 0, 0, 0});
            activate_scenario(application::Scenario("Interaction profile", first, last));
            cancellation_ = std::make_shared<std::atomic_bool>(false);
            profile_orbits_ = std::async(std::launch::async, [this, first, last,
                                                              cancel = cancellation_] {
                std::vector<std::shared_ptr<const OrbitScene>> orbits;
                for (int i = 0; i < profile_count_; ++i) {
                    OrbitInput input;
                    input.first = input.initial.epoch = first;
                    input.last = last;
                    input.object_id = "Profile-" + std::to_string(i + 1);
                    const auto angle = 6.283185307179586 * i / profile_count_;
                    input.initial.position_m = {7e6 * std::cos(angle), 7e6 * std::sin(angle), 0};
                    input.initial.velocity_mps = {-6200 * std::sin(angle), 6200 * std::cos(angle),
                                                  5300};
                    auto session = make_orbit_session(input, 1);
                    orbits.push_back(
                        prepare_orbit_scene(input, std::move(session), *frames_, *eop_, *cancel));
                }
                return orbits;
            });
        }
        if (profile_orbits_.valid()) {
            if (profile_orbits_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return true;
            auto orbits = profile_orbits_.get();
            begin_change();
            for (const auto &orbit : orbits) {
                Satellite satellite;
                satellite.orbit = orbit;
                satellite.display.extent = 1;
                satellite.fixed_track =
                    std::make_shared<render_vsg::TrackLayer>(orbit->track.nodes());
                satellite.inertial_track =
                    std::make_shared<render_vsg::TrackLayer>(orbit->inertial_track.nodes());
                satellite.tracks = vsg::Switch::create();
                satellite.tracks->addChild(true, satellite.inertial_track->node());
                satellite.tracks->addChild(true, satellite.fixed_track->node());
                satellite.item = new QTreeWidgetItem(
                    scenario_item_, {QString::fromStdString(orbit->input.object_id)});
                orbit_group_->addChild(satellite.tracks);
                model_group_->addChild(satellite.model_host);
                scenario_ = scenario_->with_object(orbit->session);
                satellites_.push_back(std::move(satellite));
            }
            rebuild();
            ground_map_->set_orbits(orbits);
            orbit_ = orbits.front();
            satellite_state_dirty_ = true;
            fit_pending_ = true;
        }
        if (satellites_.empty())
            return true;
        static const QStringList phases{"3d-paused",  "3d-drag", "2d-paused",  "2d-hover",
                                        "2d-pan",     "2d-play", "split-play", "2d-zoom",
                                        "2d-settled", "3d-zoom"};
        constexpr int warmup = 45, measured = 180, length = warmup + measured;
        const int phase = profile_frame_ / length, local = profile_frame_ % length;
        if (phase == phases.size()) {
            profile_->finish(QJsonObject{
                {"satellites", profile_count_},
                {"track_nodes_per_satellite", 1441},
                {"step_seconds", 60},
                {"duration_seconds", 86400},
                {"gpu", QString::fromUtf8(slots_[0]
                                              ->window->windowAdapter->getPhysicalDevice()
                                              ->getProperties()
                                              .deviceName)},
                {"map_width", ground_map_->width()},
                {"map_height", ground_map_->height()},
                {"device_pixel_ratio", devicePixelRatioF()},
                {"navigation",
                 QJsonObject{{"map_pan_moves", profile_pan_moves_},
                             {"map_pan_max_displacement_pixels", profile_pan_distance_},
                             {"map_zoom_changes", profile_zoom_changes_},
                             {"map_zoom_min", profile_zoom_min_},
                             {"map_zoom_max", profile_zoom_max_}}},
                {"map_quality_restored_after_zoom", true},
                {"scope",
                 "GUI CPU and timer cadence; synthetic Qt input, not input-to-photon latency"}});
            finished_ = true;
            timer_.stop();
            app_.exit(0);
            return false;
        }
        profile_->phase(phases[phase], local >= warmup);
        profile_->tick();
        const auto mouse = [](QObject *target, QEvent::Type type, QPointF p, Qt::MouseButton button,
                              Qt::MouseButtons buttons) {
            QMouseEvent event(type, p, p, button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &event);
        };
        const auto map_plot = [this] {
            return QRectF(ground_map_->rect()).adjusted(38, 14, -16, -44);
        };
        const auto view_plot = [this] {
            return QRectF(QPointF{}, QSizeF(slots_[0]->window->size()));
        };
        if (local == 0) {
            std::cerr << "Profile phase: " << phases[phase].toStdString() << '\n';
            set_playing(phase == 5 || phase == 6);
            if (phase < 2)
                workspace_->select_view(0);
            else if (phase < 6)
                workspace_->select_view(1);
            else if (phase == 6)
                workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
            else if (phase != 8) {
                workspace_->merge_page(map_panel_, view_panel_);
                workspace_->select_view(phase == 7 ? 1 : 0);
            }
            if (phase == 1)
                mouse(slots_[0]->window, QEvent::MouseButtonPress, view_plot().center(),
                      Qt::LeftButton, Qt::LeftButton);
            if (phase == 4) {
                ground_map_->zoom(2, map_plot().center());
                profile_pan_origin_ = ground_map_->marker_position(0);
                mouse(ground_map_, QEvent::MouseButtonPress, map_plot().center(), Qt::LeftButton,
                      Qt::LeftButton);
            }
            if (phase == 5)
                ground_map_->reset_view();
            if (phase == 7)
                profile_zoom_min_ = profile_zoom_max_ = ground_map_->zoom_factor();
        }
        if (phase == 1 || phase == 3 || phase == 4) {
            const auto area = phase == 1 ? view_plot() : map_plot();
            const auto point =
                area.center() + QPointF(area.width() * .18 * std::sin(local * 0.045),
                                        area.height() * .18 * std::cos(local * 0.045));
            const auto before = phase == 4 ? ground_map_->marker_position(0) : QPointF{};
            mouse(phase == 1 ? static_cast<QObject *>(slots_[0]->window) : ground_map_,
                  QEvent::MouseMove, point, Qt::NoButton,
                  phase == 3 ? Qt::NoButton : Qt::LeftButton);
            if (phase == 4) {
                const auto after = ground_map_->marker_position(0);
                if (QLineF(before, after).length() > .1)
                    ++profile_pan_moves_;
                profile_pan_distance_ =
                    std::max(profile_pan_distance_, QLineF(profile_pan_origin_, after).length());
            }
            if (local == length - 1 && phase != 3)
                mouse(phase == 1 ? static_cast<QObject *>(slots_[0]->window) : ground_map_,
                      QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        }
        if ((phase == 7 || phase == 9) && local % 3 == 0) {
            const auto anchor = (phase == 7 ? map_plot() : view_plot()).center();
            const auto before = ground_map_->zoom_factor();
            QWheelEvent wheel(anchor, anchor, {}, QPoint(0, local % 60 < 30 ? 120 : -120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            wheel.setAccepted(false);
            QCoreApplication::sendEvent(
                phase == 7 ? static_cast<QObject *>(ground_map_) : slots_[0]->window, &wheel);
            if (phase == 7 && !wheel.isAccepted())
                throw std::runtime_error("Map zoom profile input was outside its active plot");
            if (phase == 7 && ground_map_->zoom_factor() != before)
                ++profile_zoom_changes_;
            if (phase == 7 && local % 30 == 0)
                set_selection(static_cast<std::size_t>(local / 30) % satellites_.size());
        }
        if (phase == 7) {
            profile_zoom_min_ = std::min(profile_zoom_min_, ground_map_->zoom_factor());
            profile_zoom_max_ = std::max(profile_zoom_max_, ground_map_->zoom_factor());
        }
        if (phase == 4 && local == length - 1 &&
            (!profile_pan_moves_ || profile_pan_distance_ <= 1))
            throw std::runtime_error("Map pan profile did not move the viewport");
        if (phase == 7 && local == length - 1 &&
            (!profile_zoom_changes_ || profile_zoom_max_ <= profile_zoom_min_))
            throw std::runtime_error("Map zoom profile did not change its scale");
        // The final idle phase must restore exact-scale quality after the zoom burst.
        // Capture outside a measured interval; the next phase resets the cadence clock.
        if (phase == 8 && local == length - 1) {
            if (!ground_map_->tracks_ready())
                throw std::runtime_error("Map quality did not settle after the zoom profile");
            profile_->capture("2d-settled", ground_map_->grab().toImage());
        }
        ++profile_frame_;
        return true;
    }
    void frame() {
        if (!profile_step())
            return;
        const ProfileScope profile("frame_cpu_ms");
        poll_orbit();
        poll_model();
        poll_project();
        poll_display_cache();
        if (!benchmarking_ && !smoke_dir_.isEmpty())
            scenario_smoke_step();
        if (!benchmarking_ && !scenario_)
            return;
        if (!benchmarking_ && !smoke_dir_.isEmpty() && smoke_bootstrap_ == 3 &&
            frame_count_ == 20) {
            save_workbench_image("workbench-default.png");
            slots_[1]->dock->show();
            open_properties();
        }
        if (!benchmarking_ && !smoke_dir_.isEmpty() && properties_dialog_ &&
            smoke_bootstrap_ == 3) {
            if (frame_count_ == 21 &&
                !properties_dialog_->grab().save(smoke_dir_ + "/object-properties.png"))
                throw std::runtime_error("Cannot save the object properties panel");
            if (frame_count_ == 22)
                properties_dialog_->close();
        }
        if (!smoke_dir_.isEmpty() && !benchmarking_ && editor_)
            orbit_smoke_step();
        if (benchmarking_ && viewer_->windows().empty())
            return;
#ifdef ORBELLE_DESKTOP_BENCHMARK
        const auto frame_begin = run_ ? BenchmarkRun::qpc() : 0;
        const auto phase = run_ ? run_->phase() : BenchmarkRun::Phase::waiting;
        if (run_) {
            if (phase == BenchmarkRun::Phase::drain)
                run_->begin_drain();
            if (timing_presentation_ &&
                (phase != BenchmarkRun::Phase::drain || !run_->drain_expired()))
                timing_presentation_->poll(*run_);
            if (phase == BenchmarkRun::Phase::drain) {
                const bool complete =
                    timing_presentation_ && timing_presentation_->pending_count() == 0;
                if (complete || run_->drain_expired()) {
                    run_->end_drain(complete);
                    finished_ = true;
                    timer_.stop();
                    app_.exit(0);
                }
                // No more images are acquired or submitted while PE feedback drains.
                return;
            }
            if (phase == BenchmarkRun::Phase::inputs)
                run_->start_inputs(this);
            else if (phase != BenchmarkRun::Phase::drain) {
                seconds_ = run_->animation_seconds();
                slider_->blockSignals(true);
                slider_->setValue(static_cast<int>(seconds_));
                slider_->blockSignals(false);
            }
        }
#endif
        if (!requested_time_ && (playing_ || (benchmarking_ && !smoke_dir_.isEmpty()))) {
            seconds_ =
                !smoke_dir_.isEmpty()
                    ? std::fmod(static_cast<double>(frame_count_), duration_seconds())
                    : std::fmod(seconds_ + static_cast<double>(clock_.restart()) *
                                               (benchmarking_ ? 0.01 : playback_rate_ * 0.001),
                                duration_seconds());
            slider_->blockSignals(true);
            slider_->setValue(benchmarking_ ? static_cast<int>(seconds_)
                                            : qRound(10000 * seconds_ / duration_seconds()));
            slider_->blockSignals(false);
        }
        requested_time_ = false;
        const core::Epoch epoch =
            !benchmarking_ && !playing_ && precise_epoch_
                ? *precise_epoch_
                : core::Epoch((benchmarking_ ? 0 : scenario_->first().tai_nanoseconds()) +
                              static_cast<std::int64_t>(std::llround(seconds_ * 1e9)));
        if (track_pixel_ratio_ != devicePixelRatioF()) {
            track_pixel_ratio_ = devicePixelRatioF();
            satellite_state_dirty_ = true;
        }
        if (benchmarking_ || satellite_state_dirty_ || snapshot_.epoch != epoch) {
            snapshot_ = sample_frame(epoch);
            update_satellite();
        }
        request_display_cache();
        if (time_label_ && (time_label_epoch_ != epoch || time_label_scale_ != display_scale_)) {
            time_label_->setText(calendar_text(snapshot_.epoch, display_scale_));
            time_label_epoch_ = epoch;
            time_label_scale_ = display_scale_;
        }
        if (timeline_ && timeline_->current_time() != epoch.tai_nanoseconds() / 1'000'000)
            timeline_->set_current_time(snapshot_.epoch.tai_nanoseconds() / 1'000'000);
        visualization::RenderSnapshot selected_snapshot{
            snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}};
        if (selected_)
            selected_snapshot.markers.push_back(snapshot_.markers.at(*selected_));
        selection_->update(selected_snapshot);
        // The shared scene clock and 2D snapshot continue when no native view is exposed.
        if (!benchmarking_ && std::none_of(slots_.begin(), slots_.end(), [](const auto &slot) {
                return slot->container->isVisible() && slot->window->isExposed();
            })) {
            ++frame_count_;
            return;
        }
        bool active = false;
        // Image acquisition can recreate an out-of-date swapchain and wait for the device.
        {
            const ProfileScope acquire("acquire_ms");
            run_exclusive([&] { active = viewer_->advanceToNextFrame(seconds_); });
        }
        if (!active)
            throw std::runtime_error("VSG viewer became inactive");
        {
            const ProfileScope events("handle_events_ms");
            viewer_->handleEvents();
        }
        // RenderGraph normally adjusts the camera during recording, which is
        // too late for this frame's CPU label/pick projection. Resize once here.
        for (const auto &slot : slots_) {
            if (!slot->render_graph)
                continue;
            const auto extent = slot->render_graph->getExtent();
            const auto previous = slot->render_graph->previous_extent;
            if (extent.width != previous.width || extent.height != previous.height)
                run_exclusive([&] { slot->render_graph->resized(); });
        }
        if (!benchmarking_ && (frame_count_ == 0 || fit_pending_)) {
            fit_views();
            fit_pending_ = false;
        }
        {
            const ProfileScope update("viewer_update_ms");
            viewer_->update();
        }
        {
            const ProfileScope projection("projection_ms");
            for (const auto &slot : slots_) {
                if (!slot->graph)
                    continue;
                const auto extent = slot->window->windowAdapter->extent2D();
#ifdef ORBELLE_DESKTOP_BENCHMARK
                if (run_ && (extent.width != 1280 || extent.height != 720 ||
                             !slot->window->isExposed() || !slot->container->isVisible()))
                    throw std::runtime_error("Timing requires two exposed 1280x720 views");
#endif
                slot->pick_candidates = render_vsg::visible_markers(
                    snapshot_, *slot->camera, extent.width, extent.height, 0,
                    snapshot_.frame == core::FrameId::gcrf ? &*display_to_itrf_ : nullptr);
                if (slot->station_markers) {
                    visualization::RenderSnapshot visible_stations{
                        snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}};
                    auto selected_station = visible_stations;
                    for (const auto &point : slot->pick_candidates) {
                        if (point.index < satellites_.size())
                            continue;
                        const auto &marker = snapshot_.markers.at(point.index);
                        visible_stations.markers.push_back(marker);
                        if (selected_marker_index() == point.index)
                            selected_station.markers.push_back(marker);
                    }
                    slot->station_markers->update(visible_stations);
                    slot->station_selection->update(selected_station);
                }
                if (slot->labels) {
                    const auto eligible = render_vsg::visible_markers(
                        snapshot_, *slot->camera, extent.width, extent.height, 80);
                    slot->labels->update(snapshot_, eligible, *slot->camera, extent.height);
                    if (slot->labels->size() != 200)
                        throw std::runtime_error(
                            "The fixed benchmark requires 200 eligible labels per view");
                }
            }
        }
        const bool capture =
            !smoke_dir_.isEmpty() &&
            (frame_count_ == 19 || frame_count_ == 189 ||
             (benchmarking_ &&
              (frame_count_ == 30 || frame_count_ == 160 || frame_count_ == 180)) ||
             (!benchmarking_ &&
              (smoke_bootstrap_ == 1 || orbit_capture_ ||
               (orbit_smoke_stage_ == orbit_smoke_complete && frame_count_ >= 189))));
#ifdef ORBELLE_DESKTOP_BENCHMARK
        const auto update_qpc = run_ ? BenchmarkRun::qpc() : 0;
#endif
        if (capture)
            for (const auto &slot : slots_)
                if (slot->capture && slot->window->isExposed())
                    slot->capture->request();
        // VSG's convenience wrappers discard VkResult. This GUI-thread loop checks them.
        {
            const ProfileScope submit("submit_ms");
            for (const auto &task : viewer_->recordAndSubmitTasks)
                for (const auto &graph : task->commandGraphs)
                    graph->reset();
            for (const auto &task : viewer_->recordAndSubmitTasks) {
#ifdef ORBELLE_DESKTOP_BENCHMARK
                if (run_)
                    for (const auto &slot : slots_)
                        if (std::find(task->commandGraphs.begin(), task->commandGraphs.end(),
                                      slot->graph) != task->commandGraphs.end())
                            slot->gpu->prepare(frame_count_, *task);
#endif
                const auto result =
                    task->submit(vsg::ref_ptr<vsg::FrameStamp>(viewer_->getFrameStamp()));
                if (result != VK_SUCCESS)
                    throw std::runtime_error("Vulkan submission failed: " + std::to_string(result));
#ifdef ORBELLE_DESKTOP_BENCHMARK
                if (run_)
                    for (const auto &slot : slots_)
                        if (std::find(task->commandGraphs.begin(), task->commandGraphs.end(),
                                      slot->graph) != task->commandGraphs.end())
                            slot->gpu->submitted();
#endif
                if (capture)
                    for (const auto &slot : slots_)
                        if (slot->window->isExposed() &&
                            std::find(task->commandGraphs.begin(), task->commandGraphs.end(),
                                      slot->graph) != task->commandGraphs.end()) {
                            const auto pixels = slot->capture->read_after_submit(*task->fence());
                            slot->captured_image =
                                QImage(pixels.rgba.data(), static_cast<int>(pixels.width),
                                       static_cast<int>(pixels.height), QImage::Format_RGBA8888)
                                    .copy();
                        }
            }
        }
#ifdef ORBELLE_DESKTOP_BENCHMARK
        const auto submit_qpc = run_ ? BenchmarkRun::qpc() : 0;
#endif
        {
            const ProfileScope present("present_ms");
            for (const auto &presentation : viewer_->presentations) {
                VkResult result = VK_SUCCESS;
#ifdef ORBELLE_DESKTOP_BENCHMARK
                if (run_) {
                    if (viewer_->presentations.size() != 1 || presentation->windows.size() != 2)
                        throw std::runtime_error(
                            "Timing expects one shared-device dual-view presentation batch");
                    if (!timing_presentation_) {
                        timing_presentation_ = std::make_unique<BenchmarkPresentation>(
                            std::array<vsg::ref_ptr<vsg::Window>, 2>{
                                slots_[0]->window->windowAdapter, slots_[1]->window->windowAdapter},
                            run_->output_only_probe());
                        timing_metadata_ = timing_presentation_->metadata();
                    }
                    result = timing_presentation_->present(*presentation, frame_count_, *run_);
                    if (result != VK_SUCCESS)
                        throw std::runtime_error(
                            "A timing swapchain changed or failed; this run is invalid");
                } else
#endif
                {
                    result = presentation->present();
                }
                if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR &&
                    result != VK_ERROR_OUT_OF_DATE_KHR)
                    throw std::runtime_error("Vulkan presentation failed: " +
                                             std::to_string(result));
                if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                    for (auto &slot : slots_) {
                        const auto window = slot->window->windowAdapter;
                        if (window && window->visible() &&
                            window->imageIndex() < window->numFrames() &&
                            std::find(presentation->windows.begin(), presentation->windows.end(),
                                      window) != presentation->windows.end())
                            ++slot->presentations;
                    }
                }
                if (result != VK_SUCCESS)
                    run_exclusive([&] {
                        for (const auto &window : presentation->windows)
                            window->resize();
                    });
            }
        }
#ifdef ORBELLE_DESKTOP_BENCHMARK
        if (run_) {
            run_->frame({frame_count_, frame_begin, update_qpc, submit_qpc, BenchmarkRun::qpc(),
                         run_->active_input_id(), epoch.tai_nanoseconds(),
                         selected_ ? static_cast<std::int64_t>(*selected_) : -1, phase});
            if (!timing_ready_ && frame_count_ >= 5) {
                QJsonArray views;
                for (std::size_t i = 0; i < slots_.size(); ++i) {
                    const auto &slot = slots_[i];
                    const auto device = slot->window->windowAdapter->getPhysicalDevice();
                    views.append(
                        QJsonObject{{"view", static_cast<int>(i)},
                                    {"width_pixels", 1280},
                                    {"height_pixels", 720},
                                    {"native_window", QString::number(slot->window->winId(), 16)},
                                    { "gpu",
                                      QString::fromUtf8(device->getProperties().deviceName) }});
                }
                run_->ready(QJsonObject{{"views", views},
                                        {"present_timing_capabilities", timing_capabilities_},
                                        {"presentation_timing", timing_metadata_},
                                        {"gpu_timing",
                                         QJsonObject {
                                             { "views",
                                               gpu_metadata_ }
                                         }},
                                        { "benchmark",
                                          benchmark_->report }});
                timing_ready_ = true;
            }
        }
#endif
        ++frame_count_;
        statusBar()->showMessage(benchmarking_ ? ui::text("graphics_workbench.label_104")
                                 : satellites_.empty() && stations_.empty()
                                     ? ui::text("graphics_workbench.label_105")
                                     : ui::text("graphics_workbench.label_106")
                                           .arg(selected_marker_index()
                                                    ? selected_name()
                                                    : ui::text("graphics_workbench.label_107")));
        if (!smoke_dir_.isEmpty() && (benchmarking_ || smoke_bootstrap_ == 3)) {
            if (frame_count_ == 20) {
                for (std::size_t i = 0; i < slots_.size(); ++i)
                    if (slots_[i]->window->isExposed() &&
                        !slots_[i]->captured_image.save(smoke_dir_ +
                                                        QString("/view-%1-initial.png").arg(i)))
                        throw std::runtime_error("Cannot save initial swapchain image");
            }
            if (frame_count_ == 25)
                resize_to_screen(1320, 820);
            if (frame_count_ == 45)
                slots_[1]->dock->setFloating(true);
            if (frame_count_ == 65)
                slots_[1]->dock->setFloating(false);
            if (frame_count_ == 85)
                slots_[1]->dock->hide();
            if (frame_count_ == 100)
                slots_[1]->dock->show();
            if (frame_count_ == 115)
                recreate_window(*slots_[0]);
            if (frame_count_ == 145)
                recreate_window(*slots_[1]);
            if (frame_count_ >= 190 &&
                (benchmarking_ || !editor_ || orbit_smoke_stage_ == orbit_smoke_complete))
                finish_smoke();
            else if (benchmarking_)
                benchmark_smoke_step();
        }
    }
    void inject_click(Slot &slot, const QPointF &pixels) {
        const auto local = pixels / slot.window->devicePixelRatio();
        const auto global = slot.window->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        injecting_input_ = true;
        QCoreApplication::sendEvent(slot.window, &press);
        QCoreApplication::sendEvent(slot.window, &release);
        injecting_input_ = false;
    }
    void smoke_insert_dialog(std::optional<InsertObjectMethod> method,
                             const QString &screenshot = {}, bool escape = false) {
        timer_.stop();
        insert_action_->trigger();
        auto *dialog = insertion_dialog();
        dialog->select_type(InsertObjectType::satellite);
        auto *methods = dialog->findChild<QTreeWidget *>("insertObjectMethods");
        auto *insert = dialog->findChild<QPushButton *>("insertObject");
        if (!dialog->isVisible() || !methods || !insert)
            throw std::runtime_error("Insert objects command did not open the chooser");
        dialog->findChild<QCheckBox *>("keepInsertObjectsOpen")->setChecked(false);
        for (int i = 0; i < methods->topLevelItemCount(); ++i)
            if (methods->topLevelItem(i)->data(0, Qt::UserRole).toInt() ==
                static_cast<int>(method.value_or(InsertObjectMethod::define_properties)))
                methods->setCurrentItem(methods->topLevelItem(i));
        for (int pass = 0; pass < 6; ++pass)
            QApplication::processEvents();
        if (!screenshot.isEmpty() && !dialog->grab().save(smoke_dir_ + "/" + screenshot))
            throw std::runtime_error("Cannot save insert objects chooser");
        if (method) {
            insert->click();
        } else if (escape) {
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(dialog, &key);
        } else {
            dialog->findChild<QPushButton *>("closeInsertObjects")->click();
        }
        timer_.start();
    }
    void scenario_smoke_step() {
        const auto require = [](bool condition, const char *message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        const auto record = [&](const char *kind) {
            interaction_report_.append(QJsonObject{{"kind", kind}, {"passed", true}});
        };
        if (smoke_bootstrap_ == 0) {
            require(!scenario_ && satellites_.empty() && !insert_action_->isEnabled() &&
                        !time_toolbar_->isEnabled(),
                    "Startup must require a scenario before inserting objects");
            require(grab().save(smoke_dir_ + "/welcome.png"), "Cannot save welcome screenshot");
            record("startup-without-scenario");
            EpochEditor epoch_control;
            epoch_control.set_epoch(core::Epoch{1});
            for (auto scale : {CalendarScale::tai, CalendarScale::tt, CalendarScale::utc}) {
                epoch_control.set_scale(scale);
                require(epoch_control.epoch() == core::Epoch{1},
                        "Time scale selection changed the physical epoch");
            }
            const auto clock_provider = runtime::make_builtin_time_provider();
            const auto leap = clock_provider->from_utc({2016, 12, 31, 23, 59, 60, 0});
            epoch_control.set_epoch(core::Epoch{leap.tai_nanoseconds() - 1'000'000'000});
            epoch_control.adjust_seconds(1);
            require(epoch_control.epoch() == leap &&
                        epoch_control.findChild<QLineEdit *>("epochValue")->text() ==
                            "2016-12-31 23:59:60 UTC",
                    "UTC shortcut skipped the leap second");
            epoch_control.findChild<QLineEdit *>("epochValue")->setText("2016-12-31 23:59:59 UTC");
            epoch_control.findChild<QLineEdit *>("epochValue")->setText("2016-12-31 23:59:60 UTC");
            require(epoch_control.epoch() == leap, "UTC leap-second calendar input lost its epoch");
            epoch_control.set_scale(CalendarScale::tt);
            require(epoch_control.findChild<QLineEdit *>("epochValue")->text() ==
                        "2017-01-01 00:01:08.184 TT",
                    "TT calendar offset is incorrect at the leap second");
            epoch_control.set_scale(CalendarScale::utc);
            epoch_control.findChild<QLineEdit *>("epochValue")->setText("2016-12-30 23:59:60 UTC");
            bool rejected_leap = false;
            try {
                (void)epoch_control.epoch();
            } catch (const std::exception &) {
                rejected_leap = true;
            }
            require(rejected_leap, "Invalid UTC second 60 was accepted by the editor");
            epoch_control.findChild<QLineEdit *>("epochValue")
                ->setText("2000-01-01 12:00:32.184000001 TT");
            require(epoch_control.epoch() == core::Epoch{1},
                    "Pasted unified TT epoch lost nanoseconds");
            epoch_control.set_epoch(epoch_control.epoch());
            require(epoch_control.findChild<QLineEdit *>("epochValue")->text() ==
                        "2000-01-01 12:00:32.184000001 TT",
                    "Rebinding a pasted epoch lost its explicit time scale");
            epoch_control.open_picker();
            auto *picker = epoch_control.findChild<QFrame *>("epochPopup");
            require(picker, "Unified epoch picker did not open");
            for (const int scale : {0, 1, 2})
                picker->findChild<QComboBox *>("pickerScale")->setCurrentIndex(scale);
            picker->findChild<QPushButton *>("applyOrbit")->click();
            require(epoch_control.epoch() == core::Epoch{1},
                    "Picker scale changes lost nanoseconds");
            EpochEditor tai_control;
            tai_control.findChild<QLineEdit *>("epochValue")
                ->setText("2250-01-01 00:00:00.000000001 TAI");
            tai_control.set_epoch(tai_control.epoch());
            tai_control.adjust_seconds(1);
            require(tai_control.findChild<QLineEdit *>("epochValue")->text() ==
                        "2250-01-01 00:00:01.000000001 TAI",
                    "Pasted TAI binding/adjustment incorrectly required UTC coverage");
            record("time-scales-calendar-and-leap-shortcuts");
            // Exercise the same modal creation path as Ctrl+N without re-entering frame().
            timer_.stop();
            QTimer::singleShot(0, this, [this] {
                auto *dialog = findChild<QDialog *>("newScenarioDialog");
                if (dialog)
                    dialog->reject();
                else
                    fail("Scenario creation dialog did not open");
            });
            create_scenario();
            require(!scenario_, "Cancelling new scenario created one");
            record("cancel-new-scenario");
            QTimer::singleShot(0, this, [this] {
                auto *dialog = findChild<QDialog *>("newScenarioDialog");
                if (!dialog) {
                    fail("Scenario creation dialog did not open");
                    return;
                }
                auto *start =
                    dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioStart"));
                auto *end =
                    dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioEnd"));
                const auto time = runtime::make_builtin_time_provider();
                const auto today = QDateTime::currentDateTimeUtc().date();
                const auto next_day = today.addDays(1);
                if (start->epoch() !=
                        time->from_utc({today.year(), today.month(), today.day(), 0, 0, 0, 0}) ||
                    end->epoch() != time->from_utc({next_day.year(), next_day.month(),
                                                    next_day.day(), 0, 0, 0, 0})) {
                    fail("Scenario defaults must be UTC today's midnight through tomorrow's "
                         "midnight");
                    dialog->reject();
                    return;
                }
                if (!dialog->grab().save(smoke_dir_ + "/new-scenario-default.png"))
                    fail("Cannot save default scenario panel");
                start->open_picker();
                if (auto *popup = start->findChild<QFrame *>("epochPopup")) {
                    if (!popup->grab().save(smoke_dir_ + "/epoch-picker.png"))
                        fail("Cannot save date picker");
                    popup->close();
                } else
                    fail("Calendar did not open");
                interaction_report_.append(QJsonObject{
                    {"kind", "utc-today-one-day-default-and-popovers"}, {"passed", true}});
                dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioStart"))
                    ->set_epoch(core::Epoch{-1'000'000'000'000});
                dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioEnd"))
                    ->set_epoch(core::Epoch{6'600'000'000'000});
                if (!dialog->grab().save(smoke_dir_ + "/new-scenario.png"))
                    fail("Cannot save creation panel");
                dialog->findChild<QPushButton *>("applyOrbit")->click();
            });
            create_scenario();
            timer_.start();
            require(scenario_->objects().empty() && satellites_.empty() &&
                        insert_action_->isEnabled(),
                    "A new scenario must be empty and enable insertion");
            record("create-empty-scenario");
            smoke_bootstrap_ = 1;
        } else if (smoke_bootstrap_ == 1 && frame_count_ >= 3) {
            save_workbench_image("empty-scenario.png");
            smoke_insert_dialog(std::nullopt, "insert-objects.png");
            require(satellites_.empty() && scenario_->objects().empty() && !busy(),
                    "Closing the insertion chooser created an object");
            record("cancel-insert-object-chooser");
            smoke_insert_dialog(InsertObjectMethod::define_properties);
            require(workspace_->current_document() == properties_dialog_ && satellites_.empty(),
                    "Define properties must open an uncommitted workspace draft");
            record("insert-object-define-properties-in-workspace");
            auto *id = editor_->findChild<QLineEdit *>("objectName");
            require(id, "Missing insertion draft object name");
            id->setText("UncommittedDraft");
            smoke_insert_dialog(std::nullopt, {}, true);
            require(id->text() == "UncommittedDraft" &&
                        workspace_->current_document() == properties_dialog_ && satellites_.empty(),
                    "Escape in the chooser changed the existing properties draft");
            record("escape-insert-chooser-preserves-properties-draft");
            properties_dialog_->close();
            require(satellites_.empty() && scenario_->objects().empty(),
                    "Closing an insertion draft created an object");
            record("close-insert-without-applying");
            smoke_insert_dialog(InsertObjectMethod::define_properties);
            require(id->text() == "UncommittedDraft",
                    "Reopening insertion discarded its unapplied draft");
            editor_->findChild<QPushButton *>("resetOrbit")->click();
            properties_dialog_->close();
            record("reopen-and-explicitly-reset-insertion-draft");
            smoke_insert_dialog(InsertObjectMethod::defaults, "insert-objects-defaults.png");
            require(workspace_->current_document() != properties_dialog_,
                    "Default insertion unnecessarily opened the properties page");
            smoke_bootstrap_ = 2;
        } else if (smoke_bootstrap_ == 2 && !busy()) {
            require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                        scenario_->objects().size() == 1 && selected_ == 0,
                    "First inserted satellite was not published into the scenario");
            record("insert-first-satellite");
            require(orbit_->input.object_id == "Satellite1" &&
                        orbit_->input.initial.epoch == scenario_->first() &&
                        orbit_->input.first == scenario_->first() &&
                        orbit_->input.last == scenario_->last() &&
                        orbit_->input.step_seconds == 60 &&
                        orbit_->input.propagator == OrbitPropagator::two_body &&
                        orbit_->input.initial.position_m == OrbitInput{}.initial.position_m,
                    "Default insertion did not use the scenario and satellite defaults");
            record("insert-object-defaults-inherits-scenario");
            properties_dialog_->close();
            smoke_bootstrap_ = 3;
            frame_count_ = 0;
        }
    }
    static void smoke_require(bool condition, const char *message);
    static int smoke_report_column(const QAbstractItemModel *model, const char *id);
    void smoke_choose_report(const QString &key);
    void smoke_prepare_report_creation(const QString &key, bool all);
    void smoke_edit_covariance(double variance, bool accept);
    void smoke_record(const char *kind);
    bool smoke_caches_ready();
    bool smoke_action_in_workspace(const char *name);
    void smoke_confirm_delete(bool accepted);
    static void smoke_answer_data(bool accepted, const char *name = "dataChangeDialog");
    EpochEditor *smoke_scenario_epoch_control(const char *name);
    void smoke_answer_property(const char *name, QMessageBox::StandardButton choice,
                               bool save_available = false);
    void smoke_orbit_editing();
    void smoke_workspace_views();
    void smoke_display_cache_docking();
    void smoke_project_objects();
    void smoke_data_catalog();
    void smoke_scenario_properties();
    void smoke_ground_stations();
    void smoke_reports();
    void smoke_sensors_navigation();
    void smoke_hpop_apply();
    void smoke_property_drafts();
    void smoke_specialist_reports();
    void smoke_status_feedback();
    void smoke_object_insertion();
    void smoke_sensor_geometry();
    struct SmokeScenario {
        const char *name;
        int first_stage;
        int last_stage;
        void (Workbench::*step)();
    };
    static constexpr std::array smoke_scenarios_ = {
        SmokeScenario{"orbit-editing", 0, 25, &Workbench::smoke_orbit_editing},
        SmokeScenario{"workspace-views", 26, 50, &Workbench::smoke_workspace_views},
        SmokeScenario{"display-cache-docking", 51, 67, &Workbench::smoke_display_cache_docking},
        SmokeScenario{"project-objects", 68, 85, &Workbench::smoke_project_objects},
        SmokeScenario{"data-catalog", 86, 94, &Workbench::smoke_data_catalog},
        SmokeScenario{"scenario-properties", 95, 102, &Workbench::smoke_scenario_properties},
        SmokeScenario{"ground-stations", 103, 120, &Workbench::smoke_ground_stations},
        SmokeScenario{"reports", 121, 137, &Workbench::smoke_reports},
        SmokeScenario{"sensors-navigation", 138, 157, &Workbench::smoke_sensors_navigation},
        SmokeScenario{"hpop-apply", 158, 171, &Workbench::smoke_hpop_apply},
        SmokeScenario{"property-drafts", 172, 177, &Workbench::smoke_property_drafts},
        SmokeScenario{"specialist-reports", 178, 191, &Workbench::smoke_specialist_reports},
        SmokeScenario{"status-feedback", 192, 197, &Workbench::smoke_status_feedback},
        SmokeScenario{"object-insertion", 198, 203, &Workbench::smoke_object_insertion},
        SmokeScenario{"sensor-geometry", 204, 211, &Workbench::smoke_sensor_geometry}};
    const char *orbit_smoke_scenario() const {
        for (const auto &scenario : smoke_scenarios_)
            if (orbit_smoke_stage_ >= scenario.first_stage &&
                orbit_smoke_stage_ <= scenario.last_stage)
                return scenario.name;
        return orbit_smoke_stage_ == orbit_smoke_complete ? "complete" : "unknown";
    }
    void orbit_smoke_step() {
        if (orbit_smoke_step_active_ || smoke_bootstrap_ != 3 || frame_count_ < 24 || busy() ||
            orbit_smoke_stage_ == orbit_smoke_complete)
            return;
        // Menus and modal dialogs run nested event loops; never re-enter a scenario step.
        const QScopedValueRollback step_guard(orbit_smoke_step_active_, true);
        for (const auto &scenario : smoke_scenarios_) {
            if (orbit_smoke_stage_ < scenario.first_stage ||
                orbit_smoke_stage_ > scenario.last_stage)
                continue;
            if (smoke_scenario_first_ != scenario.first_stage) {
                smoke_scenario_first_ = scenario.first_stage;
                smoke_scenario_elapsed_.start();
                std::cout << "smoke scenario: " << scenario.name << std::endl;
            }
            (this->*scenario.step)();
            if (orbit_smoke_stage_ > scenario.last_stage)
                smoke_scenario_report_.append(
                    QJsonObject{{"name", scenario.name},
                                {"first_stage", scenario.first_stage},
                                {"last_stage", scenario.last_stage},
                                {"passed", true},
                                {"elapsed_ms", smoke_scenario_elapsed_.elapsed()}});
            return;
        }
        throw std::logic_error("Unknown workbench smoke stage");
    }
#ifdef ORBELLE_DESKTOP_BENCHMARK
    void benchmark_input(const BenchmarkInputEvent &input) {
        QJsonObject result;
        if (input.seek) {
            const int seconds = 5 + static_cast<int>((input.id * 37 + 23) % 591);
            slider_->setValue(seconds);
            result.insert("target_epoch_ns", static_cast<qint64>(seconds) * 1'000'000'000);
        } else {
            const auto view_index = static_cast<std::size_t>(input.id % 2);
            const auto &slot = *slots_[view_index];
            std::vector<bool> in_other(snapshot_.markers.size());
            for (const auto &point : slots_[1 - view_index]->pick_candidates)
                in_other[point.index] = true;
            std::vector<render_vsg::ProjectedMarker> common;
            for (const auto &point : slot.pick_candidates)
                if (in_other[point.index])
                    common.push_back(point);
            if (common.empty())
                throw std::runtime_error("No shared visible target for timed selection");
            const auto &point = common[(input.id * 131) % common.size()];
            inject_click(*slots_[view_index], {point.x_pixels, point.y_pixels});
            if (!selected_ || *selected_ != point.index)
                throw std::runtime_error("Timed selection did not reach its target");
            result.insert("view", static_cast<int>(view_index));
            result.insert("target_index", static_cast<qint64>(point.index));
            result.insert("target_epoch_ns",
                          static_cast<qint64>(snapshot_.epoch.tai_nanoseconds()));
        }
        run_->input(input, result);
    }
#endif
    void benchmark_smoke_step() {
        if (frame_count_ == 30)
            slider_->setValue(450);
        if (frame_count_ == 31) {
            if (snapshot_.epoch != core::Epoch{450'000'000'000})
                throw std::runtime_error("Cached slider seek did not reach the requested epoch");
            interaction_report_.append(QJsonObject{
                {"kind", "cached-slider-seek"},
                {"target_tai_ns", static_cast<qint64>(450'000'000'000)},
                {"rendered_tai_ns", static_cast<qint64>(snapshot_.epoch.tai_nanoseconds())}});
            save_interaction_images("jump");
        }
        if (frame_count_ == 160 || frame_count_ == 180) {
            const auto view_index = frame_count_ == 160 ? 0u : 1u;
            auto &slot = *slots_[view_index];
            const auto &other = slots_[1u - view_index]->pick_candidates;
            const auto candidate = std::find_if(
                slot.pick_candidates.begin(), slot.pick_candidates.end(), [&](const auto &value) {
                    return std::any_of(other.begin(), other.end(),
                                       [&](const auto &p) { return p.index == value.index; });
                });
            if (candidate == slot.pick_candidates.end())
                throw std::runtime_error("No jointly visible test target for selection");
            expected_selection_ = candidate->index;
            inject_click(slot, {candidate->x_pixels, candidate->y_pixels});
        }
        if (frame_count_ == 161 || frame_count_ == 181) {
            if (selected_ != expected_selection_ || selection_->size() != 1)
                throw std::runtime_error("Synthetic Qt click did not select its visible target");
            interaction_report_.append(QJsonObject{
                {"kind", "synthetic-Qt-click"},
                {"view", frame_count_ == 161 ? 0 : 1},
                {"object_id", QString::fromStdString(snapshot_.markers.at(*selected_).object_id)}});
            save_interaction_images(frame_count_ == 161 ? "selected-view0" : "selected-view1");
        }
        if (frame_count_ == 170)
            inject_click(*slots_[0], {0, 0});
        if (frame_count_ == 171) {
            if (selected_ || selection_->size() != 0)
                throw std::runtime_error("Empty-space click did not clear the selection");
            interaction_report_.append(QJsonObject{{"kind", "clear-selection"}, {"passed", true}});
        }
    }
    void save_interaction_images(const QString &step) {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i]->captured_image.save(smoke_dir_ +
                                                QString("/%1-view-%2.png").arg(step).arg(i)))
                throw std::runtime_error("Cannot save the interaction swapchain image");
    }
    bool view_captures_ready() const {
        for (const auto &slot : slots_) {
            if (!slot->container->isVisible())
                continue;
            const auto extent = slot->window->windowAdapter->extent2D();
            const auto ratio = slot->window->devicePixelRatio();
            if (slot->captured_image.width() != static_cast<int>(extent.width) ||
                slot->captured_image.height() != static_cast<int>(extent.height) ||
                std::abs(extent.width - slot->container->width() * ratio) > 1 ||
                std::abs(extent.height - slot->container->height() * ratio) > 1 ||
                slot->window->size() != slot->container->size())
                return false;
        }
        return true;
    }
    void save_workbench_image(const QString &name) {
        // QWidget does not paint native Vulkan children. Insert same-frame
        // swapchain readbacks into the actual Qt control capture, without restyling.
        auto workbench = grab();
        QPainter painter(&workbench);
        for (const auto &slot : slots_) {
            if (!slot->container->isVisible() || slot->captured_image.isNull())
                continue;
            const auto position = slot->container->mapTo(this, QPoint{});
            painter.drawImage(QRect(position, slot->container->size()), slot->captured_image);
        }
        painter.end();
        if (!workbench.save(smoke_dir_ + "/" + name))
            throw std::runtime_error("Cannot save workbench controls and view capture");
    }
    void recreate_window(Slot &slot) {
        // QWindow::destroy sends SurfaceAboutToBeDestroyed; winId creates a new native surface.
        slot.window->destroy();
        slot.window->initializeWindow();
        slot.window->show();
    }
    void finish_smoke() {
        run_exclusive([&] { viewer_->deviceWaitIdle(); });
        shared_device_ = slots_[0]->window->windowAdapter->getDevice() ==
                         slots_[1]->window->windowAdapter->getDevice();
        shared_scene_ = slots_[0]->view->children.front() == slots_[1]->view->children.front();
        independent_cameras_ = slots_[0]->camera != slots_[1]->camera &&
                               slots_[0]->view->viewID != slots_[1]->view->viewID;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto *window = slots_[i]->window;
            if (slots_[i]->creations < 2 || slots_[i]->destructions < 1 ||
                slots_[i]->presentations < 30)
                throw std::runtime_error("A view did not complete recreation and presentation");
            const auto extent = window->windowAdapter->extent2D();
            if (benchmarking_ &&
                (extent.width != 1280 || extent.height != 720 || markers_->size() != 10000 ||
                 !slots_[i]->labels || slots_[i]->labels->size() != 200))
                throw std::runtime_error(
                    "Actual workload or native viewport differs from the frozen benchmark");
            const auto format = window->windowAdapter->surfaceFormat();
            const auto properties = window->windowAdapter->getPhysicalDevice()->getProperties();
            const auto &image = slots_[i]->captured_image;
            if (image.isNull() || !image.save(smoke_dir_ + QString("/view-%1.png").arg(i))) {
                throw std::runtime_error("Cannot save the application's swapchain image");
            }
            const auto pixels = image.convertToFormat(QImage::Format_RGB32);
            const auto background = pixels.pixelColor(0, 0);
            qint64 foreground = 0, marker = 0, foreground_at_edge = 0, selected_pixels = 0;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x) {
                    const auto color = pixels.pixelColor(x, y);
                    if (std::abs(color.red() - background.red()) +
                            std::abs(color.green() - background.green()) +
                            std::abs(color.blue() - background.blue()) >
                        90) {
                        ++foreground;
                        if (x == 0 || y == 0 || x == pixels.width() - 1 || y == pixels.height() - 1)
                            ++foreground_at_edge;
                    }
                    // The benchmark retains its fixed marker palette.
                    if (benchmarking_ && color.red() > 180 && color.green() > 110 &&
                        color.blue() < std::min(color.red(), color.green()) * 0.65)
                        ++marker;
                    if (color.red() > 180 && color.blue() > 140 && color.green() < 120)
                        ++selected_pixels;
                }
            if (!benchmarking_) {
                // Satellite colors are user-defined. Check the actual projected markers,
                // accepting UNORM and sRGB framebuffer encodings of their configured RGB.
                for (const auto &point : slots_[i]->pick_candidates) {
                    if (point.index >= satellites_.size() || satellites_[point.index].model)
                        continue;
                    const auto expected =
                        QColor::fromRgb(satellites_[point.index].display.line_color);
                    const auto encode = [](double value) {
                        return qRound(255 * (value <= .0031308
                                                 ? value * 12.92
                                                 : 1.055 * std::pow(value, 1 / 2.4) - .055));
                    };
                    const QColor srgb(encode(expected.redF()), encode(expected.greenF()),
                                      encode(expected.blueF()));
                    const auto close = [](QColor a, QColor b) {
                        return std::abs(a.red() - b.red()) < 12 &&
                               std::abs(a.green() - b.green()) < 12 &&
                               std::abs(a.blue() - b.blue()) < 12;
                    };
                    for (int y = std::max(0, qRound(point.y_pixels) - 5);
                         y < std::min(pixels.height(), qRound(point.y_pixels) + 6); ++y)
                        for (int x = std::max(0, qRound(point.x_pixels) - 5);
                             x < std::min(pixels.width(), qRound(point.x_pixels) + 6); ++x)
                            if (close(pixels.pixelColor(x, y), expected) ||
                                close(pixels.pixelColor(x, y), srgb))
                                ++marker;
                }
            }
            if (foreground < static_cast<qint64>(pixels.width()) * pixels.height() / 100 ||
                marker < 6)
                throw std::runtime_error("Rendered image lacks the globe or satellite marker");
            if (foreground_at_edge)
                throw std::runtime_error("The initial camera clips the globe or demo track");
            if (selected_ && selected_pixels < 20)
                throw std::runtime_error(
                    "The selected satellite ring is missing from the rendered view");
            view_report_.append(QJsonObject{
                {"native_creations", static_cast<qint64>(slots_[i]->creations)},
                {"native_destructions", static_cast<qint64>(slots_[i]->destructions)},
                {"successful_presentations", static_cast<qint64>(slots_[i]->presentations)},
                {"capture_source", "acquired swapchain image; copy in render submission"},
                {"capture_width_pixels", pixels.width()},
                {"capture_height_pixels", pixels.height()},
                {"capture_frame", static_cast<qint64>(frame_count_)},
                {"capture_epoch_tai_ns", static_cast<qint64>(snapshot_.epoch.tai_nanoseconds())},
                {"foreground_pixels", foreground},
                {"foreground_pixels_at_edge", foreground_at_edge},
                {"marker_pixels", marker},
                {"selection_pixels", selected_pixels},
                {"submitted_markers", static_cast<qint64>(markers_->size())},
                {"submitted_labels",
                 static_cast<qint64>(slots_[i]->labels ? slots_[i]->labels->size() : 0)},
                {"visible_pick_candidates", static_cast<qint64>(slots_[i]->pick_candidates.size())},
                {"surface_format", static_cast<int>(format.format)},
                {"color_space", static_cast<int>(format.colorSpace)},
                {"vulkan_api_version", static_cast<qint64>(properties.apiVersion)},
                {"vulkan_driver_version", static_cast<qint64>(properties.driverVersion)},
                {"width_pixels", static_cast<qint64>(extent.width)},
                {"height_pixels", static_cast<qint64>(extent.height)},
                {"width_logical", window->width()},
                {"height_logical", window->height()},
                {"device_pixel_ratio", window->devicePixelRatio()},
                {"gpu", QString::fromUtf8(properties.deviceName)}});
        }
        if (!benchmarking_)
            save_workbench_image("workbench.png");
        timer_.stop();
        // Exercise shutdown while the background compiler has no remaining view contexts.
        for (auto &slot : slots_)
            slot->window->destroy();
        all_windows_closed_ = viewer_->windows().empty();
        finished_ = true;
        app_.exit(0);
    }
    QApplication &app_;
    ValidationLog &log_;
    QString smoke_dir_, failure_;
    bool validation_, stopped_ = false, finished_ = false, playing_ = true;
    bool benchmarking_, requested_time_ = false;
    bool satellite_state_dirty_ = true;
    double track_pixel_ratio_ = 0;
    std::optional<core::Epoch> time_label_epoch_;
    CalendarScale time_label_scale_ = CalendarScale::utc;
    bool injecting_input_ = false;
    bool shared_device_ = false, shared_scene_ = false, independent_cameras_ = false;
    bool all_windows_closed_ = false;
    std::shared_ptr<const OrbitScene> orbit_;
    std::optional<application::Scenario> scenario_;
    std::shared_ptr<runtime::OrbitSessionCache> orbit_sessions_ =
        std::make_shared<runtime::OrbitSessionCache>();
    std::shared_ptr<runtime::OrbitSessionCache> pending_orbit_sessions_;
    QString project_path_, pending_project_path_, last_project_outcome_;
    bool project_dirty_ = false;
    application::DataLock project_recorded_data_;
    std::optional<core::TimeDataInfo> standalone_time_data_;
    std::optional<application::AnalysisRecord> analysis_record_;
    std::optional<core::Epoch> precise_epoch_;
    struct ProjectProgress {
        enum class Phase { gravity, ocean, weather, ephemeris, orbits };
        std::atomic<Phase> phase{Phase::gravity};
        std::atomic<std::size_t> index{0};
        std::size_t count = 0;
    };
    std::shared_ptr<ProjectProgress> project_progress_;
    StartupResources startup_resources_;
    std::shared_ptr<std::atomic_bool> project_cancellation_;
    std::future<std::shared_ptr<PreparedProject>> project_candidate_;
    QAction *open_action_ = nullptr, *save_action_ = nullptr, *save_as_action_ = nullptr;
    QAction *rename_action_ = nullptr, *duplicate_action_ = nullptr, *delete_action_ = nullptr;
    QAction *eop_action_ = nullptr;
    ApplicationUpdateUi *application_update_ = nullptr;
    bool update_restart_ = false;
    bool update_handover_pending_ = false, update_handed_over_ = false;
    QAction *report_action_ = nullptr;
    ReportDialog *report_dialog_ = nullptr;
    QPointer<ReportCreationDialog> report_creation_;
    std::vector<application::ReportDefinition> reports_;
    ReportDialog::Ready pending_report_input_;
    std::unique_ptr<EopCatalog> eop_catalog_;
    std::map<std::string, std::shared_ptr<const core::GravityModel>> gravity_;
    std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> ephemerides_;
    std::map<std::string, std::shared_ptr<const core::SpaceWeather>> weather_;
    std::map<std::string, std::shared_ptr<const core::OceanTideModel>> ocean_tides_;
    mutable std::shared_ptr<const core::ITimeProvider> active_time_;
    bool project_edit_ = false, project_orbit_edit_ = false, project_settings_edit_ = false;
    std::optional<std::size_t> pending_project_selection_;
    std::optional<std::string> pending_project_sensor_selection_;
    QPushButton *cancel_project_button_ = nullptr;
    std::vector<Satellite> satellites_;
    std::vector<Station> stations_;
    SensorEditor *sensor_editor_ = nullptr;
    std::vector<application::ProjectSensor> sensors_;
    std::optional<runtime::AccessSession> access_session_;
    std::vector<std::string> access_ids_;
    std::string access_hash_;
    GroundStationEditor *station_editor_ = nullptr;
    std::optional<std::string> station_editor_target_;
    std::optional<std::string> pending_station_apply_, pending_sensor_apply_;
    bool orbit_editor_bound_ = false, station_editor_bound_ = false, sensor_editor_bound_ = false,
         scenario_editor_bound_ = false, sensor_editor_creating_ = false;
    std::optional<std::size_t> selected_station_;
    QTreeWidgetItem *scenario_item_ = nullptr;
    std::optional<std::size_t> editor_target_, pending_orbit_target_;
    std::string pending_orbit_name_;
    bool insert_defaults_pending_ = false;
    std::size_t pending_model_target_ = 0;
    QString pending_model_path_;
    QStackedWidget *workspace_pages_ = nullptr;
    QWidget *view_panel_ = nullptr;
    Workspace *workspace_ = nullptr;
    QWidget *map_panel_ = nullptr;
    std::vector<QToolButton *> view_focus_buttons_;
    QAction *lighting_action_ = nullptr, *tracks_action_ = nullptr;
    std::optional<core::Epoch> sun_epoch_;
    std::array<double, 3> sun_direction_{1, 0, 0};
    core::FrameId display_frame_ = core::FrameId::gcrf;
    std::optional<core::PositionRotation> display_to_itrf_;
    std::vector<QComboBox *> frame_choices_;
    vsg::ref_ptr<vsg::MatrixTransform> earth_transform_;
    QLabel *workspace_label_ = nullptr, *workspace_dot_ = nullptr;
    QMenuBar *menu_bar_ = nullptr;
    QAction *new_action_ = nullptr, *example_action_ = nullptr, *insert_action_ = nullptr;
    std::array<QAction *, 3> quick_insert_actions_{};
    InsertObjectDialog *insert_dialog_ = nullptr;
    bool insertion_pending_ = false;
    QAction *fit_action_ = nullptr, *aux_action_ = nullptr, *map_action_ = nullptr;
    GroundMap *ground_map_ = nullptr;
    QToolBar *time_toolbar_ = nullptr;
    QMenu *insert_menu_ = nullptr;
    int smoke_bootstrap_ = 0;
    std::array<std::size_t, 3> smoke_insert_counts_{};
    static constexpr int orbit_smoke_complete = smoke_scenarios_.back().last_stage + 1;
    int smoke_scenario_first_ = -1;
    QElapsedTimer smoke_scenario_elapsed_;
    QJsonArray smoke_scenario_report_;
    std::shared_ptr<const core::GravityModel> smoke_gravity_;
    std::weak_ptr<const core::GravityModel> smoke_startup_gravity_;
    std::string smoke_gravity_id_ = "apply-audit";
    QElapsedTimer smoke_apply_elapsed_;
    double smoke_navigation_distance_ = 0;
    int smoke_final_frames_ = 0;
    std::vector<std::uint8_t> smoke_project_bytes_;
    application::DataLock smoke_data_lock_;
    std::array<vsg::dvec3, 3> smoke_camera_;
    std::shared_ptr<const OrbitScene> smoke_cache_;
    std::optional<application::Project> smoke_settings_project_;
    vsg::RecordAndSubmitTasks smoke_render_tasks_;
    std::size_t smoke_publications_ = 0;
    core::Epoch smoke_view_epoch_{0};
    std::array<double, 3> smoke_sun_{};
    QImage smoke_map_image_, smoke_globe_image_;
    std::uint64_t smoke_wait_frame_ = 0;
    qint64 smoke_timeline_epoch_ = 0;
    qint64 smoke_timeline_first_ = 0, smoke_timeline_last_ = 0;
    int smoke_workspace_height_ = 0;
    std::future<std::shared_ptr<const OrbitScene>> candidate_;
    std::future<std::shared_ptr<const OrbitScene>> display_candidate_;
    std::shared_ptr<const OrbitScene> display_source_, failed_display_source_;
    DisplaySettings display_settings_, failed_display_settings_;
    std::shared_ptr<std::atomic_bool> display_cancellation_;
    std::size_t display_target_ = 0, display_cache_publications_ = 0;
    QMetaObject::Connection display_status_connection_;
    bool display_status_pending_ = false;
    std::shared_ptr<std::atomic_bool> cancellation_;
    OrbitEditor *editor_ = nullptr;
    QTreeWidgetItem *object_item_ = nullptr;
    QTreeWidget *objects_ = nullptr;
    QDockWidget *object_dock_ = nullptr;
    QStackedWidget *inspector_pages_ = nullptr;
    QAction *properties_action_ = nullptr, *focus_action_ = nullptr, *play_action_ = nullptr;
    QString last_orbit_outcome_;
    std::shared_ptr<ApplyTiming> apply_timing_;
    qint64 apply_timing_sequence_ = 0;
    QString last_model_outcome_;
    std::future<vsg::ref_ptr<vsg::MatrixTransform>> model_candidate_;
    vsg::ref_ptr<vsg::MatrixTransform> model_node_;
    vsg::ref_ptr<vsg::Group> model_group_;
    int model_kind_ = 0, pending_model_kind_ = 0;
    double model_diameter_km_ = 1000, pending_model_diameter_km_ = 1000;
    int orbit_smoke_stage_ = 0;
    bool orbit_smoke_step_active_ = false;
    QSize smoke_responsive_size_;
    bool orbit_capture_ = false, fit_pending_ = false;
    std::shared_ptr<const core::IFrameProvider> frames_;
    std::shared_ptr<const core::IEarthOrientationProvider> eop_, eop_current_;
    QString test_resources_;
    vsg::ref_ptr<vsgQt::Viewer> viewer_;
    vsg::ref_ptr<vsg::Device> shared_device_ref_;
    vsg::ref_ptr<vsg::ResourceHints> hints_;
    vsg::ref_ptr<render_vsg::CompileManager> compiler_;
    std::unique_ptr<render_vsg::MarkerLayer> markers_;
    std::unique_ptr<render_vsg::SensorLayer> sensor_layer_;
    std::map<std::string, visualization::SensorView> sensor_views_;
    std::map<std::string, core::SensorPattern> sensor_view_patterns_;
    std::unique_ptr<render_vsg::MarkerLayer> selection_;
    std::unique_ptr<render_vsg::IEarthDisplay> globe_;
    vsg::ref_ptr<vsg::Group> scene_;
    vsg::ref_ptr<vsg::Group> orbit_group_;
    vsg::ref_ptr<vsg::Switch> orbit_switch_;
    vsg::ref_ptr<vsg::Node> smoke_old_track_;
    std::vector<std::unique_ptr<Slot>> slots_;
    QSlider *slider_ = nullptr;
    MissionTimeline *timeline_ = nullptr;
    QToolButton *time_label_ = nullptr;
    QDialog *properties_dialog_ = nullptr;
    QDialog *scenario_properties_ = nullptr;
    QToolBar *commands_ = nullptr;
    QMenu *scene_menu_ = nullptr, *view_menu_ = nullptr, *time_menu_ = nullptr;
    double playback_rate_ = 20;
    CalendarScale display_scale_ = CalendarScale::utc;
    QTimer timer_;
    std::unique_ptr<WorkbenchProfile> profile_;
    std::future<std::vector<std::shared_ptr<const OrbitScene>>> profile_orbits_;
    int profile_count_ = 24, profile_frame_ = 0;
    bool profile_started_ = false;
    QPointF profile_pan_origin_;
    int profile_pan_moves_ = 0, profile_zoom_changes_ = 0;
    double profile_pan_distance_ = 0, profile_zoom_min_ = 1, profile_zoom_max_ = 1;
    QElapsedTimer clock_;
    QJsonArray view_report_;
    QJsonArray interaction_report_;
    visualization::RenderSnapshot snapshot_{
        core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}};
    std::optional<std::size_t> selected_, expected_selection_;
#ifdef ORBELLE_DESKTOP_BENCHMARK
    std::unique_ptr<BenchmarkScene> benchmark_;
    std::unique_ptr<BenchmarkRun> run_;
    std::unique_ptr<BenchmarkPresentation> timing_presentation_;
    QJsonObject timing_metadata_;
    QJsonArray timing_capabilities_;
    QJsonArray gpu_metadata_;
    bool timing_ready_ = false;
#endif
    std::uint64_t frame_count_ = 0;
    double seconds_ = 0;
    std::array<double, 3> initial_position_{};
};

// The smoke driver shares this translation unit to exercise private integration state
// without exposing test-only APIs. Scenario implementations live with desktop tests.
#include "../../tests/graphics_smoke/data_catalog.inc"
#include "../../tests/graphics_smoke/display_cache_docking.inc"
#include "../../tests/graphics_smoke/ground_stations.inc"
#include "../../tests/graphics_smoke/helpers.inc"
#include "../../tests/graphics_smoke/hpop_apply.inc"
#include "../../tests/graphics_smoke/object_insertion.inc"
#include "../../tests/graphics_smoke/orbit_editing.inc"
#include "../../tests/graphics_smoke/project_objects.inc"
#include "../../tests/graphics_smoke/property_drafts.inc"
#include "../../tests/graphics_smoke/reports.inc"
#include "../../tests/graphics_smoke/scenario_properties.inc"
#include "../../tests/graphics_smoke/sensor_geometry.inc"
#include "../../tests/graphics_smoke/sensors_navigation.inc"
#include "../../tests/graphics_smoke/specialist_reports.inc"
#include "../../tests/graphics_smoke/status_feedback.inc"
#include "../../tests/graphics_smoke/workspace_views.inc"
} // namespace

int run_graphics_workbench(QApplication &app) {
    const auto arguments = app.arguments();
    const auto benchmark_index = arguments.indexOf("--benchmark-scenario");
    if (benchmark_index >= 0 && benchmark_index + 1 >= arguments.size()) {
        std::cerr << "--benchmark-scenario requires the fixed workload manifest\n";
        return 1;
    }
    const auto benchmark_manifest =
        benchmark_index >= 0 ? arguments[benchmark_index + 1] : QString{};
    if (benchmark_manifest.isEmpty())
        apply_workbench_style(app);
    const auto timing_index = arguments.indexOf("--benchmark-run");
    const bool timing_probe = arguments.contains("--benchmark-trace-probe");
    const bool output_only_probe = arguments.contains("--benchmark-output-only-probe");
    if (output_only_probe && !timing_probe) {
        std::cerr << "--benchmark-output-only-probe requires --benchmark-trace-probe\n";
        return 1;
    }
    if (timing_index >= 0 &&
        (timing_index + 1 >= arguments.size() || benchmark_manifest.isEmpty())) {
        std::cerr << "--benchmark-run requires a new output directory and --benchmark-scenario\n";
        return 1;
    }
    const auto timing_dir = timing_index >= 0 ? arguments[timing_index + 1] : QString{};
    const auto portable_smoke_index = arguments.indexOf("--portable-smoke");
    const auto graphics_smoke_index = arguments.indexOf("--graphics-smoke");
    if (portable_smoke_index >= 0 && graphics_smoke_index >= 0) {
        std::cerr << "Select either portable smoke or validation-layer smoke\n";
        return 1;
    }
    const auto smoke_index =
        portable_smoke_index >= 0 ? portable_smoke_index : graphics_smoke_index;
    if (smoke_index >= 0 && smoke_index + 1 >= arguments.size()) {
        std::cerr << "--graphics-smoke requires an output directory\n";
        return 1;
    }
    const QString smoke_dir = smoke_index >= 0 ? arguments[smoke_index + 1] : QString{};
    if ((timing_index >= 0 && smoke_index >= 0) || (timing_probe && timing_index < 0)) {
        std::cerr
            << "Timing and graphics-smoke modes are separate; probe requires --benchmark-run\n";
        return 1;
    }
    const bool validation = graphics_smoke_index >= 0 || arguments.contains("--validation");
    if (validation) {
        std::uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
            return 1;
        std::vector<VkLayerProperties> layers(count);
        if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
            return 1;
        const bool available = std::any_of(layers.begin(), layers.end(), [](const auto &layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (!available) {
            std::cerr << "Vulkan validation layer is required; set VK_LAYER_PATH to its manifest "
                         "directory\n";
            return 1;
        }
    }
    const auto resources_index = arguments.indexOf("--rocky-resources");
    if (resources_index >= 0 && resources_index + 1 >= arguments.size()) {
        std::cerr << "--rocky-resources requires a directory\n";
        return 1;
    }
    const auto resources = resources_index >= 0
                               ? arguments[resources_index + 1]
                               : QCoreApplication::applicationDirPath() + "/resources/rocky";
    for (const auto *shader : {"rocky.terrain.vert", "rocky.terrain.frag"}) {
        if (!QFileInfo(resources + "/shaders/" + shader).isFile()) {
            std::cerr << "Rocky shader resource missing: " << shader << '\n';
            return 1;
        }
    }
    qputenv("ROCKY_FILE_PATH", resources.toUtf8());
    const auto local_resources = QCoreApplication::applicationDirPath() + "/resources";
    if (QFileInfo(local_resources + "/proj/proj.db").isFile()) {
        qputenv("PROJ_DATA", (local_resources + "/proj").toUtf8());
        qputenv("PROJ_NETWORK", "OFF");
    }
    if (QDir(local_resources + "/gdal").exists())
        qputenv("GDAL_DATA", (local_resources + "/gdal").toUtf8());
    ValidationLog log;
    try {
        std::unique_ptr<StartupSplash> splash;
        if (benchmark_manifest.isEmpty()) {
            splash = std::make_unique<StartupSplash>(app.primaryScreen());
            splash->show();
            splash->showMessage(ui::text("graphics_workbench.label_108"));
            app.processEvents(QEventLoop::ExcludeUserInputEvents);
            if (!smoke_dir.isEmpty()) {
                QDir().mkpath(smoke_dir);
                if (!splash->grab().save(smoke_dir + "/startup-splash.png"))
                    throw std::runtime_error("Cannot capture the startup splash");
            }
        }
        Workbench window(app, log, smoke_dir, validation, benchmark_manifest, timing_dir,
                         timing_probe, output_only_probe);
        if (!window.initialize_data(splash.get())) {
            if (splash)
                splash->hide();
            window.shutdown();
            return window.report(0);
        }
        window.show();
        if (splash) {
            splash->finish(&window);
            if (splash->isVisible())
                throw std::runtime_error(
                    "Startup splash did not close after opening the workbench");
            splash.reset();
        }
        window.start_rendering();
        const int code = app.exec();
        window.shutdown();
        return window.report(code);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const vsg::Exception &error) {
        std::cerr << "Graphics initialization failed: " << error.message << " (VkResult "
                  << error.result << ")\n";
        return 1;
    }
}
} // namespace orbelle::desktop
