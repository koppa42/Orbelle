#include "graphics_workbench.hpp"
#include "epoch_editor.hpp"
#include "ground_map.hpp"
#include "mission_timeline.hpp"
#include "orbit_editor.hpp"
#include "orbit_scene.hpp"
#include "quantity_editor.hpp"
#include "satellite_model.hpp"
#include "scenario_dialog.hpp"
#include "workbench_profile.hpp"
#include "workbench_style.hpp"
#include "workspace.hpp"
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
#include "benchmark_gpu.hpp"
#include "benchmark_presentation.hpp"
#include "benchmark_run.hpp"
#include "benchmark_scene.hpp"
#endif
#include <QAction>
#include <QApplication>
#include <QCalendarWidget>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QExposeEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QFormLayout>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTimeZone>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <orbitforge/geo_rocky/globe.hpp>
#include <orbitforge/render_vsg/compile_manager.hpp>
#include <orbitforge/render_vsg/labels.hpp>
#include <orbitforge/render_vsg/marker_layer.hpp>
#include <orbitforge/render_vsg/swapchain_capture.hpp>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <tuple>
#include <vsg/all.h>
#include <vsgQt/Window.h>

namespace orbitforge::desktop {
namespace {
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

class Workbench final : public QMainWindow {
    struct Satellite {
        std::shared_ptr<const OrbitScene> orbit;
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
          eop_(runtime::load_eop_snapshot(
              std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
              "resources/eop/manifest.json")),
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
            eop_history_ = eop_;
            if (!benchmarking_)
                eop_current_ = runtime::load_eop_snapshot(
                    std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                    "resources/eop-current/manifest.json");
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
                scene_menu_ = menuBar()->addMenu(QStringLiteral("场景"));
                insert_menu_ = menuBar()->addMenu(QStringLiteral("插入"));
                view_menu_ = menuBar()->addMenu(QStringLiteral("视图"));
                time_menu_ = menuBar()->addMenu(QStringLiteral("时间"));
                commands_ = addToolBar(QStringLiteral("场景与图形"));
                commands_->setObjectName("commandBar");
                commands_->setMovable(false);
                commands_->setFixedHeight(40);
                commands_->setIconSize(QSize(18, 18));
                commands_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                auto *symbol = new QLabel;
                symbol->setPixmap(
                    workbench_icon(WorkbenchIcon::orbit, QColor("#74b9ee")).pixmap(22, 22));
                commands_->addWidget(symbol);
                auto *name = new QLabel("OrbitForge");
                name->setObjectName("brand");
                commands_->addWidget(name);
                commands_->addSeparator();
                new_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::new_scenario),
                                                   QStringLiteral("新建场景"));
                new_action_->setShortcut(QKeySequence("Ctrl+N"));
                connect(new_action_, &QAction::triggered, this, [this] { create_scenario(); });
                scene_menu_->addAction(new_action_);
                example_action_ = scene_menu_->addAction(QStringLiteral("打开二体示例"));
                connect(example_action_, &QAction::triggered, this, [this] { open_example(); });
                scene_menu_->addSeparator();
                insert_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::satellite),
                                                      QStringLiteral("插入卫星"));
                insert_action_->setShortcut(QKeySequence("Ctrl+Shift+S"));
                connect(insert_action_, &QAction::triggered, this,
                        [this] { begin_insert_satellite(); });
                insert_menu_->addAction(insert_action_);
                auto *space = new QWidget;
                space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                commands_->addWidget(space);
                auto *scene_symbol = new QLabel;
                scene_symbol->setPixmap(workbench_icon(WorkbenchIcon::earth).pixmap(16, 16));
                commands_->addWidget(scene_symbol);
                workspace_label_ = new QLabel(QStringLiteral("未创建场景"));
                workspace_label_->setObjectName("workspaceName");
                workspace_label_->setMaximumWidth(260);
                commands_->addWidget(workspace_label_);
            }
            if (benchmarking_) {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
            setWindowTitle(benchmarking_ ? QStringLiteral("OrbitForge — 万星双视图基准场景")
                                         : QStringLiteral("OrbitForge  |  新建场景"));
            resize_to_screen(1600, 960);
            auto *objects = new QTreeWidget;
            objects_ = objects;
            objects->setHeaderLabel(QStringLiteral("场景"));
            if (!benchmarking_)
                objects->setHeaderHidden(true);
            if (benchmarking_) {
                auto *scenario =
                    new QTreeWidgetItem(objects, {QStringLiteral("固定星座 · 10,000 颗")});
                object_item_ =
                    new QTreeWidgetItem(scenario, {QStringLiteral("100 条轨迹 · 每视图 200 标签")});
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
            auto *object_dock = new QDockWidget(QStringLiteral("对象浏览器"), this);
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
                search->setPlaceholderText(QStringLiteral("搜索对象…"));
                search->addAction(workbench_icon(WorkbenchIcon::search),
                                  QLineEdit::LeadingPosition);
                search->setClearButtonEnabled(true);
                connect(search, &QLineEdit::textChanged, objects, [this](const QString &text) {
                    for (auto &satellite : satellites_)
                        satellite.item->setHidden(
                            !satellite.item->text(0).contains(text, Qt::CaseInsensitive));
                });
                layout->addWidget(search);
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
                properties_dialog_ = new QDialog(this, Qt::Window);
                properties_dialog_->setAttribute(Qt::WA_QuitOnClose, false);
                properties_dialog_->setWindowTitle(QStringLiteral("对象属性"));
                properties_dialog_->resize(
                    980, std::min(660, screen()->availableGeometry().height() - 100));
                properties_dialog_->setMinimumSize(910, 240);
                auto *dialog_layout = new QVBoxLayout(properties_dialog_);
                dialog_layout->setSizeConstraint(QLayout::SetNoConstraint);
                dialog_layout->setContentsMargins(0, 0, 0, 0);
                dialog_layout->setSpacing(0);
                inspector_pages_ = new QStackedWidget;
                inspector_pages_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
                inspector_pages_->addWidget(editor_);
                auto *empty = new QLabel(QStringLiteral("选择卫星"));
                empty->setObjectName("muted");
                empty->setAlignment(Qt::AlignCenter);
                empty->setWordWrap(true);
                inspector_pages_->addWidget(empty);
                dialog_layout->addWidget(inspector_pages_, 1);
                resizeDocks({object_dock}, {224}, Qt::Horizontal);
                editor_->apply = [this](OrbitInput input) { start_orbit(std::move(input)); };
                editor_->close_requested = [this] { properties_dialog_->close(); };
                editor_->display = [this](DisplaySettings settings) {
                    if (!editor_target_ || busy())
                        return;
                    try {
                        auto &satellite = satellites_.at(*editor_target_);
                        (void)display_interval(*satellite.orbit, settings, snapshot_.epoch);
                        satellite.display = settings;
                        update_satellite();
                        statusBar()->showMessage(QStringLiteral("已应用显示设置，将按需更新轨迹"),
                                                 5000);
                    } catch (const std::exception &error) {
                        QMessageBox::information(this, QStringLiteral("显示范围无效"),
                                                 QString::fromUtf8(error.what()));
                    }
                };
                editor_->cancel = [this] {
                    if (cancellation_)
                        cancellation_->store(true);
                };
                editor_->appearance = [this](int kind, const QString &path, double diameter) {
                    start_model(kind, path, diameter);
                };
                auto *properties = new QAction(workbench_icon(WorkbenchIcon::properties),
                                               QStringLiteral("属性"), this);
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
                            set_selection(index);
                            open_properties();
                        });
                scene_menu_->addAction(properties);
                scene_menu_->addSeparator();
                auto *quit = scene_menu_->addAction(QStringLiteral("退出"));
                connect(quit, &QAction::triggered, this, &QWidget::close);
                view_menu_->addAction(object_dock->toggleViewAction());
                show_orbit_status();
            }
            auto *toolbar = new QToolBar(QStringLiteral("时间"), this);
            time_toolbar_ = toolbar;
            addToolBar(benchmarking_ ? Qt::TopToolBarArea : Qt::BottomToolBarArea, toolbar);
            toolbar->setMovable(false);
            toolbar->setIconSize(QSize(18, 18));
            toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_)
                toolbar->setEnabled(false);
#endif
            auto *play =
                new QAction(workbench_icon(playing_ ? WorkbenchIcon::pause : WorkbenchIcon::play),
                            playing_ ? QStringLiteral("暂停") : QStringLiteral("播放"), this);
            play_action_ = play;
            connect(play, &QAction::triggered, this, [this] { set_playing(!playing_); });
            if (benchmarking_) {
                toolbar->addAction(play);
                slider_ = new QSlider(Qt::Horizontal);
                slider_->setRange(0, 600);
                slider_->setMinimumWidth(300);
                toolbar->addWidget(slider_);
            } else {
                toolbar->setObjectName("timelineBar");
                auto *timeline = new QWidget;
                timeline->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                auto *layout = new QVBoxLayout(timeline);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(0);
                auto *controls = new QHBoxLayout;
                controls->setContentsMargins(10, 4, 10, 4);
                controls->setSpacing(4);
                auto *heading = new QLabel(QStringLiteral("时间轴"));
                heading->setObjectName("timelineTitle");
                heading->setMinimumWidth(60);
                controls->addWidget(heading);
                auto *start = new QAction(workbench_icon(WorkbenchIcon::first),
                                          QStringLiteral("分析起点"), this);
                auto *end = new QAction(workbench_icon(WorkbenchIcon::last),
                                        QStringLiteral("分析终点"), this);
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
                for (auto *action : {start, play, end}) {
                    auto *button = new QToolButton;
                    button->setDefaultAction(action);
                    button->setFixedSize(28, 28);
                    button->setIconSize(QSize(18, 18));
                    if (action == play)
                        button->setObjectName("transportPlay");
                    controls->addWidget(button);
                }
                time_menu_->addAction(start);
                time_menu_->addAction(play);
                time_menu_->addAction(end);
                time_menu_->addSeparator();
                auto *jump = time_menu_->addAction(QStringLiteral("跳转到指定时间…"));
                jump->setShortcut(QKeySequence("Ctrl+T"));
                jump->setToolTip(QStringLiteral("跳转到指定时间 · Ctrl+T"));
                connect(jump, &QAction::triggered, this, [this] { choose_time(); });
                auto *speed = new QDoubleSpinBox;
                speed->setButtonSymbols(QAbstractSpinBox::NoButtons);
                speed->setDecimals(1);
                speed->setRange(0.1, 1000);
                speed->setValue(20);
                speed->setSuffix(QStringLiteral(" ×"));
                speed->setFixedWidth(84);
                speed->setToolTip(QStringLiteral("仿真时间相对实际时间的播放倍率"));
                speed->setAccessibleName(QStringLiteral("播放倍率"));
                controls->addWidget(speed);
                connect(speed, &QDoubleSpinBox::valueChanged, this,
                        [this](double value) { playback_rate_ = value; });
                timeline_ = new MissionTimeline;
                slider_ = timeline_;
                time_menu_->addSeparator();
                auto *object_interval = time_menu_->addAction(QStringLiteral("显示所选卫星区间"));
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
                     {std::tuple{WorkbenchIcon::zoom_out, QStringLiteral("缩小时间轴 · −"),
                                 "timelineZoomOut",
                                 std::function<void()>{[this] { timeline_->zoom(0.5); }}},
                      std::tuple{WorkbenchIcon::zoom_in, QStringLiteral("放大时间轴 · +"),
                                 "timelineZoomIn",
                                 std::function<void()>{[this] { timeline_->zoom(2); }}},
                      std::tuple{WorkbenchIcon::focus, QStringLiteral("定位当前时间 · C"),
                                 "timelineCenter",
                                 std::function<void()>{[this] { timeline_->center_current(); }}},
                      std::tuple{WorkbenchIcon::time_fit, QStringLiteral("显示全时段 · F"),
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
                time_label_ = new QToolButton;
                time_label_->setObjectName("timeReadout");
                time_label_->setDefaultAction(jump);
                time_label_->setToolButtonStyle(Qt::ToolButtonTextOnly);
                controls->addWidget(time_label_);
                auto *time_scale = new QComboBox;
                time_scale->setObjectName("timelineScale");
                time_scale->addItems({"UTC", "TAI", "TT"});
                time_scale->setAccessibleName(QStringLiteral("时间轴显示标准"));
                controls->addWidget(time_scale);
                connect(time_scale, &QComboBox::currentIndexChanged, this, [this](int index) {
                    display_scale_ = static_cast<CalendarScale>(index);
                    if (timeline_)
                        timeline_->set_scale(display_scale_);
                });
                auto *expand = time_menu_->addAction(QStringLiteral("展开时间轴"));
                expand->setObjectName("expandTimeline");
                expand->setCheckable(true);
                expand->setChecked(true);
                expand->setShortcut(QKeySequence("Ctrl+J"));
                auto *collapse = new QToolButton;
                collapse->setObjectName("collapseTimeline");
                collapse->setFixedSize(28, 28);
                collapse->setIconSize(QSize(16, 16));
                collapse->setIcon(workbench_icon(WorkbenchIcon::chevron_down));
                collapse->setToolTip(QStringLiteral("收起时间轴 · Ctrl+J"));
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
                    collapse->setToolTip(expanded ? QStringLiteral("收起时间轴 · Ctrl+J")
                                                  : QStringLiteral("展开时间轴 · Ctrl+J"));
                    collapse->setAccessibleName(collapse->toolTip());
                });
                controls->addWidget(collapse);
                layout->addLayout(controls);
                layout->addWidget(timeline_);
                toolbar->addWidget(timeline);
                auto *credit = new QLabel("NASA Blue Marble");
                credit->setObjectName("hint");
                statusBar()->addPermanentWidget(credit);
                show_orbit_status();
            }
            connect(slider_, &QSlider::valueChanged, this, [this](int value) {
                seconds_ = benchmarking_ ? value : duration_seconds() * value / 10000.0;
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
                workspace_->add_view(map_panel_, QStringLiteral("二维视图"),
                                     workbench_icon(WorkbenchIcon::map));
                map_action_ = new QAction(QStringLiteral("二维视图"), this);
                map_action_->setIcon(workbench_icon(WorkbenchIcon::map));
                map_action_->setShortcut(QKeySequence("Ctrl+3"));
                view_menu_->addAction(map_action_);
                connect(map_action_, &QAction::triggered, this,
                        [this] { workspace_->select_view(1); });
                auto *three_d = view_menu_->addAction(workbench_icon(WorkbenchIcon::earth),
                                                      QStringLiteral("三维视图"));
                three_d->setShortcut(QKeySequence("Ctrl+1"));
                connect(three_d, &QAction::triggered, this, [this] { workspace_->select_view(0); });
                fit_views();
                auto *fit = new QAction(workbench_icon(WorkbenchIcon::fit),
                                        QStringLiteral("适配轨道"), this);
                fit_action_ = fit;
                fit->setShortcut(QKeySequence("Ctrl+0"));
                connect(fit, &QAction::triggered, this, [this] { fit_views(); });
                auto *focus = new QAction(workbench_icon(WorkbenchIcon::focus),
                                          QStringLiteral("定位卫星"), this);
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
                                set_selection(index);
                                if (properties_action_)
                                    menu.addAction(properties_action_);
                                menu.addAction(focus);
                            } else {
                                if (item && item == scenario_item_) {
                                    auto *properties =
                                        menu.addAction(workbench_icon(WorkbenchIcon::properties),
                                                       QStringLiteral("属性"));
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
            timer_.start(16);
            if (!smoke_dir_.isEmpty()) {
                QDir().mkpath(smoke_dir_);
                QTimer::singleShot(30000, this, [this] {
                    if (!finished_)
                        fail("Rendering smoke test timed out");
                });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~Workbench() override { shutdown(); }
    void closeEvent(QCloseEvent *event) override {
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
        if (candidate_.valid())
            candidate_.wait();
        if (display_candidate_.valid())
            display_candidate_.wait();
        if (model_candidate_.valid())
            model_candidate_.wait();
        if (profile_orbits_.valid())
            profile_orbits_.wait();
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            slot->gpu.reset();
#endif
            slot->trackball = {};
            slot->labels.reset();
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
                               {"rocky_version", ORBITFORGE_ROCKY_VERSION},
                               {"vsgqt_version", ORBITFORGE_VSGQT_VERSION},
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
            QJsonArray messages;
            {
                std::lock_guard lock(log_.mutex);
                for (const auto &message : log_.messages)
                    messages.append(message);
            }
            report.insert("validation_messages", messages);
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
        vsg::ref_ptr<vsg::Trackball> trackball;
        vsg::ref_ptr<render_vsg::SwapchainCapture> capture;
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        std::shared_ptr<BenchmarkGpu> gpu;
#endif
        std::unique_ptr<render_vsg::LabelLayer> labels;
        std::vector<render_vsg::ProjectedMarker> pick_candidates;
        QImage captured_image;
        std::uint64_t creations = 0, destructions = 0, presentations = 0;
    };
    visualization::RenderSnapshot sample_frame(core::Epoch epoch) const {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (benchmark_)
            return benchmark_->cache->sample(epoch);
#endif
        visualization::RenderSnapshot result{epoch, display_frame_, core::OriginId::earth, {}};
        std::optional<core::PositionRotation> rotation;
        for (const auto &satellite : satellites_) {
            if (cache_covers(*satellite.orbit, epoch, epoch))
                result.markers.push_back((display_frame_ == core::FrameId::gcrf
                                              ? satellite.orbit->inertial_track
                                              : satellite.orbit->track)
                                             .sample(epoch)
                                             .markers.front());
            else {
                auto position =
                    satellite.orbit->session.sample(std::array{epoch}).states().front().position_m;
                if (display_frame_ == core::FrameId::itrf) {
                    if (!rotation)
                        rotation = frames_->gcrf_to_itrf(epoch, eop_->at(epoch).sample);
                    position = rotation->apply_position(position);
                }
                result.markers.push_back({satellite.orbit->input.object_id, position});
            }
        }
        return result;
    }
    double duration_seconds() const {
        return benchmarking_ ? 600
               : scenario_   ? scenario_->last().seconds_since(scenario_->first())
                             : 1;
    }
    bool busy() const { return candidate_.valid() || model_candidate_.valid(); }
    void update_actions() {
        if (benchmarking_)
            return;
        const bool active = scenario_.has_value();
        const bool idle = !busy();
        if (new_action_)
            new_action_->setEnabled(idle);
        if (example_action_)
            example_action_->setEnabled(idle && runtime::two_body_available());
        if (insert_action_)
            insert_action_->setEnabled(active && idle && editor_);
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
                idle && (selected_.has_value() ||
                         (scenario_ && objects_->currentItem() == scenario_item_)));
        if (focus_action_)
            focus_action_->setEnabled(selected_.has_value());
        for (auto *button : view_focus_buttons_)
            button->setEnabled(selected_.has_value());
    }
    std::optional<std::size_t> object_index(const QTreeWidgetItem *item) const {
        for (std::size_t i = 0; i < satellites_.size(); ++i)
            if (satellites_[i].item == item)
                return i;
        return std::nullopt;
    }
    bool confirm_replace_scenario() {
        if (!scenario_ || satellites_.empty())
            return true;
        return QMessageBox::question(
                   this, QStringLiteral("替换当前场景"),
                   QStringLiteral("当前版本尚未支持场景文件保存。继续将关闭当前场景及其对象。"),
                   QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Ok;
    }
    void create_scenario() {
        if (busy())
            return;
        const auto &coverage = eop_current_->data_info();
        auto next = new_scenario_dialog(
            this, coverage.first_epoch, coverage.last_epoch,
            [this](core::Epoch first, core::Epoch last) { (void)data_for_interval(first, last); });
        if (next && confirm_replace_scenario())
            activate_scenario(std::move(*next));
    }
    std::shared_ptr<const core::IEarthOrientationProvider>
    data_for_interval(core::Epoch first, core::Epoch last) const {
        if (last.tai_nanoseconds() <= first.tai_nanoseconds())
            throw std::invalid_argument("结束时间必须晚于开始时间。");
        for (const auto &candidate : {eop_current_, eop_history_}) {
            if (candidate &&
                first.tai_nanoseconds() >= candidate->data_info().first_epoch.tai_nanoseconds() &&
                last.tai_nanoseconds() <= candidate->data_info().last_epoch.tai_nanoseconds())
                return candidate;
        }
        throw std::out_of_range("所选时段超出已安装的地球定向数据覆盖，请选择覆盖范围内的时间。");
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
        satellites_.clear();
        if (ground_map_) {
            ground_map_->set_orbits({});
            ground_map_->reset_view();
        }
        orbit_.reset();
        model_node_ = {};
        editor_target_.reset();
        model_kind_ = 0;
        model_diameter_km_ = 1000;
        const QSignalBlocker block(objects_);
        objects_->clear();
        object_item_ = nullptr;
        scenario_ = std::move(next);
        eop_ = next_eop;
        scenario_item_ = new QTreeWidgetItem(objects_, {QString::fromStdString(scenario_->name())});
        scenario_item_->setIcon(0, workbench_icon(WorkbenchIcon::earth));
        scenario_item_->setToolTip(
            0, QStringLiteral("Scenario · 中心天体 Earth\n%1 — %2 TAI")
                   .arg(display_tai(scenario_->first()), display_tai(scenario_->last())));
        scenario_item_->setExpanded(true);
        workspace_label_->setText(QString::fromStdString(scenario_->name()));
        workspace_label_->setToolTip(workspace_label_->text());
        setWindowTitle(QString::fromStdString(scenario_->name()) + QStringLiteral(" · OrbitForge"));
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
    void show_properties_dialog() { workspace_->open(properties_dialog_); }
    void begin_insert_satellite() {
        if (!scenario_ || !editor_ || busy())
            return;
        if (satellites_.size() >= 10000) {
            QMessageBox::information(this, QStringLiteral("对象数量"),
                                     QStringLiteral("当前场景最多支持 10,000 个卫星显示对象。"));
            return;
        }
        OrbitInput input;
        std::size_t suffix = satellites_.size() + 1;
        do {
            input.object_id = "Satellite" + std::to_string(suffix++);
        } while (std::any_of(satellites_.begin(), satellites_.end(), [&](const auto &satellite) {
            return satellite.orbit->input.object_id == input.object_id;
        }));
        input.first = scenario_->first();
        input.last = scenario_->last();
        input.initial.epoch = scenario_->first();
        editor_target_.reset();
        editor_->set_input(input);
        editor_->set_create_mode(true);
        editor_->set_appearance(0, {}, 1000);
        editor_->set_display({});
        editor_->show_appearance_result({});
        editor_->setToolTip({});
        editor_->show_result({});
        inspector_pages_->setCurrentIndex(0);
        properties_dialog_->setWindowTitle(QStringLiteral("插入卫星 · ") +
                                           QString::fromStdString(scenario_->name()));
        show_properties_dialog();
    }
    void set_playing(bool value) {
        playing_ = value;
        if (play_action_) {
            play_action_->setText(value ? QStringLiteral("暂停") : QStringLiteral("播放"));
            play_action_->setIcon(
                workbench_icon(value ? WorkbenchIcon::pause : WorkbenchIcon::play));
        }
        clock_.restart();
    }
    void seek_epoch(core::Epoch epoch) {
        if (!scenario_ || epoch.tai_nanoseconds() < scenario_->first().tai_nanoseconds() ||
            epoch.tai_nanoseconds() > scenario_->last().tai_nanoseconds())
            throw std::out_of_range("Requested display epoch lies outside the analysis interval");
        seconds_ = epoch.seconds_since(scenario_->first());
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
        dialog.setWindowTitle(QStringLiteral("跳转时间"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *time = new EpochEditor;
        time->set_scale(display_scale_);
        time->set_range(scenario_->first(), scenario_->last());
        time->set_epoch(snapshot_.epoch);
        time->setMinimumWidth(380);
        layout->addWidget(time);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new QPushButton(QStringLiteral("取消"));
        auto *apply = new QPushButton(QStringLiteral("跳转"));
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
                QMessageBox::information(&dialog, QStringLiteral("时间无效"),
                                         QString::fromUtf8(error.what()));
            }
        });
        if (dialog.exec() == QDialog::Accepted) {
            const auto epoch = time->epoch();
            if (scenario_ && epoch.tai_nanoseconds() >= scenario_->first().tai_nanoseconds() &&
                epoch.tai_nanoseconds() <= scenario_->last().tai_nanoseconds())
                seek_epoch(epoch);
            else
                QMessageBox::information(
                    this, QStringLiteral("分析时段已更新"),
                    QStringLiteral("新轨道的分析时段已改变，请重新选择跳转时间。"));
        } else
            set_playing(resume);
    }
    void open_properties() {
        if (!selected_ && scenario_ && objects_->currentItem() == scenario_item_) {
            open_scenario_properties();
            return;
        }
        if (!properties_dialog_ || !selected_ || busy())
            return;
        editor_target_ = selected_;
        const auto &satellite = satellites_.at(*editor_target_);
        editor_->set_input(satellite.orbit->input);
        editor_->set_create_mode(false);
        editor_->set_appearance(satellite.model_kind, satellite.model_path,
                                satellite.model_diameter_km);
        editor_->set_display(satellite.display);
        editor_->show_appearance_result({});
        inspector_pages_->setCurrentIndex(0);
        properties_dialog_->setWindowTitle(
            QString::fromStdString(satellite.orbit->input.object_id) +
            QStringLiteral(" · 对象属性"));
        show_orbit_status();
        show_properties_dialog();
    }
    void open_scenario_properties() {
        if (!scenario_ || busy())
            return;
        if (!scenario_properties_)
            scenario_properties_ = new QDialog(this);
        auto &dialog = *scenario_properties_;
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
        dialog.setWindowTitle(QString::fromStdString(scenario_->name()) +
                              QStringLiteral(" · 场景属性"));
        dialog.setMinimumWidth(620);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(22, 18, 22, 18);
        auto *form = new QFormLayout;
        form->setSpacing(12);
        auto *name = new QLineEdit(QString::fromStdString(scenario_->name()));
        name->setMaximumWidth(380);
        name->setReadOnly(true);
        form->addRow(QStringLiteral("场景名称"), name);
        form->addRow(QStringLiteral("中心天体"), new QLabel(QStringLiteral("Earth / 地球")));
        for (const auto &[label, epoch] :
             {std::pair{QStringLiteral("开始时间"), scenario_->first()},
              std::pair{QStringLiteral("结束时间"), scenario_->last()}}) {
            auto *control = new EpochEditor;
            control->set_epoch(epoch);
            control->set_read_only(true);
            control->setToolTip(QStringLiteral("场景时段在创建后保持固定。"));
            form->addRow(label, control);
        }
        form->addRow(QStringLiteral("对象数量"), new QLabel(QString::number(satellites_.size())));
        form->addRow(
            QStringLiteral("时长"),
            new QLabel(QString::number(scenario_->last().seconds_since(scenario_->first()) / 3600,
                                       'g', 8) +
                       QStringLiteral(" 小时")));
        layout->addLayout(form);
        auto *eop_caption =
            new QLabel(eop_ == eop_current_ ? QStringLiteral("IERS Bulletin A · 含预测值")
                                            : QStringLiteral("IERS C04"));
        eop_caption->setToolTip(QString::fromStdString(eop_->data_info().id));
        form->addRow(QStringLiteral("地球定向数据"), eop_caption);
        auto *close = new QPushButton(QStringLiteral("关闭"));
        layout->addWidget(close, 0, Qt::AlignRight);
        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
        layout->addStretch();
        workspace_->open(&dialog);
    }
    void set_selection(std::optional<std::size_t> selection, bool synchronize_tree = true) {
        selected_ = selection;
        if (benchmarking_)
            return;
        if (ground_map_)
            ground_map_->set_selection(selection);
        if (synchronize_tree && objects_) {
            const QSignalBlocker block(objects_);
            objects_->clearSelection();
            objects_->setCurrentItem(selected_ ? satellites_.at(*selected_).item : nullptr);
            if (selected_)
                objects_->scrollToItem(satellites_.at(*selected_).item);
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
        for (std::size_t i = 0; i < satellites_.size(); ++i) {
            const auto &satellite = satellites_[i];
            const auto &marker = snapshot_.markers.at(i);
            if (cache_covers(*satellite.orbit, snapshot_.epoch, snapshot_.epoch))
                fixed.markers.push_back(
                    satellite.orbit->track.sample(snapshot_.epoch).markers.front());
            else
                fixed.markers.push_back(
                    {marker.object_id, display_frame_ == core::FrameId::gcrf
                                           ? display_to_itrf_->apply_position(marker.position_m)
                                           : marker.position_m});
            const auto [first, last] =
                display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
            satellite.fixed_track->set_interval(first, last);
            satellite.inertial_track->set_interval(first, last);
            satellite.tracks->setAllChildren(false);
            if (satellite.display.track_3d)
                satellite.tracks->setSingleChildOn(display_frame_ == core::FrameId::gcrf ? 0 : 1);
            if (ground_map_)
                ground_map_->set_display(i, satellite.display, first, last);
            if (satellite.model) {
                const auto &r = marker.position_m;
                satellite.model->matrix = vsg::translate(r[0], r[1], r[2]) * earth_rotation *
                                          vsg::scale(satellite.model_diameter_km * 1000);
            } else
                points.markers.push_back(marker);
        }
        markers_->update(points);
        if (ground_map_)
            ground_map_->set_snapshot(fixed);
        globe_->set_illumination(sun_direction_, lighting_action_->isChecked());
        satellite_state_dirty_ = false;
    }
    void start_model(int kind, const QString &path, double diameter_km) {
        if (busy() || !editor_target_)
            return;
        if (kind < 0 || kind > 2 || !std::isfinite(diameter_km) || diameter_km <= 0 ||
            (kind == 2 && path.isEmpty())) {
            editor_->show_appearance_result(QStringLiteral("请选择模型文件和有效的显示尺寸。"),
                                            true);
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
            if (pending_model_target_ == 0) {
                model_node_ = satellite.model;
                model_kind_ = satellite.model_kind;
                model_diameter_km_ = satellite.model_diameter_km;
            }
            last_model_outcome_ = "applied";
            update_satellite();
            editor_->show_appearance_result(QStringLiteral("已应用"));
        } catch (const std::exception &error) {
            editor_->show_appearance_result(
                QStringLiteral("未应用；原外观保留。\n") + QString::fromUtf8(error.what()), true);
            last_model_outcome_ = "rejected";
        } catch (const vsg::Exception &error) {
            editor_->show_appearance_result(QStringLiteral("模型编译失败；原外观保留。\n") +
                                                QString::fromStdString(error.message),
                                            true);
            last_model_outcome_ = "rejected";
        }
        update_actions();
    }
    void show_timeline_status() {
        if (timeline_ && scenario_) {
            const auto first_ms = scenario_->first().tai_nanoseconds() / 1'000'000;
            // The ruler resolves milliseconds; keep a drawable span for a valid
            // sub-millisecond scenario. Seeks still clamp to the exact Epoch bounds.
            const auto last_ms =
                std::max(first_ms + 1, scenario_->last().tai_nanoseconds() / 1'000'000);
            timeline_->set_interval(
                first_ms, last_ms, QString::fromStdString(scenario_->name()),
                selected_
                    ? QString::fromStdString(satellites_.at(*selected_).orbit->input.object_id)
                    : QString{});
        }
    }
    void show_orbit_status() {
        show_timeline_status();
        if (!editor_ || !editor_target_ || busy())
            return;
        editor_->show_result({});
    }
    void start_orbit(OrbitInput input) {
        if (busy() || !scenario_)
            return;
        if (display_cancellation_)
            display_cancellation_->store(true);
        pending_orbit_target_ = editor_target_;
        input.first = scenario_->first();
        input.last = scenario_->last();
        if (editor_target_)
            input.object_id = satellites_.at(*editor_target_).orbit->input.object_id;
        else if (std::any_of(satellites_.begin(), satellites_.end(), [&](const auto &satellite) {
                     return satellite.orbit->input.object_id == input.object_id;
                 })) {
            last_orbit_outcome_ = "rejected";
            editor_->show_result(QStringLiteral("对象 ID 已存在，请使用其他名称。"), true);
            return;
        }
        cancellation_ = std::make_shared<std::atomic_bool>(false);
        last_orbit_outcome_.clear();
        const auto revision =
            editor_target_
                ? satellites_.at(*editor_target_).orbit->session.provenance().input_revision + 1
                : 1;
        const auto settings =
            editor_target_ ? satellites_.at(*editor_target_).display : DisplaySettings{};
        const auto cache = display_cache_interval(input, settings, scenario_->first());
        const auto eop = eop_;
        candidate_ = std::async(std::launch::async, [this, input = std::move(input), revision,
                                                     cache, eop, cancel = cancellation_] {
            auto session = runtime::make_two_body_session(
                input.initial, core::GravitationalParameter{input.mu}, input.object_id, revision);
            return prepare_orbit_scene(input, std::move(session), *frames_, *eop, *cancel, cache);
        });
        editor_->set_busy(true);
        update_actions();
    }
    void poll_orbit() {
        if (!candidate_.valid() ||
            candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        editor_->set_busy(false);
        try {
            auto candidate = candidate_.get();
            if (cancellation_->load())
                throw OrbitCancelled{};
            auto next_scenario = scenario_->with_object(candidate->session);
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
                    0, workbench_icon(WorkbenchIcon::satellite, QColor("#78a4c9")));
                satellite.item->setToolTip(0, QStringLiteral("双击打开对象属性 · 默认显示为点"));
                satellites_.push_back(std::move(satellite));
                scenario_item_->setExpanded(true);
            }
            scenario_ = std::move(next_scenario);
            if (pending_orbit_target_)
                ground_map_->replace_track(index, std::move(ground_tracks.front()));
            else
                ground_map_->append_track(std::move(ground_tracks.front()));
            satellites_[index].fixed_track = std::move(fixed_track);
            satellites_[index].inertial_track = std::move(inertial_track);
            satellites_[index].tracks = track;
            editor_target_ = index;
            editor_->set_create_mode(false);
            properties_dialog_->setWindowTitle(
                QString::fromStdString(satellites_[index].orbit->input.object_id) +
                QStringLiteral(" · 对象属性"));
            orbit_ = satellites_.front().orbit;
            object_item_ = satellites_.front().item;
            snapshot_ = sample_frame(scenario_->first());
            update_satellite();
            set_selection(index);
            selection_->update({snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}});
            for (auto &slot : slots_)
                slot->pick_candidates.clear();
            seconds_ = 0;
            requested_time_ = true;
            const QSignalBlocker block(slider_);
            slider_->setValue(0);
            clock_.restart();
            initial_position_ = snapshot_.markers.front().position_m;
            fit_views();
            fit_pending_ = true;
            show_orbit_status();
            last_orbit_outcome_ = "applied";
        } catch (const OrbitCancelled &) {
            editor_->show_result(QStringLiteral("已取消；继续显示原场景。"));
            last_orbit_outcome_ = "cancelled";
        } catch (const std::exception &error) {
            editor_->show_result(
                QStringLiteral("未应用；原场景保留。\n") + QString::fromUtf8(error.what()), true);
            last_orbit_outcome_ = "rejected";
        } catch (const vsg::Exception &error) {
            editor_->show_result(QStringLiteral("轨迹资源编译失败；原场景已恢复。\n") +
                                     QString::fromStdString(error.message),
                                 true);
            last_orbit_outcome_ = "rejected";
        }
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
            statusBar()->showMessage(QStringLiteral("正在准备当前时段的显示轨迹…"));
            break;
        }
    }
    void poll_display_cache() {
        if (!display_candidate_.valid() ||
            display_candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
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
            satellite_state_dirty_ = true;
            orbit_ = satellites_.front().orbit;
            ++display_cache_publications_;
            statusBar()->showMessage(QStringLiteral("轨迹已更新"), 2000);
        } catch (const OrbitCancelled &) {
        } catch (const std::exception &error) {
            failed_display_source_ = display_source_;
            failed_display_settings_ = display_settings_;
            statusBar()->showMessage(QStringLiteral("显示轨迹未更新：") +
                                     QString::fromUtf8(error.what()));
        } catch (const vsg::Exception &error) {
            failed_display_source_ = display_source_;
            failed_display_settings_ = display_settings_;
            statusBar()->showMessage(QStringLiteral("显示轨迹未更新：") +
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
                           : snapshot_.markers.at(selected_.value_or(0)).position_m;
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
            if (slot.trackball)
                slot.trackball->setViewpoint(vsg::LookAt::create(look->eye, look->center, look->up),
                                             0);
            perspective->nearDistance = 6378.137;
            perspective->farDistance = distance + bound * 20;
        }
    }
    void focus_satellite(std::optional<std::size_t> target = std::nullopt) {
        if (!selected_)
            return;
        const auto &satellite = satellites_.at(*selected_);
        const auto &r = snapshot_.markers.at(*selected_).position_m;
        const vsg::dvec3 centre(r[0], r[1], r[2]);
        const double distance = satellite.model ? satellite.model_diameter_km * 3000 : 1e6;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (target && *target != i)
                continue;
            auto &slot = slots_[i];
            auto *look = dynamic_cast<vsg::LookAt *>(slot->camera->viewMatrix.get());
            const auto direction = vsg::normalize(look->eye - centre);
            const auto viewpoint =
                vsg::LookAt::create(centre + direction * distance, centre, look->up);
            if (slot->trackball)
                slot->trackball->setViewpoint(viewpoint, 0);
            else {
                look->eye = viewpoint->eye;
                look->center = centre;
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
        auto *look = dynamic_cast<vsg::LookAt *>(slot.camera->viewMatrix.get());
        const auto offset = look->eye - look->center;
        const auto distance =
            std::clamp(vsg::length(offset) * factor,
                       vsg::length(look->center) < 1 ? 6378137.0 * 1.02 : 1.0, 1e11);
        slot.trackball->setViewpoint(
            vsg::LookAt::create(look->center + vsg::normalize(offset) * distance, look->center,
                                look->up),
            0);
        auto *projection = dynamic_cast<vsg::Perspective *>(slot.camera->projectionMatrix.get());
        projection->farDistance = std::max(projection->farDistance, distance * 3);
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
            if (slot->trackball)
                slot->trackball->setViewpoint(viewpoint, 0);
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
    QWidget *make_view_controls(std::optional<std::size_t> slot) {
        auto *header = new QWidget;
        header->setObjectName("viewHeading");
        header->setFixedHeight(34);
        auto *bar = new QHBoxLayout(header);
        bar->setContentsMargins(10, 3, 6, 3);
        bar->setSpacing(3);
        auto *title = new QLabel(slot ? QStringLiteral("地球") : QStringLiteral("地面 · WGS84"));
        title->setObjectName("viewTitle");
        bar->addWidget(title);
        if (slot) {
            auto *choice = new QComboBox;
            choice->setObjectName("viewFrame");
            choice->setAccessibleName(QStringLiteral("三维参考系"));
            choice->addItems({QStringLiteral("GCRF · 惯性"), QStringLiteral("ITRF · 地固")});
            choice->setToolTip(QStringLiteral("三维轨迹参考系；二维始终使用 ITRF"));
            choice->setCurrentIndex(display_frame_ == core::FrameId::gcrf ? 0 : 1);
            connect(choice, &QComboBox::currentIndexChanged, this,
                    [this](int index) { change_display_frame(index); });
            frame_choices_.push_back(choice);
            bar->addWidget(choice);
        }
        bar->addSpacing(10);
        const auto button = [&](WorkbenchIcon icon, const QString &text, auto command) {
            auto *control = new QToolButton;
            control->setIcon(workbench_icon(icon));
            control->setIconSize(QSize(17, 17));
            control->setFixedSize(28, 28);
            control->setToolTip(text);
            control->setAccessibleName(text);
            connect(control, &QToolButton::clicked, this, command);
            bar->addWidget(control);
            return control;
        };
        button(WorkbenchIcon::fit, slot ? QStringLiteral("适配轨道") : QStringLiteral("显示全球"),
               [this, slot] {
                   if (slot)
                       fit_views(slot);
                   else
                       ground_map_->reset_view();
               });
        view_focus_buttons_.push_back(
            button(WorkbenchIcon::focus, QStringLiteral("定位所选卫星"), [this, slot] {
                if (slot)
                    focus_satellite(slot);
                else
                    ground_map_->focus_selection();
            }));
        for (const auto &[icon, factor, text] :
             {std::tuple{WorkbenchIcon::zoom_in, 1.3, QStringLiteral("放大")},
              std::tuple{WorkbenchIcon::zoom_out, 1 / 1.3, QStringLiteral("缩小")}})
            button(icon, text, [this, slot, factor] {
                if (slot)
                    zoom_view(*slot, 1 / factor);
                else
                    ground_map_->zoom(factor, ground_map_->rect().center());
            });
        bar->addStretch();
        auto *settings = new QMenu(header);
        if (!lighting_action_) {
            lighting_action_ = new QAction(QStringLiteral("昼夜光照 · 同步二维 / 三维"), this);
            lighting_action_->setCheckable(true);
            lighting_action_->setChecked(true);
            connect(lighting_action_, &QAction::toggled, this, [this](bool enabled) {
                if (ground_map_)
                    ground_map_->set_lighting(enabled);
                satellite_state_dirty_ = true;
            });
            tracks_action_ = new QAction(QStringLiteral("三维轨迹"), this);
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
                 {std::pair{QStringLiteral("经纬网"), &GroundMap::set_grid_visible},
                  std::pair{QStringLiteral("地面轨迹"), &GroundMap::set_tracks_visible},
                  std::pair{QStringLiteral("对象名称"), &GroundMap::set_labels_visible},
                  std::pair{QStringLiteral("太阳直射点"), &GroundMap::set_sun_visible}}) {
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
        display->setToolTip(QStringLiteral("显示设置"));
        display->setAccessibleName(QStringLiteral("显示设置"));
        display->setFixedSize(30, 28);
        display->setMenu(settings);
        display->setPopupMode(QToolButton::InstantPopup);
        bar->addWidget(display);
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_)
            prepare_present_timing(*traits);
#endif
        if (!smoke_dir_.isEmpty())
            traits->swapchainPreferences.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        traits->device = shared_device_ref_;
        current->window = new EmbeddedWindow(viewer_, traits);
        current->container = QWidget::createWindowContainer(current->window);
        current->container->setMinimumSize(64, 64);
        if (benchmarking_) {
            const auto ratio = current->window->devicePixelRatio();
            const int width = qRound(1280.0 / ratio), height = qRound(720.0 / ratio);
            if (qRound(width * ratio) != 1280 || qRound(height * ratio) != 720)
                throw std::runtime_error("This DPI cannot represent a 1280x720 native viewport; "
                                         "choose a compatible QT_SCALE_FACTOR");
            current->container->setFixedSize(width, height);
        }
        if (secondary) {
            current->dock = new QDockWidget(QStringLiteral("辅助三维视图"), this);
            current->dock->setWidget(current->container);
            addDockWidget(Qt::RightDockWidgetArea, current->dock);
            if (benchmarking_) {
                resizeDocks({current->dock}, {600}, Qt::Horizontal);
                auto *action = menuBar()->addAction(QStringLiteral("显示视图 2"));
                connect(action, &QAction::triggered, current->dock, &QWidget::show);
            } else {
                style_dock(current->dock, WorkbenchIcon::views);
                auto *action = current->dock->toggleViewAction();
                aux_action_ = action;
                action->setIcon(workbench_icon(WorkbenchIcon::views));
                action->setText(QStringLiteral("辅助视图"));
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
                workspace_->add_view(panel, QStringLiteral("三维视图"),
                                     workbench_icon(WorkbenchIcon::earth));
                workspace_pages_ = new QStackedWidget;
                workspace_pages_->addWidget(workspace_);
                auto *welcome = new QWidget;
                welcome->setObjectName("welcome");
                auto *welcome_layout = new QVBoxLayout(welcome);
                welcome_layout->setContentsMargins(48, 40, 48, 40);
                welcome_layout->addStretch();
                auto *mark = new QLabel;
                mark->setPixmap(
                    workbench_icon(WorkbenchIcon::orbit, QColor("#4a80b5")).pixmap(56, 56));
                welcome_layout->addWidget(mark, 0, Qt::AlignHCenter);
                auto *heading = new QLabel(QStringLiteral("从一个场景开始"));
                heading->setObjectName("welcomeTitle");
                heading->setAlignment(Qt::AlignCenter);
                welcome_layout->addWidget(heading);
                welcome_layout->addSpacing(20);
                auto *create = new QPushButton(workbench_icon(WorkbenchIcon::orbit),
                                               QStringLiteral("新建场景"));
                create->setObjectName("applyOrbit");
                create->setMinimumWidth(200);
                connect(create, &QPushButton::clicked, new_action_, &QAction::trigger);
                welcome_layout->addWidget(create, 0, Qt::AlignHCenter);
                auto *example = new QPushButton(QStringLiteral("打开二体示例"));
                example->setMinimumWidth(200);
                connect(example, &QPushButton::clicked, example_action_, &QAction::trigger);
                welcome_layout->addWidget(example, 0, Qt::AlignHCenter);
                welcome_layout->addStretch();
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (slot.gpu)
            slot.graph->addChild(slot.gpu->command(false));
#endif
        if (slot.capture)
            slot.graph->addChild(slot.capture);
        if (!benchmarking_) {
            slot.trackball = vsg::Trackball::create(slot.camera);
            slot.trackball->addWindow(adapter);
            viewer_->addEventHandler(slot.trackball);
        }
        ++slot.creations;
        if (timer_.isActive())
            rebuild();
    }
    void detach(Slot &slot) {
        if (!slot.graph || stopped_)
            return;
        begin_change();
        auto &handlers = viewer_->getEventHandlers();
        handlers.remove(slot.trackball);
        globe_->release_view(*slot.view);
        slot.trackball = {};
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
                    auto session = runtime::make_two_body_session(
                        input.initial, core::GravitationalParameter{input.mu}, input.object_id, 1);
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
                mouse(slots_[0]->window, QEvent::MouseButtonPress, QPointF(300, 250),
                      Qt::LeftButton, Qt::LeftButton);
            if (phase == 4) {
                ground_map_->zoom(2, ground_map_->rect().center());
                mouse(ground_map_, QEvent::MouseButtonPress, QPointF(300, 250), Qt::LeftButton,
                      Qt::LeftButton);
            }
            if (phase == 5)
                ground_map_->reset_view();
        }
        if (phase == 1 || phase == 3 || phase == 4) {
            const QPointF point(300 + 75 * std::sin(local * 0.045),
                                250 + 45 * std::cos(local * 0.045));
            mouse(phase == 1 ? static_cast<QObject *>(slots_[0]->window) : ground_map_,
                  QEvent::MouseMove, point, Qt::NoButton,
                  phase == 3 ? Qt::NoButton : Qt::LeftButton);
            if (local == length - 1 && phase != 3)
                mouse(phase == 1 ? static_cast<QObject *>(slots_[0]->window) : ground_map_,
                      QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        }
        if ((phase == 7 || phase == 9) && local % 3 == 0) {
            QWheelEvent wheel(QPointF(300, 250), QPointF(300, 250), {},
                              QPoint(0, local % 60 < 30 ? 120 : -120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(
                phase == 7 ? static_cast<QObject *>(ground_map_) : slots_[0]->window, &wheel);
            if (phase == 7 && local % 30 == 0)
                set_selection(static_cast<std::size_t>(local / 30) % satellites_.size());
        }
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
        const core::Epoch epoch((benchmarking_ ? 0 : scenario_->first().tai_nanoseconds()) +
                                static_cast<std::int64_t>(std::llround(seconds_ * 1e9)));
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
        viewer_->handleEvents();
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
        viewer_->update();
        for (const auto &slot : slots_) {
            if (!slot->graph)
                continue;
            const auto extent = slot->window->windowAdapter->extent2D();
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_ && (extent.width != 1280 || extent.height != 720 ||
                         !slot->window->isExposed() || !slot->container->isVisible()))
                throw std::runtime_error("Timing requires two exposed 1280x720 views");
#endif
            slot->pick_candidates = render_vsg::visible_markers(
                snapshot_, *slot->camera, extent.width, extent.height, 0,
                snapshot_.frame == core::FrameId::gcrf ? &*display_to_itrf_ : nullptr);
            if (slot->labels) {
                const auto eligible = render_vsg::visible_markers(snapshot_, *slot->camera,
                                                                  extent.width, extent.height, 80);
                slot->labels->update(snapshot_, eligible, *slot->camera, extent.height);
                if (slot->labels->size() != 200)
                    throw std::runtime_error(
                        "The fixed benchmark requires 200 eligible labels per view");
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        const auto update_qpc = run_ ? BenchmarkRun::qpc() : 0;
#endif
        if (capture)
            for (const auto &slot : slots_)
                if (slot->capture && slot->window->isExposed())
                    slot->capture->request();
        // VSG's convenience wrappers discard VkResult. This GUI-thread loop checks them.
        for (const auto &task : viewer_->recordAndSubmitTasks)
            for (const auto &graph : task->commandGraphs)
                graph->reset();
        for (const auto &task : viewer_->recordAndSubmitTasks) {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        const auto submit_qpc = run_ ? BenchmarkRun::qpc() : 0;
#endif
        for (const auto &presentation : viewer_->presentations) {
            VkResult result = VK_SUCCESS;
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_) {
                if (viewer_->presentations.size() != 1 || presentation->windows.size() != 2)
                    throw std::runtime_error(
                        "Timing expects one shared-device dual-view presentation batch");
                if (!timing_presentation_) {
                    timing_presentation_ = std::make_unique<BenchmarkPresentation>(
                        std::array<vsg::ref_ptr<vsg::Window>, 2>{slots_[0]->window->windowAdapter,
                                                                 slots_[1]->window->windowAdapter},
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
                throw std::runtime_error("Vulkan presentation failed: " + std::to_string(result));
            if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                for (auto &slot : slots_) {
                    const auto window = slot->window->windowAdapter;
                    if (window && window->visible() && window->imageIndex() < window->numFrames() &&
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
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
        statusBar()->showMessage(
            benchmarking_ ? QStringLiteral("ITRF · 固定双视图基准场景")
            : satellites_.empty()
                ? QStringLiteral("空场景  ·  使用“插入卫星”添加第一个对象")
                : QStringLiteral("就绪  ·  %1")
                      .arg(selected_
                               ? QString::fromStdString(snapshot_.markers.at(*selected_).object_id)
                               : QStringLiteral("未选择对象")));
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
            begin_insert_satellite();
            properties_dialog_->close();
            require(satellites_.empty() && scenario_->objects().empty(),
                    "Closing an insertion draft created an object");
            record("close-insert-without-applying");
            begin_insert_satellite();
            editor_->submit();
            smoke_bootstrap_ = 2;
        } else if (smoke_bootstrap_ == 2 && !busy()) {
            require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                        scenario_->objects().size() == 1 && selected_ == 0,
                    "First inserted satellite was not published into the scenario");
            record("insert-first-satellite");
            properties_dialog_->close();
            smoke_bootstrap_ = 3;
            frame_count_ = 0;
        }
    }
    void orbit_smoke_step() {
        if (smoke_bootstrap_ != 3 || frame_count_ < 24 || busy() ||
            orbit_smoke_stage_ == orbit_smoke_complete)
            return;
        const auto require = [](bool condition, const char *message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        const auto record = [&](const char *kind) {
            interaction_report_.append(QJsonObject{
                {"kind", kind},
                {"passed", true},
                {"revision",
                 static_cast<qint64>(orbit_->track.samples().provenance().input_revision)},
                {"rendered_tai_ns", QString::number(snapshot_.epoch.tai_nanoseconds())}});
        };
        const auto caches_ready = [&] {
            return std::all_of(satellites_.begin(), satellites_.end(), [&](const auto &satellite) {
                const auto wanted =
                    display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
                return cache_covers(*satellite.orbit, wanted.first, wanted.second);
            });
        };
        const auto action_in_workspace = [&](const char *name) {
            for (auto *button : editor_->findChildren<QPushButton *>(name))
                if (button->isVisible() &&
                    workspace_->rect().contains(button->mapTo(workspace_, button->rect().center())))
                    return true;
            return false;
        };
        auto input = orbit_->input;
        switch (orbit_smoke_stage_) {
        case 0:
            // Start below the south pole to cover the former north-facing camera assumption.
            input.initial.position_m = {0, 0, -7e6};
            input.initial.velocity_mps = {6200, 5300, 0};
            editor_->set_input(input);
            editor_->set_state_mode(0);
            editor_->set_state_mode(1);
            require(editor_->findChild<QComboBox *>("stateRepresentation")->currentIndex() == 1,
                    "Classical element state conversion failed in the editor");
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 1:
            require(last_orbit_outcome_ == "applied" && selected_ == 0 &&
                        orbit_->input.object_id == "Satellite1" &&
                        orbit_->max_radius_m - orbit_->min_radius_m > 1e6,
                    "Edited inclined eccentric orbit was not published");
            record("apply-edited-initial-state");
            require(std::abs(orbit_->input.initial.position_m[0]) < 0.001 &&
                        std::abs(orbit_->input.initial.position_m[1]) < 0.001 &&
                        std::abs(orbit_->input.initial.position_m[2] + 7e6) < 0.001,
                    "Changing state representation changed the initial position beyond 1 mm");
            record("apply-keplerian-orbit");
            smoke_old_track_ = orbit_group_->children.front();
            slider_->setValue(10000);
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 2:
            require(snapshot_.epoch == orbit_->input.last, "Analysis end seek failed");
            record("seek-analysis-end");
            save_interaction_images("orbit-end");
            slider_->setValue(0);
            ++orbit_smoke_stage_;
            break;
        case 3:
            require(snapshot_.epoch == orbit_->input.first, "Analysis start seek failed");
            record("seek-analysis-start");
            save_interaction_images("orbit-start");
            orbit_capture_ = false;
            try {
                activate_scenario(application::Scenario("Invalid", core::Epoch{0},
                                                        core::Epoch{100'000'000'000'000}));
                throw std::runtime_error("Out-of-coverage scenario was accepted");
            } catch (const std::out_of_range &) {
                require(scenario_->name() == "Scenario1" &&
                            orbit_group_->children.front() == smoke_old_track_,
                        "Invalid scenario replaced the live scene");
            }
            record("reject-scenario-eop-coverage");
            input.initial.velocity_mps = {0, 0, 0};
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 4: {
            require(last_orbit_outcome_ == "rejected" &&
                        orbit_->track.samples().provenance().input_revision == 2 &&
                        orbit_group_->children.front() == smoke_old_track_,
                    "Invalid edit replaced the live scene");
            record("reject-invalid-initial-state");
            const auto error_message = editor_->findChild<QLabel *>("orbitResult")->text();
            set_selection(std::nullopt);
            require(editor_->findChild<QLabel *>("orbitResult")->text() == error_message,
                    "Changing selection overwrote the property panel's failed update result");
            set_selection(0);
            record("selection-preserves-property-error");
            input.step_seconds = 1;
            editor_->set_input(input);
            editor_->submit();
            cancellation_->store(true);
            orbit_smoke_stage_ = 6;
            break;
        }
        case 6:
            require(last_orbit_outcome_ == "cancelled" &&
                        orbit_->track.samples().provenance().input_revision == 2 &&
                        orbit_group_->children.front() == smoke_old_track_,
                    "Cancelled computation replaced the live scene");
            record("cancel-candidate");
            smoke_old_track_ = {};
            for (auto &slot : slots_) {
                slot->window->hide();
                slot->window->destroy();
            }
            require(viewer_->windows().empty(), "Views remained after destroying both windows");
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 7:
            require(last_orbit_outcome_ == "applied" && viewer_->windows().empty() &&
                        orbit_->track.samples().provenance().input_revision == 3,
                    "Orbit could not be applied with all native windows closed");
            record("apply-without-windows");
            for (auto &slot : slots_) {
                slot->window->initializeWindow();
                slot->window->show();
            }
            fit_pending_ = true;
            ++orbit_smoke_stage_;
            break;
        case 8:
            if (slots_[0]->pick_candidates.empty() || slots_[1]->pick_candidates.empty())
                return;
            inject_click(*slots_[0], {slots_[0]->pick_candidates.front().x_pixels,
                                      slots_[0]->pick_candidates.front().y_pixels});
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 9:
            require(selected_ == std::optional<std::size_t>{0} && selection_->size() == 1,
                    "First view could not select the edited satellite");
            record("select-edited-orbit-view0");
            save_interaction_images("orbit-selected-view0");
            inject_click(*slots_[0], {0, 0});
            ++orbit_smoke_stage_;
            break;
        case 10:
            require(!selected_ && selection_->size() == 0, "Edited orbit selection did not clear");
            require(!slots_[1]->pick_candidates.empty(), "Edited orbit disappeared from view 2");
            record("clear-edited-selection");
            inject_click(*slots_[1], {slots_[1]->pick_candidates.front().x_pixels,
                                      slots_[1]->pick_candidates.front().y_pixels});
            ++orbit_smoke_stage_;
            break;
        case 11:
            require(selected_ == std::optional<std::size_t>{0} && selection_->size() == 1,
                    "Second view could not select the edited satellite");
            record("select-edited-orbit-view1");
            save_interaction_images("orbit-selected-view1");
            orbit_capture_ = false;
            ++orbit_smoke_stage_;
            break;
        case 12:
            editor_->configure_appearance(1, {}, 1000);
            ++orbit_smoke_stage_;
            break;
        case 13:
            require(last_model_outcome_ == "applied" && model_kind_ == 1 && markers_->size() == 0,
                    "Built-in satellite model was not applied");
            record("built-in-satellite-model");
            focus_satellite();
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 14:
            save_interaction_images("satellite-model");
            editor_->configure_appearance(
                2, QCoreApplication::applicationDirPath() + "/resources/models/satellite_demo.gltf",
                1000);
            ++orbit_smoke_stage_;
            break;
        case 15:
            require(last_model_outcome_ == "applied" && model_kind_ == 2,
                    "Local glTF satellite model was not applied");
            record("local-gltf-satellite-model");
            ++orbit_smoke_stage_;
            break;
        case 16:
            save_interaction_images("satellite-gltf");
            editor_->configure_appearance(2, smoke_dir_ + "/absent-model.glb", 1000);
            ++orbit_smoke_stage_;
            break;
        case 17:
            require(last_model_outcome_ == "rejected" && model_kind_ == 2,
                    "Missing model replaced the existing satellite appearance");
            record("reject-missing-model");
            editor_->configure_appearance(0, {}, 1000);
            ++orbit_smoke_stage_;
            break;
        case 18:
            require(last_model_outcome_ == "applied" && model_kind_ == 0 && markers_->size() == 1,
                    "Point fallback was not restored");
            record("restore-point-marker");
            fit_views();
            orbit_capture_ = false;
            ++orbit_smoke_stage_;
            break;
        case 19:
            begin_insert_satellite();
            input.object_id = "Satellite1";
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 20:
            require(last_orbit_outcome_ == "rejected" && satellites_.size() == 1,
                    "Duplicate insertion changed the scene");
            record("reject-duplicate-object-id");
            input.object_id = "Satellite2";
            input.initial.position_m = {1e6, 0, -7e6};
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 21:
            require(
                last_orbit_outcome_ == "applied" && satellites_.size() == 2 &&
                    scenario_->objects().size() == 2 && selected_ == 1 &&
                    orbit_->session.provenance().input_revision == 3 && markers_->size() == 2,
                "Second insertion changed the first satellite or failed to select the new object");
            record("insert-second-satellite");
            input = satellites_[1].orbit->input;
            input.initial.velocity_mps[0] += 10;
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 22:
            require(last_orbit_outcome_ == "applied" &&
                        satellites_[1].orbit->session.provenance().input_revision == 2 &&
                        orbit_->session.provenance().input_revision == 3 && selected_ == 1,
                    "Editing satellite 2 affected satellite 1");
            record("independent-object-properties");
            editor_->configure_appearance(1, {}, 500);
            ++orbit_smoke_stage_;
            break;
        case 23:
            require(last_model_outcome_ == "applied" && satellites_[1].model_kind == 1 &&
                        satellites_[0].model_kind == 0 && markers_->size() == 1,
                    "Appearance was not applied only to the chosen satellite");
            record("independent-object-appearance");
            editor_->configure_appearance(0, {}, 500);
            ++orbit_smoke_stage_;
            break;
        case 24:
            require(last_model_outcome_ == "applied" && markers_->size() == 2,
                    "Second point marker was not restored");
            open_properties();
            input = satellites_[1].orbit->input;
            input.initial.position_m[0] += 1e6;
            editor_->set_input(input);
            properties_dialog_->close();
            require(satellites_[1].orbit->input.initial.position_m[0] !=
                        input.initial.position_m[0],
                    "Closing properties applied a draft");
            record("close-properties-without-applying");
            fit_views();
            set_selection(0);
            seek_epoch(scenario_->first());
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 25:
            save_workbench_image("scenario-two-satellites.png");
            orbit_capture_ = false;
            ++orbit_smoke_stage_;
            break;
        case 26: {
            const auto time = runtime::make_builtin_time_provider();
            const auto today = QDateTime::currentDateTimeUtc().date();
            const auto tomorrow = today.addDays(1);
            activate_scenario(application::Scenario(
                "Today", time->from_utc({today.year(), today.month(), today.day(), 0, 0, 0, 0}),
                time->from_utc({tomorrow.year(), tomorrow.month(), tomorrow.day(), 0, 0, 0, 0})));
            begin_insert_satellite();
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        }
        case 27:
            require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                        eop_ == eop_current_ && orbit_->input.step_seconds == 60 &&
                        orbit_->track.samples().states().size() < 200,
                    "Current UTC one-day scenario failed to propagate with current EOP data");
            record("current-utc-day-orbit-and-predicted-eop");
            require(!commands_->actions().contains(properties_action_),
                    "Properties action remains in the command bar");
            open_scenario_properties();
            require(workspace_->current_document() == scenario_properties_,
                    "Scenario properties did not open in the workspace");
            require(scenario_properties_->grab().save(smoke_dir_ + "/scenario-properties.png"),
                    "Cannot save scenario properties");
            scenario_properties_->close();
            record("scenario-tree-properties");
            open_properties();
            require(properties_dialog_->grab().save(smoke_dir_ + "/current-object-properties.png"),
                    "Cannot save current object properties");
            properties_dialog_->close();
            slots_[1]->dock->hide();
            fit_views();
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 28:
            save_workbench_image("current-day-workbench.png");
            workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
            ground_map_->reset_view();
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 29: {
            require(ground_map_->track_count() == satellites_.size() &&
                        ground_map_->epoch() == snapshot_.epoch,
                    "2D map does not share the published scenario and epoch");
            require(QLineF(ground_coordinates({7e6, 0, 0}), QPointF(0, 0)).length() < 1e-9 &&
                        QLineF(ground_coordinates({0, 7e6, 0}), QPointF(90, 0)).length() < 1e-9 &&
                        std::abs(ground_coordinates({0, 0, 7e6}).y() - 90) < 1e-9,
                    "2D geodetic axes are incorrect");
            const double e2 = 6.6943799901413165e-3;
            const double radius = 6378137.0 / std::sqrt(1 - e2 * 0.5);
            const auto latitude45 =
                ground_coordinates({(radius + 500000) / std::sqrt(2.0), 0,
                                    (radius * (1 - e2) + 500000) / std::sqrt(2.0)});
            require(std::abs(latitude45.y() - 45) < 1e-6,
                    "2D latitude is geocentric instead of WGS84 geodetic");
            const auto seam = ground_path({QPointF(179, 10), QPointF(-179, 12)});
            require(seam.elementCount() == 4 && seam.elementAt(1).x == 180 &&
                        seam.elementAt(2).isMoveTo() && seam.elementAt(2).x == -180 &&
                        std::abs(seam.elementAt(1).y - 11) < 1e-12,
                    "Ground track draws a false segment across the antimeridian");
            const auto exact_seam = ground_path({QPointF(180, 10), QPointF(-180, 10)});
            for (int i = 0; i < exact_seam.elementCount(); ++i)
                require(std::isfinite(exact_seam.elementAt(i).x) &&
                            std::isfinite(exact_seam.elementAt(i).y),
                        "Exact antimeridian produced an invalid path");
            for (const int direction : {-1, 1}) {
                const auto vertical =
                    ground_path({QPointF(direction * 180, 10), QPointF(-direction * 180, 12)});
                require(std::abs(vertical.length() - 2.0) < 1e-12,
                        "Ground track dropped latitude changes along the antimeridian");
            }
            save_workbench_image("workbench-2d-3d.png");
            require(ground_map_->grab().save(smoke_dir_ + "/ground-map.png"), "Cannot save 2D map");
            set_selection(std::nullopt);
            const auto point = ground_map_->marker_position(0);
            const auto global = ground_map_->mapToGlobal(point);
            QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(ground_map_, &press);
            QCoreApplication::sendEvent(ground_map_, &release);
            require(selected_ == 0 && objects_->currentItem() == satellites_[0].item,
                    "Picking a 2D satellite does not select the same scene object");
            record("2d-geodetic-projection-seam-and-selection");
            seek_epoch(scenario_->last());
            ++orbit_smoke_stage_;
            break;
        }
        case 30: {
            require(ground_map_->epoch() == scenario_->last() &&
                        ground_map_->epoch() == snapshot_.epoch &&
                        QLineF(ground_map_->marker_coordinates(0),
                               ground_coordinates(display_to_itrf_->apply_position(
                                   snapshot_.markers[0].position_m)))
                                .length() < 2e-5,
                    "2D/3D epoch or position diverged after a timeline seek");
            const auto center = QPointF(ground_map_->rect().center());
            QWheelEvent wheel(center, ground_map_->mapToGlobal(center), QPoint{}, QPoint{0, 120},
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(ground_map_, &wheel);
            require(ground_map_->zoom_factor() > 1, "2D wheel zoom did not respond");
            workspace_->detach_page(map_panel_);
            map_panel_->window()->resize(780, 480);
            record("2d-3d-shared-time-and-zoom");
            ++orbit_smoke_stage_;
            break;
        }
        case 31:
            require(workspace_->is_floating(map_panel_) && map_panel_->isVisible(),
                    "2D view cannot float");
            ground_map_->reset_view();
            require(map_panel_->window()->grab().save(smoke_dir_ + "/ground-map-floating.png"),
                    "Cannot save floating 2D view");
            workspace_->dock_page(map_panel_);
            workspace_->select_view(0);
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            record("2d-floating-and-dock-restore");
            ++orbit_smoke_stage_;
            break;
        case 32:
            require(ground_map_->epoch() == snapshot_.epoch,
                    "Hidden 2D view lost its current epoch");
            slots_[1]->dock->hide();
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 43'200'123'000'000});
            timeline_->zoom(4);
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 33: {
            if (!view_captures_ready())
                return;
            save_workbench_image("timeline-zoom.png");
            const auto epoch = snapshot_.epoch;
            const auto visible = timeline_->visible_last() - timeline_->visible_first();
            require(visible == 21'600'000, "Timeline zoom did not expose a six-hour interval");
            set_selection(std::nullopt);
            set_selection(0);
            require(timeline_->visible_last() - timeline_->visible_first() == visible &&
                        snapshot_.epoch == epoch,
                    "Changing selected object reset the visible interval or simulation time");
            record("timeline-zoom-and-selection-preserves-range");
            timeline_->zoom(1'000'000);
            const auto plot = timeline_->detail_rect();
            const auto point = QPointF(plot.left() + plot.width() * 0.371, plot.center().y());
            smoke_timeline_epoch_ = timeline_->visible_first() + 371;
            set_playing(true);
            QMouseEvent press(QEvent::MouseButtonPress, point, timeline_->mapToGlobal(point),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, timeline_->mapToGlobal(point),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(timeline_, &press);
            QCoreApplication::sendEvent(timeline_, &release);
            require(!playing_, "Timeline scrubbing did not pause playback");
            ++orbit_smoke_stage_;
            break;
        }
        case 34:
            require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                        timeline_->current_time() == smoke_timeline_epoch_ &&
                        ground_map_->epoch() == snapshot_.epoch,
                    "Zoomed timeline seek lost millisecond precision or 2D/3D synchronization");
            record("timeline-precise-pointer-seek-and-pause");
            findChild<QComboBox *>("timelineScale")->setCurrentIndex(2);
            ++orbit_smoke_stage_;
            break;
        case 35:
            require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                        time_label_->text() == calendar_text(snapshot_.epoch, CalendarScale::tt),
                    "Changing the timeline time standard changed the physical instant");
            record("timeline-scale-preserves-instant");
            require(time_toolbar_->grab().save(smoke_dir_ + "/timeline-precision-tt.png"),
                    "Cannot save the precise TT time ruler");
            findChild<QComboBox *>("timelineScale")->setCurrentIndex(0);
            timeline_->reset_view();
            findChild<QAction *>("showTimelineObject")->setChecked(true);
            ++orbit_smoke_stage_;
            break;
        case 36:
            require(time_toolbar_->grab().save(smoke_dir_ + "/timeline-object-expanded.png"),
                    "Cannot save the optional object interval");
            findChild<QAction *>("showTimelineObject")->setChecked(false);
            smoke_timeline_epoch_ =
                scenario_->first().tai_nanoseconds() / 1'000'000 + 13 * 3'600'000 + 1234;
            timer_.stop();
            QTimer::singleShot(0, this, [this] {
                auto *dialog = findChild<QDialog *>("jumpTimeDialog");
                if (!dialog) {
                    fail("Clicking the time readout did not open precise time input");
                    return;
                }
                dialog->findChild<QLineEdit *>("epochValue")
                    ->setText(calendar_text(core::Epoch{smoke_timeline_epoch_ * 1'000'000},
                                            CalendarScale::utc)
                                  .replace("  ", " ") +
                              " UTC");
                dialog->findChild<QPushButton *>("applyOrbit")->click();
            });
            time_label_->click();
            timer_.start();
            ++orbit_smoke_stage_;
            break;
        case 37:
            if (!view_captures_ready())
                return;
            require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                        ground_map_->epoch() == snapshot_.epoch,
                    "Precise readout jump did not update the shared epoch");
            require(timeline_->sizeHint().height() == 118,
                    "Optional satellite row did not collapse");
            record("timeline-readout-jump-and-optional-object-row");
            save_workbench_image("timeline-workbench.png");
            require(time_toolbar_->grab().save(smoke_dir_ + "/timeline.png"),
                    "Cannot save the compact timeline");
            // Restore the known visible marker fixture for the final native-window checks.
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 38:
            // Qt applies dock layout changes on the next event-loop iteration.
            // Wait for matching native geometry and readback before final validation.
            if (!view_captures_ready())
                return;
            slots_[1]->dock->hide();
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 6 * 3600'000'000'000LL});
            smoke_set_view_layout(2);
            ++orbit_smoke_stage_;
            break;
        case 39:
            if (!view_captures_ready() || !caches_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible(),
                    "Parallel view layout did not expose both renderers");
            save_workbench_image("views-parallel.png");
            smoke_view_epoch_ = snapshot_.epoch;
            smoke_sun_ = sun_direction_;
            smoke_set_view_layout(1);
            ++orbit_smoke_stage_;
            break;
        case 40:
            require(ground_map_->isVisible() && !slots_[0]->container->isVisible() &&
                        snapshot_.epoch == smoke_view_epoch_ && selected_ == 0,
                    "2D-only layout lost its content, time or selection");
            save_workbench_image("views-2d.png");
            smoke_map_image_ = ground_map_->grab().toImage();
            lighting_action_->setChecked(false);
            require(ground_map_->grab().toImage() != smoke_map_image_,
                    "The 2D daylight setting did not affect the map");
            lighting_action_->setChecked(true);
            record("view-modes-and-shared-display-settings");
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 12 * 3600'000'000'000LL});
            ++orbit_smoke_stage_;
            break;
        case 41: {
            const auto dot = smoke_sun_[0] * sun_direction_[0] + smoke_sun_[1] * sun_direction_[1] +
                             smoke_sun_[2] * sun_direction_[2];
            require(ground_map_->epoch() == snapshot_.epoch && dot < 0.1 && dot > -0.1,
                    "2D-only seeking failed to rotate Earth-fixed sunlight over six hours");
            require(ground_map_->grab().toImage() != smoke_map_image_,
                    "2D night overlay did not follow the shared scene time");
            smoke_view_epoch_ = snapshot_.epoch;
            set_playing(true);
            ++orbit_smoke_stage_;
            break;
        }
        case 42:
            require(snapshot_.epoch != smoke_view_epoch_ && ground_map_->epoch() == snapshot_.epoch,
                    "Playback stopped when the 3D window was hidden");
            set_playing(false);
            record("2d-only-playback-and-solar-time-synchronization");
            smoke_set_view_layout(0);
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 6 * 3600'000'000'000LL});
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 43:
            if (!view_captures_ready() || !caches_ready())
                return;
            save_workbench_image("views-3d.png");
            smoke_globe_image_ = slots_[0]->captured_image;
            lighting_action_->setChecked(false);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 44:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(slots_[0]->captured_image != smoke_globe_image_,
                    "3D lighting switch did not change the rendered globe");
            record("3d-daylight-toggle-and-restored-native-view");
            lighting_action_->setChecked(true);
            smoke_set_view_layout(2);
            ++orbit_smoke_stage_;
            break;
        case 45:
            if (!view_captures_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible(),
                    "2D/3D pages did not return to split groups");
            record("map-tab-to-split-roundtrip");
            lighting_action_->setChecked(false);
            smoke_set_view_layout(0);
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 46:
            if (!view_captures_ready() || !caches_ready())
                return;
            slots_[1]->dock->hide();
            open_properties();
            require(workspace_->current_document() == properties_dialog_,
                    "Object properties did not open in the central workspace");
            require(editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 60,
                    "The default display step is not 60 seconds");
            editor_->findChild<QuantityEditor *>("displayStep")->set_value(45, Unit::second);
            workspace_->select_view(1);
            ++orbit_smoke_stage_;
            break;
        case 47:
            require(ground_map_->isVisible() && !workspace_->current_document() &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            45,
                    "Switching to a view tab discarded the property draft");
            workspace_->open(properties_dialog_);
            ++orbit_smoke_stage_;
            break;
        case 48: {
            auto *navigation = editor_->findChild<QTreeWidget *>("propertyNavigation");
            require(navigation && navigation->topLevelItemCount() == 3 &&
                        navigation->topLevelItem(0)->childCount() == 2,
                    "The property navigation does not group orbit and range under Basic");
            editor_->findChild<QuantityEditor *>("displayStep")->set_value(60, Unit::second);
            {
                auto *angle = editor_->findChild<QuantityEditor *>("raan");
                angle->set_value(180, Unit::degree);
                auto *selector = angle->findChild<QToolButton *>("quantityUnit");
                QTimer::singleShot(0, selector->menu(), [this, selector, require] {
                    require(selector->menu()->grab().save(smoke_dir_ + "/quantity-unit-menu.png"),
                            "Cannot capture the quantity unit menu");
                    for (auto *action : selector->menu()->actions())
                        if (action->data().toInt() == static_cast<int>(Unit::radian))
                            action->trigger();
                    selector->menu()->close();
                });
                timer_.stop(); // Keep the scripted stage from re-entering the modal menu.
                selector->showMenu();
                timer_.start();
                require(angle->unit() == Unit::radian &&
                            std::abs(angle->value(Unit::degree) - 180) < 1e-12 &&
                            angle->findChild<QLineEdit *>("quantityValue")
                                ->text()
                                .startsWith("3.14159"),
                        "Clickable angle unit did not convert the number");
                save_workbench_image("workspace-quantity-units.png");
                angle->set_unit(Unit::degree);
                // Unit changes are a draft; restore the actual orbit value before later checks.
                angle->set_value(
                    runtime::to_keplerian(orbit_->input.initial,
                                          core::GravitationalParameter{orbit_->input.mu})
                        .raan_rad,
                    Unit::radian);
            }
            save_workbench_image("workspace-properties.png");
            require(action_in_workspace("applyOrbit"),
                    "Orbit Apply button is clipped by the central workspace");
            navigation->topLevelItem(0)->setExpanded(false);
            require(!navigation->topLevelItem(0)->isExpanded(), "Property group cannot collapse");
            navigation->topLevelItem(0)->setExpanded(true);
            navigation->setCurrentItem(navigation->topLevelItem(0)->child(1));
            ++orbit_smoke_stage_;
            break;
        }
        case 49:
            save_workbench_image("workspace-display-settings.png");
            require(action_in_workspace("applyDisplay"),
                    "Display Apply button is clipped by the central workspace");
            workspace_->detach_page(properties_dialog_);
            ++orbit_smoke_stage_;
            break;
        case 50:
            require(workspace_->is_floating(properties_dialog_) &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            60,
                    "Detaching properties changed the field values");
            require(
                properties_dialog_->window()->grab().save(smoke_dir_ + "/properties-detached.png"),
                "Cannot capture detached properties");
            workspace_->dock_page(properties_dialog_);
            require(workspace_->current_document() == properties_dialog_,
                    "Detached properties did not return to the workspace");
            properties_dialog_->close();
            require(isVisible() && !workspace_->contains(properties_dialog_),
                    "Closing properties closed the workbench");
            record("grouped-properties-tabs-draft-and-detach-roundtrip");
            workspace_->select_view(0);
            smoke_cache_ = orbit_;
            smoke_view_epoch_ = snapshot_.epoch;
            require(display_frame_ == core::FrameId::gcrf && snapshot_.frame == core::FrameId::gcrf,
                    "3D did not default to GCRF");
            frame_choices_.front()->setCurrentIndex(1);
            ++orbit_smoke_stage_;
            break;
        case 51: {
            const auto direct =
                orbit_->session.sample(std::array{snapshot_.epoch}).states().front();
            const auto fixed = display_to_itrf_->apply_position(direct.position_m);
            const auto shown = snapshot_.markers.front().position_m;
            require(
                display_frame_ == core::FrameId::itrf && snapshot_.frame == core::FrameId::itrf &&
                    snapshot_.epoch == smoke_view_epoch_ && orbit_ == smoke_cache_ &&
                    std::hypot(fixed[0] - shown[0], fixed[1] - shown[1], fixed[2] - shown[2]) <= 1,
                "Switching the 3D frame changed time/cache or used the wrong coordinates");
            frame_choices_.front()->setCurrentIndex(0);
            const auto inertial = snapshot_.markers.front().position_m;
            require(std::hypot(direct.position_m[0] - inertial[0],
                               direct.position_m[1] - inertial[1],
                               direct.position_m[2] - inertial[2]) <= 1,
                    "Returning to GCRF did not restore the inertial state");
            record("gcrf-itrf-frame-switch-without-propagation");
            const core::Epoch jump{scenario_->first().tai_nanoseconds() + 18 * 3600'000'000'000LL};
            require(!cache_covers(*orbit_, jump, jump), "Seek test unexpectedly already cached");
            const auto immediate = sample_frame(jump).markers.front().position_m;
            const auto exact = orbit_->session.sample(std::array{jump}).states().front().position_m;
            require(immediate == exact,
                    "Uncached time did not query the analysis session directly");
            smoke_publications_ = display_cache_publications_;
            smoke_render_tasks_ = viewer_->recordAndSubmitTasks;
            seek_epoch(jump);
            ++orbit_smoke_stage_;
            break;
        }
        case 52:
            if (!caches_ready())
                return;
            require(display_cache_publications_ > smoke_publications_ && orbit_ != smoke_cache_ &&
                        viewer_->recordAndSubmitTasks == smoke_render_tasks_ &&
                        orbit_->track.samples().states().size() < 200 &&
                        orbit_->session.provenance().input_revision ==
                            smoke_cache_->session.provenance().input_revision,
                    "Seeking did not refill a bounded cache without revising the orbit");
            record("uncached-seek-exact-position-and-asynchronous-period-cache");
            smoke_render_tasks_.clear();
            open_properties();
            editor_->findChild<QComboBox *>("trackExtent")->setCurrentIndex(1);
            editor_->findChild<QPushButton *>("applyDisplay")->click();
            properties_dialog_->close();
            ++orbit_smoke_stage_;
            break;
        case 53:
            if (!caches_ready())
                return;
            require(cache_covers(*orbit_, scenario_->first(), scenario_->last()) &&
                        orbit_->track.samples().states().size() == 1441,
                    "Explicit full-interval request did not prepare the day at 60 seconds");
            record("explicit-full-interval-cache-request");
            smoke_cache_ = orbit_;
            editor_->display({.track_2d = false, .track_3d = false});
            ++orbit_smoke_stage_;
            break;
        case 54:
            require(orbit_ == smoke_cache_ && !display_candidate_.valid(),
                    "Hiding trajectories launched another propagation");
            editor_->display({});
            require(orbit_ == smoke_cache_ && satellites_.front().inertial_track->size() < 160,
                    "One-period display failed to restrict the retained full cache");
            record("display-visibility-and-range-reuse-existing-cache");
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            fit_pending_ = true;
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 55:
            // Complete the frame-driven surface lifecycle checks before measuring layout.
            if (frame_count_ < 150 || frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            slots_[1]->dock->hide();
            open_properties();
            ++orbit_smoke_stage_;
            break;
        case 56:
            smoke_workspace_height_ = workspace_->height();
            smoke_timeline_epoch_ = timeline_->current_time();
            smoke_timeline_first_ = timeline_->visible_first();
            smoke_timeline_last_ = timeline_->visible_last();
            findChild<QToolButton *>("collapseTimeline")->click();
            ++orbit_smoke_stage_;
            break;
        case 57:
            save_workbench_image("workspace-collapsed-timeline.png");
            require(!timeline_->isVisible() && time_label_->isVisible() &&
                        play_action_->isEnabled(),
                    "Collapsing the timeline did not keep the transport controls visible");
            require(workspace_->height() >= smoke_workspace_height_ + 100,
                    qPrintable(QString("Collapsing the timeline did not release workspace "
                                       "height: %1 -> %2")
                                   .arg(smoke_workspace_height_)
                                   .arg(workspace_->height())));
            require(timeline_->current_time() == smoke_timeline_epoch_ &&
                        timeline_->visible_first() == smoke_timeline_first_ &&
                        timeline_->visible_last() == smoke_timeline_last_ &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            60,
                    "Collapsing the timeline changed the instant, range or property draft");
            findChild<QAction *>("expandTimeline")->setChecked(true);
            ++orbit_smoke_stage_;
            break;
        case 58:
            require(timeline_->isVisible() && workspace_->height() == smoke_workspace_height_ &&
                        timeline_->current_time() == smoke_timeline_epoch_ &&
                        timeline_->visible_first() == smoke_timeline_first_ &&
                        timeline_->visible_last() == smoke_timeline_last_,
                    "Restoring the timeline changed its time or browsing range");
            record("timeline-collapse-restores-workspace-and-time-range");
            properties_dialog_->close();
            slots_[1]->dock->show();
            fit_views();
            fit_pending_ = true;
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 59:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            slots_[1]->dock->hide();
            workspace_->select_view(0);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 60: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            smoke_cache_ = orbit_;
            smoke_view_epoch_ = snapshot_.epoch;
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            smoke_camera_ = {look->eye, look->center, look->up};
            require(smoke_tabs(QStringLiteral("三维视图"))->count() == 2,
                    "Workspace still has a special parallel-view tab");
            smoke_drag_page(QStringLiteral("三维视图"),
                            workspace_->mapToGlobal(
                                QPoint(workspace_->width() - 12, workspace_->height() / 2)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 61: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible() &&
                        smoke_tabs(QStringLiteral("三维视图")) !=
                            smoke_tabs(QStringLiteral("二维视图")),
                    "Dragging the native 3D page did not split the workspace");
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            require(orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                        look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                        look->up == smoke_camera_[2],
                    "Dragging 3D changed the camera, current time or orbit cache");
            save_workbench_image("workspace-drag-split.png");
            record("drag-native-3d-tab-to-split-without-resetting-state");
            auto *tabs = smoke_tabs(QStringLiteral("二维视图"));
            smoke_drag_page(QStringLiteral("三维视图"),
                            tabs->mapToGlobal(QPoint(tabs->width() - 8, 16)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 62:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(smoke_tabs(QStringLiteral("三维视图")) ==
                            smoke_tabs(QStringLiteral("二维视图")) &&
                        !ground_map_->isVisible() && slots_[0]->container->isVisible(),
                    "Dragging 3D back to the tab bar did not merge the groups");
            record("drag-native-3d-tab-to-merge");
            smoke_drag_page(QStringLiteral("三维视图"), workspace_->mapToGlobal(QPoint(-80, 100)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 63: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(workspace_->is_floating(view_panel_) && slots_[0]->container->isVisible(),
                    "Dragging 3D out did not create a floating page");
            auto *window = view_panel_->window();
            require(window->width() >= 480 && window->height() >= 320,
                    qPrintable(QString("Floating 3D page is too small: %1 x %2")
                                   .arg(window->width())
                                   .arg(window->height())));
            auto picture = window->grab();
            QPainter painter(&picture);
            painter.drawImage(
                QRect(slots_[0]->container->mapTo(window, QPoint{}), slots_[0]->container->size()),
                slots_[0]->captured_image);
            painter.end();
            require(picture.save(smoke_dir_ + "/workspace-3d-floating.png"),
                    "Cannot capture floating 3D page");
            record("drag-native-3d-tab-to-floating-window");
            workspace_->dock_page(view_panel_);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 64:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(!workspace_->is_floating(view_panel_), "3D page did not dock again");
            workspace_->close_page(view_panel_);
            require(!workspace_->contains(view_panel_), "3D page could not close");
            workspace_->select_view(0);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 65: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            require(workspace_->contains(view_panel_) && slots_[0]->container->isVisible() &&
                        orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                        look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                        look->up == smoke_camera_[2],
                    "Floating/closing/reopening 3D lost the camera, time or cached orbit");
            save_workbench_image("workspace-pages-restored.png");
            record("native-3d-float-dock-close-reopen-preserves-state");
            smoke_drag_page(QStringLiteral("二维视图"),
                            workspace_->mapToGlobal(
                                QPoint(workspace_->width() / 2, workspace_->height() - 12)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 66: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible() &&
                        map_panel_->mapTo(workspace_, QPoint{}).y() >
                            view_panel_->mapTo(workspace_, QPoint{}).y(),
                    "Dragging to the bottom edge did not create a vertical split");
            const auto globe_rect = QRect(slots_[0]->container->mapTo(workspace_, QPoint{}),
                                          slots_[0]->container->size());
            const auto map_tabs = smoke_tabs(QStringLiteral("二维视图"));
            require(globe_rect.bottom() < map_tabs->mapTo(workspace_, QPoint{}).y() &&
                        workspace_->rect().contains(
                            QRect(ground_map_->mapTo(workspace_, QPoint{}), ground_map_->size())),
                    "Vertical split clips the map tabs or the map content");
            save_workbench_image("workspace-drag-vertical.png");
            record("drag-pages-to-vertical-split");
            auto *tabs = smoke_tabs(QStringLiteral("三维视图"));
            smoke_drag_page(QStringLiteral("二维视图"),
                            tabs->mapToGlobal(QPoint(tabs->width() - 8, 16)));
            workspace_->select_view(0);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 67: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            require(orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                        look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                        look->up == smoke_camera_[2],
                    "Vertical split/merge changed the camera, time or cached orbit");
            slots_[1]->dock->show();
            fit_views();
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 68:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            orbit_capture_ = false;
            ++orbit_smoke_stage_;
            break;
        }
    }
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
                    // Accept both UNORM and sRGB encodings of the shader's warm yellow.
                    if (color.red() > 180 && color.green() > 110 &&
                        color.blue() < std::min(color.red(), color.green()) * 0.65)
                        ++marker;
                    if (color.red() > 180 && color.blue() > 140 && color.green() < 120)
                        ++selected_pixels;
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
    std::optional<core::Epoch> time_label_epoch_;
    CalendarScale time_label_scale_ = CalendarScale::utc;
    bool injecting_input_ = false;
    bool shared_device_ = false, shared_scene_ = false, independent_cameras_ = false;
    bool all_windows_closed_ = false;
    std::shared_ptr<const OrbitScene> orbit_;
    std::optional<application::Scenario> scenario_;
    std::vector<Satellite> satellites_;
    QTreeWidgetItem *scenario_item_ = nullptr;
    std::optional<std::size_t> editor_target_, pending_orbit_target_;
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
    QLabel *workspace_label_ = nullptr;
    QAction *new_action_ = nullptr, *example_action_ = nullptr, *insert_action_ = nullptr;
    QAction *fit_action_ = nullptr, *aux_action_ = nullptr, *map_action_ = nullptr;
    GroundMap *ground_map_ = nullptr;
    QToolBar *time_toolbar_ = nullptr;
    QMenu *insert_menu_ = nullptr;
    int smoke_bootstrap_ = 0;
    static constexpr int orbit_smoke_complete = 69;
    std::array<vsg::dvec3, 3> smoke_camera_;
    std::shared_ptr<const OrbitScene> smoke_cache_;
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
    std::shared_ptr<std::atomic_bool> cancellation_;
    OrbitEditor *editor_ = nullptr;
    QTreeWidgetItem *object_item_ = nullptr;
    QTreeWidget *objects_ = nullptr;
    QDockWidget *object_dock_ = nullptr;
    QStackedWidget *inspector_pages_ = nullptr;
    QAction *properties_action_ = nullptr, *focus_action_ = nullptr, *play_action_ = nullptr;
    QString last_orbit_outcome_;
    QString last_model_outcome_;
    std::future<vsg::ref_ptr<vsg::MatrixTransform>> model_candidate_;
    vsg::ref_ptr<vsg::MatrixTransform> model_node_;
    vsg::ref_ptr<vsg::Group> model_group_;
    int model_kind_ = 0, pending_model_kind_ = 0;
    double model_diameter_km_ = 1000, pending_model_diameter_km_ = 1000;
    int orbit_smoke_stage_ = 0;
    bool orbit_capture_ = false, fit_pending_ = false;
    std::unique_ptr<core::IFrameProvider> frames_;
    std::shared_ptr<const core::IEarthOrientationProvider> eop_, eop_history_, eop_current_;
    vsg::ref_ptr<vsgQt::Viewer> viewer_;
    vsg::ref_ptr<vsg::Device> shared_device_ref_;
    vsg::ref_ptr<vsg::ResourceHints> hints_;
    vsg::ref_ptr<render_vsg::CompileManager> compiler_;
    std::unique_ptr<render_vsg::MarkerLayer> markers_;
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
    QElapsedTimer clock_;
    QJsonArray view_report_;
    QJsonArray interaction_report_;
    visualization::RenderSnapshot snapshot_{
        core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}};
    std::optional<std::size_t> selected_, expected_selection_;
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
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
        Workbench window(app, log, smoke_dir, validation, benchmark_manifest, timing_dir,
                         timing_probe, output_only_probe);
        window.show();
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
} // namespace orbitforge::desktop
