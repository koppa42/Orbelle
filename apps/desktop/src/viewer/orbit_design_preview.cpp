#include "orbit_design_preview.hpp"
#include "../ui_text.hpp"
#include "responsive_layout.hpp"
#include "workbench_profile.hpp"
#include "workbench_style.hpp"
#include <QComboBox>
#include <QLabel>
#include <QMetaObject>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <orbelle/runtime/j2.hpp>

namespace orbelle::desktop {
namespace {
bool same_geometry(const core::EarthOrbitDesign &a, const core::EarthOrbitDesign &b) {
    const auto &x = a.elements;
    const auto &y = b.elements;
    return a.secular_j2 == b.secular_j2 && a.nodal_period_seconds == b.nodal_period_seconds &&
           x.epoch == y.epoch && x.semi_major_axis_m == y.semi_major_axis_m &&
           x.eccentricity == y.eccentricity && x.inclination_rad == y.inclination_rad &&
           x.raan_rad == y.raan_rad && x.argument_of_periapsis_rad == y.argument_of_periapsis_rad &&
           x.true_anomaly_rad == y.true_anomaly_rad;
}
struct PreviewCancelled {};
core::Epoch offset_epoch(core::Epoch start, double seconds) {
    const double offset = seconds * 1e9;
    if (!std::isfinite(offset) || offset < 0 ||
        offset >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        throw std::out_of_range(ui::utf8("orbit_wizard.preview_time_range"));
    const auto delta = static_cast<std::int64_t>(std::llround(offset));
    if (start.tai_nanoseconds() > std::numeric_limits<std::int64_t>::max() - delta)
        throw std::out_of_range(ui::utf8("orbit_wizard.preview_time_range"));
    return core::Epoch{start.tai_nanoseconds() + delta};
}
} // namespace
OrbitDesignPreview::OrbitDesignPreview(QWidget *parent) : QWidget(parent) {
    setObjectName("orbitDesignPreview");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto *row = new QHBoxLayout;
    auto *heading = new QLabel(ui::text("orbit_wizard.map_title"));
    heading->setObjectName("sectionTitle");
    row->addWidget(heading);
    row->addStretch();
    revolutions_ = new QComboBox;
    revolutions_->setObjectName("wizardPreviewRevolutions");
    revolutions_->addItem(ui::text("orbit_wizard.one_revolution"), 1);
    revolutions_->addItem(ui::text("orbit_wizard.three_revolutions"), 3);
    revolutions_->setAccessibleName(ui::text("orbit_wizard.preview_span"));
    row->addWidget(revolutions_);
    auto *reset = new QToolButton;
    reset->setIcon(workbench_icon(WorkbenchIcon::fit));
    reset->setToolTip(ui::text("orbit_wizard.reset_map"));
    reset->setAccessibleName(ui::text("orbit_wizard.reset_map"));
    reset->setAutoRaise(true);
    row->addWidget(reset);
    layout->addLayout(row);
    status_ = new QLabel(ui::text("orbit_wizard.preview_unavailable"));
    status_->setObjectName("orbitPreviewStatus");
    status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout->addWidget(status_);
    new WidthObserver(status_, [this](int width) {
        // Status changes should never resize the map, even when a narrow window
        // wraps one of the messages onto a second line.
        int height = status_->fontMetrics().lineSpacing();
        for (const char *key : {"orbit_wizard.preview_unavailable", "orbit_wizard.preview_invalid",
                                "orbit_wizard.preview_updating", "orbit_wizard.preview_failed",
                                "orbit_wizard.map_hint"})
            height = std::max(height, status_->fontMetrics()
                                          .boundingRect(QRect(0, 0, width, 10000), Qt::TextWordWrap,
                                                        ui::text(key))
                                          .height());
        status_->setFixedHeight(height);
    });
    setMinimumHeight(250);
    update_timer_.setSingleShot(true);
    update_timer_.setTimerType(Qt::PreciseTimer);
    refine_timer_.setSingleShot(true);
    refine_timer_.setInterval(100);
    connect(&update_timer_, &QTimer::timeout, this, [this] { start(); });
    connect(&refine_timer_, &QTimer::timeout, this, [this] {
        if (published_revision_ == revision_ && published_full_)
            return;
        full_requested_ = true;
        dirty_ = true;
        start();
    });
    connect(revolutions_, &QComboBox::currentIndexChanged, this, [this] { request_update(); });
    connect(reset, &QToolButton::clicked, this, [this] {
        if (map_)
            map_->reset_view();
    });
}
OrbitDesignPreview::~OrbitDesignPreview() {
    if (cancel_)
        cancel_->store(true);
    // Keep the QObject context alive until its worker has posted completion.
    // QObject destruction discards any queued callback that has not run yet.
    if (pending_.valid())
        pending_.wait();
}
void OrbitDesignPreview::set_context(QImage imagery,
                                     std::shared_ptr<const core::IFrameProvider> frames,
                                     std::shared_ptr<const core::IEarthOrientationProvider> eop) {
    frames_ = std::move(frames);
    eop_ = std::move(eop);
    if (cancel_)
        cancel_->store(true);
    ++context_revision_;
    rotation_cache_ = {};
    if (!map_ && !imagery.isNull()) {
        map_ = new GroundMap(std::move(imagery));
        map_->setObjectName("wizardGroundMap");
        map_->setMinimumHeight(220);
        map_->set_lighting(false);
        map_->set_sun_visible(false);
        map_->set_labels_visible(false);
        qobject_cast<QVBoxLayout *>(layout())->insertWidget(1, map_, 1);
    }
    request_update();
}
void OrbitDesignPreview::set_design(std::optional<core::EarthOrbitDesign> design) {
    if (design && design_ && same_geometry(*design, *design_))
        return;
    design_ = std::move(design);
    request_update();
}
void OrbitDesignPreview::request_update() {
    ++revision_;
    dirty_ = true;
    full_requested_ = false;
    refine_timer_.stop();
    status_->setToolTip({});
    // Input must not starve preparation of a geometry-independent time grid.
    // Detailed trajectories still cancel immediately; they belong to one design.
    if (cancel_ && (running_full_ || !design_))
        cancel_->store(true);
    // Keep the last valid picture and stable controls visible while dragging.
    // Invalid input or unavailable context must not retain an old trajectory.
    if (map_ && (!design_ || !frames_ || !eop_)) {
        map_->publish_tracks({});
        map_->set_snapshot({core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}});
    }
    if (!design_) {
        status_->setText(ui::text("orbit_wizard.preview_invalid"));
        update_timer_.stop();
    } else if (!map_ || !frames_ || !eop_) {
        status_->setText(ui::text("orbit_wizard.preview_unavailable"));
        update_timer_.stop();
    } else {
        status_->setText(ui::text(map_->track_count() ? "orbit_wizard.map_hint"
                                                      : "orbit_wizard.preview_updating"));
        // Coalesce edits from one event turn, without another frame of timer delay.
        if (!update_timer_.isActive())
            update_timer_.start(0);
        refine_timer_.start();
    }
}
void OrbitDesignPreview::start() {
    if (!dirty_ || !design_ || !map_ || !frames_ || !eop_)
        return;
    const auto count = revolutions_->currentData().toInt();
    const double duration = design_->nodal_period_seconds * count;
    // A power-of-two time span keeps exact sample epochs unchanged as altitude,
    // eccentricity and inclination vary. Three revolutions need a denser preview
    // so its shape already closely matches the settled curve.
    const double span = std::exp2(std::ceil(std::log2(duration)));
    const int interactive_segments = count == 1 ? 256 : 512;
    if (published_revision_ != revision_) {
        for (int i = 0; i < 2; ++i) {
            const auto &grid = rotation_cache_[i];
            if (grid && grid->matches(design_->elements.epoch, span, interactive_segments) &&
                grid->covers(duration)) {
                try {
                    const ProfileScope profile("preview_project_ms");
                    publish(sample(*design_, grid, duration, false, [] {}));
                } catch (const std::exception &error) {
                    map_->publish_tracks({});
                    map_->set_snapshot(
                        {core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}});
                    status_->setText(ui::text("orbit_wizard.preview_failed"));
                    status_->setToolTip(QString::fromUtf8(error.what()));
                    dirty_ = false;
                    return;
                }
                break;
            }
        }
    }
    if (published_revision_ == revision_ && (published_full_ || !full_requested_)) {
        dirty_ = false;
        return;
    }
    if (pending_.valid())
        return;
    dirty_ = false;
    running_revision_ = revision_;
    running_context_ = context_revision_;
    running_full_ = full_requested_;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    const bool full = running_full_;
    const int samples = full ? count * 360 : interactive_segments;
    const double grid_duration = full ? duration : span;
    auto cached = full ? rotation_cache_[2] : nullptr;
    if (cached && !cached->matches(design_->elements.epoch, grid_duration, samples))
        cached.reset();
    std::promise<Result> promise;
    result_ = promise.get_future();
    pending_ = std::async(
        std::launch::async, [this, design = *design_, frames = frames_, eop = eop_,
                             cancelled = cancel_, duration, grid_duration, samples, full,
                             cached = std::move(cached), promise = std::move(promise)]() mutable {
            try {
                const auto checkpoint = [&] {
                    if (cancelled->load(std::memory_order_relaxed))
                        throw PreviewCancelled{};
                };
                Result result;
                result.full = full;
                const auto &elements = design.elements;
                if (!cached) {
                    auto grid = std::make_shared<RotationGrid>();
                    grid->epoch = elements.epoch;
                    grid->duration = grid_duration;
                    grid->segments = samples;
                    grid->epochs.reserve(samples + 1);
                    grid->rotations.reserve(samples + 1);
                    for (int index = 0; index <= samples; ++index) {
                        checkpoint();
                        const double seconds =
                            index == samples ? grid_duration : grid_duration * index / samples;
                        // Future cache coverage may end before the bucket boundary.
                        // Failure within the requested view still fails the preview.
                        try {
                            const auto epoch = offset_epoch(elements.epoch, seconds);
                            const auto rotation =
                                frames->gcrf_to_itrf(epoch, eop->at(epoch).sample);
                            grid->epochs.push_back(epoch);
                            grid->rotations.push_back(rotation.matrix);
                        } catch (const std::exception &) {
                            if (!full && seconds > duration)
                                break;
                            throw;
                        }
                    }
                    cached = std::move(grid);
                }
                result.grid = cached;
                if (full)
                    result = sample(design, cached, duration, true, checkpoint);
                promise.set_value(std::move(result));
            } catch (const PreviewCancelled &) {
                Result result;
                result.cancelled = true;
                promise.set_value(std::move(result));
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
            // A context-bound queued call works with the minimal Qt Core build and
            // wakes the GUI immediately, without polling or touching widgets here.
            QMetaObject::invokeMethod(this, [this] { finish(); }, Qt::QueuedConnection);
        });
}
OrbitDesignPreview::Result OrbitDesignPreview::sample(const core::EarthOrbitDesign &design,
                                                      std::shared_ptr<const RotationGrid> grid,
                                                      double duration, bool full,
                                                      const std::function<void()> &checkpoint) {
    Result result;
    result.full = full;
    result.grid = std::move(grid);
    const auto &elements = design.elements;
    const auto initial =
        runtime::to_cartesian(elements, core::central_body(core::OriginId::earth).mu);
    const auto session =
        design.secular_j2
            ? runtime::make_j2_session(runtime::j2_mean_ellipse_elements(initial), {}, "preview")
            : runtime::make_two_body_session(initial, core::central_body(core::OriginId::earth).mu,
                                             "preview");
    const auto end = offset_epoch(elements.epoch, duration);
    auto last = std::lower_bound(
        result.grid->epochs.begin(), result.grid->epochs.end(), end,
        [](core::Epoch a, core::Epoch b) { return a.tai_nanoseconds() < b.tai_nanoseconds(); });
    if (last == result.grid->epochs.end())
        throw std::out_of_range(ui::utf8("orbit_wizard.preview_time_range"));
    result.track.epochs.assign(result.grid->epochs.begin(), last + 1);
    const auto states = session.sample(result.track.epochs, checkpoint);
    for (std::size_t index = 0; index < result.track.epochs.size(); ++index) {
        checkpoint();
        const auto &state = states.states()[index];
        const auto &rotation = result.grid->rotations[index];
        std::array<double, 3> position{};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                position[row] += rotation[row][column] * state.position_m[column];
        result.track.points.push_back(ground_coordinates(position));
        if (index == 0)
            result.marker = {
                state.epoch, core::FrameId::itrf, core::OriginId::earth, {{"preview", position}}};
    }
    if (result.track.epochs.back() != end) {
        // Clip the DISPLAY polyline to the requested duration, along its existing
        // last segment. No interpolated state or frame matrix enters the model.
        const auto n = result.track.epochs.size();
        const auto from = result.track.points[n - 2];
        auto &to = result.track.points.back();
        const double fraction = static_cast<double>(end.tai_nanoseconds() -
                                                    result.track.epochs[n - 2].tai_nanoseconds()) /
                                (result.track.epochs.back().tai_nanoseconds() -
                                 result.track.epochs[n - 2].tai_nanoseconds());
        const double longitude = from.x() + std::remainder(to.x() - from.x(), 360.0) * fraction;
        to = {std::remainder(longitude, 360.0), from.y() + (to.y() - from.y()) * fraction};
        result.track.epochs.back() = end;
    }
    result.track.path = ground_path(result.track.points);
    result.track.last = result.track.points.size();
    result.track.display.line_color = 0xecb3cf;
    result.track.display.line_width = 2;
    return result;
}
void OrbitDesignPreview::publish(Result result) {
    if (result.full && published_revision_ == revision_ && !published_full_)
        map_->refine_tracks({std::move(result.track)});
    else
        map_->publish_tracks({std::move(result.track)});
    map_->set_snapshot(result.marker);
    map_->set_selection(0);
    status_->setText(ui::text("orbit_wizard.map_hint"));
    published_revision_ = revision_;
    published_full_ = result.full;
}
void OrbitDesignPreview::finish() {
    pending_.get();
    try {
        auto result = result_.get();
        if (result.grid && running_context_ == context_revision_) {
            if (result.full)
                rotation_cache_[2] = result.grid;
            else {
                rotation_cache_[1] = std::move(rotation_cache_[0]);
                rotation_cache_[0] = result.grid;
                dirty_ = true; // Project the latest design, even if input kept moving.
                if (design_ && result.grid->epoch == design_->elements.epoch &&
                    !result.grid->covers(design_->nodal_period_seconds *
                                         revolutions_->currentData().toInt()))
                    full_requested_ = true;
            }
        }
        if (!result.cancelled && result.full && running_revision_ == revision_)
            publish(std::move(result));
    } catch (const std::exception &error) {
        if (running_revision_ == revision_) {
            map_->publish_tracks({});
            map_->set_snapshot({core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}});
            status_->setText(ui::text("orbit_wizard.preview_failed"));
            status_->setToolTip(QString::fromUtf8(error.what()));
        }
    }
    if (dirty_)
        start();
}
} // namespace orbelle::desktop
