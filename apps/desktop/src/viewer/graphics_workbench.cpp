#include "graphics_workbench.hpp"
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
#include "benchmark_gpu.hpp"
#include "benchmark_presentation.hpp"
#include "benchmark_run.hpp"
#include "benchmark_scene.hpp"
#endif
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QExposeEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <orbitforge/geo_rocky/globe.hpp>
#include <orbitforge/render_vsg/compile_manager.hpp>
#include <orbitforge/render_vsg/labels.hpp>
#include <orbitforge/render_vsg/marker_layer.hpp>
#include <orbitforge/render_vsg/swapchain_capture.hpp>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <vsg/all.h>
#include <vsgQt/Window.h>

namespace orbitforge::desktop {
namespace {
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
  public:
    Workbench(QApplication &app, ValidationLog &log, const QString &smoke_dir, bool validation,
              const QString &benchmark_manifest, const QString &timing_dir, bool timing_probe,
              bool output_only_probe)
        : app_(app), log_(log), smoke_dir_(smoke_dir), validation_(validation),
          benchmarking_(!benchmark_manifest.isEmpty()),
          session_(runtime::make_circular_demo_session()), frames_(runtime::make_frame_provider()),
          eop_(runtime::load_eop_snapshot(
              std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
              "resources/eop/manifest.json")),
          viewer_(vsgQt::Viewer::create()),
          markers_(std::make_unique<render_vsg::MarkerLayer>(benchmarking_ ? 10000 : 1)),
          selection_(
              std::make_unique<render_vsg::MarkerLayer>(1, render_vsg::MarkerStyle::selection)) {
        try {
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
            globe_ = geo_rocky::make_globe(viewer_);
            scene_ = vsg::Group::create();
            scene_->addChild(globe_->node());
            scene_->addChild(markers_->node());
            if (benchmarking_) {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
                for (std::size_t i = 0; i < 100; ++i)
                    scene_->addChild(render_vsg::make_track(benchmark_->cache->track(i * 100)));
#endif
            } else {
                const auto series = session_.sample(runtime::demo_epochs());
                std::vector<visualization::RenderSnapshot> track;
                for (std::size_t i = 0; i < series.states().size(); ++i) {
                    auto snapshot = visualization::make_snapshot(series, i);
                    track.push_back(visualization::rotate_snapshot(
                        snapshot,
                        frames_->gcrf_to_itrf(snapshot.epoch, eop_->at(snapshot.epoch).sample)));
                }
                scene_->addChild(render_vsg::make_track(track));
            }
            snapshot_ = sample_frame(core::Epoch{0});
            markers_->update(snapshot_);
            initial_position_ = snapshot_.markers.front().position_m;
            scene_->addChild(selection_->node());
            setWindowTitle(benchmarking_ ? QStringLiteral("OrbitForge — 万星双视图基准场景")
                                         : QStringLiteral("OrbitForge — 双视窗圆轨道"));
            resize_to_screen(1480, 900);
            auto *objects = new QTreeWidget;
            objects->setHeaderLabel(QStringLiteral("场景"));
            auto *scenario =
                new QTreeWidgetItem(objects, {benchmarking_ ? QStringLiteral("固定星座 · 10,000 颗")
                                                            : QStringLiteral("圆轨道示例")});
            new QTreeWidgetItem(scenario,
                                {benchmarking_ ? QStringLiteral("100 条轨迹 · 每视图 200 标签")
                                               : QStringLiteral("demo-satellite")});
            objects->expandAll();
            auto *object_dock = new QDockWidget(QStringLiteral("对象"), this);
            object_dock->setWidget(objects);
            addDockWidget(Qt::LeftDockWidgetArea, object_dock);
            object_dock->setMaximumWidth(220);
            auto *toolbar = addToolBar(QStringLiteral("时间"));
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_)
                toolbar->setEnabled(false);
#endif
            auto *play = toolbar->addAction(QStringLiteral("播放 / 暂停"));
            connect(play, &QAction::triggered, this, [this] {
                playing_ = !playing_;
                clock_.restart();
            });
            slider_ = new QSlider(Qt::Horizontal);
            slider_->setRange(0, benchmarking_ ? 600 : 300);
            slider_->setMinimumWidth(300);
            toolbar->addWidget(slider_);
            connect(slider_, &QSlider::valueChanged, this, [this](int value) {
                seconds_ = value;
                requested_time_ = true;
                clock_.restart();
            });
            build_slot(false);
            build_slot(true);
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
    void shutdown() {
        if (stopped_)
            return;
        stopped_ = true;
        timer_.stop();
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
        const std::array epochs{epoch};
        const auto sample = session_.sample(epochs);
        return visualization::rotate_snapshot(visualization::make_snapshot(sample, 0),
                                              frames_->gcrf_to_itrf(epoch, eop_->at(epoch).sample));
    }
    void fail(const std::string &message) {
        failure_ = QString::fromStdString(message);
        std::cerr << "Graphics failure: " << message << '\n';
        timer_.stop();
        app_.exit(1);
        QTimer::singleShot(0, this, [this] { app_.exit(1); });
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
        current->container->setMinimumSize(240, 240);
        if (benchmarking_) {
            const auto ratio = current->window->devicePixelRatio();
            const int width = qRound(1280.0 / ratio), height = qRound(720.0 / ratio);
            if (qRound(width * ratio) != 1280 || qRound(height * ratio) != 720)
                throw std::runtime_error("This DPI cannot represent a 1280x720 native viewport; "
                                         "choose a compatible QT_SCALE_FACTOR");
            current->container->setFixedSize(width, height);
        }
        if (secondary) {
            current->dock = new QDockWidget(QStringLiteral("三维视图 2"), this);
            current->dock->setWidget(current->container);
            addDockWidget(Qt::RightDockWidgetArea, current->dock);
            resizeDocks({current->dock}, {600}, Qt::Horizontal);
            auto *action = menuBar()->addAction(QStringLiteral("显示视图 2"));
            connect(action, &QAction::triggered, current->dock, &QWidget::show);
        } else {
            setCentralWidget(current->container);
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
            selected_ = render_vsg::pick_marker(current->pick_candidates, point.x(), point.y(), 8);
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
        render_graph->setClearValues({{0.018f, 0.025f, 0.045f, 1.0f}});
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
    void frame() {
        if (viewer_->windows().empty())
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
        if (!requested_time_ && (playing_ || !smoke_dir_.isEmpty())) {
            seconds_ = !smoke_dir_.isEmpty()
                           ? static_cast<double>(frame_count_)
                           : std::fmod(seconds_ + static_cast<double>(clock_.restart()) *
                                                      (benchmarking_ ? 0.01 : 0.02),
                                       benchmarking_ ? 600.0 : 300.0);
            slider_->blockSignals(true);
            slider_->setValue(static_cast<int>(seconds_));
            slider_->blockSignals(false);
        }
        requested_time_ = false;
        const core::Epoch epoch(static_cast<std::int64_t>(std::llround(seconds_ * 1e9)));
        snapshot_ = sample_frame(epoch);
        markers_->update(snapshot_);
        visualization::RenderSnapshot selected_snapshot{
            snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}};
        if (selected_)
            selected_snapshot.markers.push_back(snapshot_.markers.at(*selected_));
        selection_->update(selected_snapshot);
        bool active = false;
        // Image acquisition can recreate an out-of-date swapchain and wait for the device.
        run_exclusive([&] { active = viewer_->advanceToNextFrame(seconds_); });
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
            slot->pick_candidates =
                render_vsg::visible_markers(snapshot_, *slot->camera, extent.width, extent.height);
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
             (benchmarking_ && (frame_count_ == 30 || frame_count_ == 160 || frame_count_ == 180)));
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        const auto update_qpc = run_ ? BenchmarkRun::qpc() : 0;
#endif
        if (capture)
            for (const auto &slot : slots_)
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
                    if (std::find(task->commandGraphs.begin(), task->commandGraphs.end(),
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
            QStringLiteral("ITRF · 椭球地球 | 2000-01-01 12:00:00 TAI + %1 s | %2")
                .arg(seconds_, 0, 'f', 1)
                .arg(selected_ ? QString::fromStdString(snapshot_.markers.at(*selected_).object_id)
                               : QStringLiteral("未选中")));
        if (!smoke_dir_.isEmpty()) {
            if (frame_count_ == 20) {
                for (std::size_t i = 0; i < slots_.size(); ++i)
                    if (!slots_[i]->captured_image.save(smoke_dir_ +
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
            if (frame_count_ == 190)
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
            if (benchmarking_ && selected_pixels < 20)
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
                {"capture_epoch_tai_ns", static_cast<qint64>(std::llround(seconds_ * 1e9))},
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
    bool injecting_input_ = false;
    bool shared_device_ = false, shared_scene_ = false, independent_cameras_ = false;
    bool all_windows_closed_ = false;
    application::Session session_;
    std::unique_ptr<core::IFrameProvider> frames_;
    std::unique_ptr<core::IEarthOrientationProvider> eop_;
    vsg::ref_ptr<vsgQt::Viewer> viewer_;
    vsg::ref_ptr<vsg::Device> shared_device_ref_;
    vsg::ref_ptr<vsg::ResourceHints> hints_;
    vsg::ref_ptr<render_vsg::CompileManager> compiler_;
    std::unique_ptr<render_vsg::MarkerLayer> markers_;
    std::unique_ptr<render_vsg::MarkerLayer> selection_;
    std::unique_ptr<render_vsg::IEarthDisplay> globe_;
    vsg::ref_ptr<vsg::Group> scene_;
    std::vector<std::unique_ptr<Slot>> slots_;
    QSlider *slider_ = nullptr;
    QTimer timer_;
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
    const auto smoke_index = arguments.indexOf("--graphics-smoke");
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
    const bool validation = smoke_index >= 0 || arguments.contains("--validation");
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
