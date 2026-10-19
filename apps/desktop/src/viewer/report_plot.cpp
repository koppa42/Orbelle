#include "report_plot.hpp"
#include "workbench_theme.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string_view>

namespace orbitforge::desktop {
namespace {
QString str(const std::string &text) { return QString::fromStdString(text); }
double nice_step(double value) {
    const auto scale = std::pow(10.0, std::floor(std::log10(value)));
    for (auto multiplier : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (value <= multiplier * scale)
            return multiplier * scale;
    return 10 * scale;
}
QString axis_number(double value, double step) {
    if (std::abs(value) < step * 1e-8)
        value = 0;
    if (std::abs(value) >= 1e9 || (step < 1e-6 && value != 0))
        return QString::number(value, 'g', 9);
    const auto decimals = std::clamp(static_cast<int>(-std::floor(std::log10(step))) + 1, 0, 10);
    auto text = QString::number(value, 'f', decimals);
    if (text.contains('.')) {
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith('.'))
            text.chop(1);
    }
    return text;
}
constexpr const char *colors[]{"#a9a7ff", "#6cdaca", "#ffbd87", "#87bcff", "#efa0cc", "#bcd68a",
                               "#dac5ff", "#82d0ee", "#e9d67f", "#e99595", "#97adb9", "#cfb49a"};
} // namespace
ReportPlot::ReportPlot(QWidget *parent) : QWidget(parent) {
    setObjectName("reportPlot");
    setAccessibleName(QStringLiteral("报告时间曲线"));
    setMinimumSize(0, 220);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
}
void ReportPlot::set_result(std::shared_ptr<const runtime::ReportResult> result, std::size_t column,
                            QString label, std::string object, bool preserve_view) {
    set_result(std::move(result), {{column, {}, std::move(label), {}}}, std::move(object),
               preserve_view);
}
void ReportPlot::set_field(std::size_t column, QString label) {
    set_fields({{column, {}, std::move(label), {}}});
}
void ReportPlot::set_result(std::shared_ptr<const runtime::ReportResult> result,
                            std::vector<ReportCurveField> fields, std::string object,
                            bool preserve_view) {
    result_ = std::move(result);
    fields_ = std::move(fields);
    object_ = std::move(object);
    rebuild(preserve_view);
}
void ReportPlot::set_fields(std::vector<ReportCurveField> fields) {
    fields_ = std::move(fields);
    rebuild(true);
}
void ReportPlot::set_object(std::string id) {
    object_ = std::move(id);
    rebuild(true);
}
void ReportPlot::rebuild(bool preserve_view) {
    plot_image_ = {};
    series_.clear();
    panels_.clear();
    hover_ = dragging_ = false;
    QToolTip::hideText();
    setCursor(pan_mode_ ? Qt::OpenHandCursor : Qt::CrossCursor);
    if (!result_ || fields_.empty()) {
        setMinimumHeight(220);
        update();
        return;
    }
    const auto &columns = result_->table.columns();
    const auto time = std::find_if(columns.begin(), columns.end(),
                                   [](const auto &c) { return c.id == "time.epoch"; });
    const auto id_column = std::ranges::find(columns, "object.id", &results::Column::id);
    const auto name_column = std::ranges::find(columns, "object.name", &results::Column::id);
    if (time == columns.end() || id_column == columns.end() || name_column == columns.end()) {
        update();
        return;
    }
    time_column_ = static_cast<std::size_t>(time - columns.begin());
    std::map<std::string, std::size_t> color_indices;
    const Qt::PenStyle styles[]{Qt::SolidLine, Qt::DashLine, Qt::DotLine, Qt::DashDotLine};
    for (std::size_t f = 0; f < fields_.size(); ++f) {
        auto &field = fields_[f];
        if (field.column >= columns.size())
            continue;
        field.id = str(columns[field.column].id);
        field.unit = str(result_->column_unit(field.column));
        auto field_name = field.label;
        const auto suffix = " (" + field.unit + ")";
        if (!field.unit.isEmpty() && field_name.endsWith(suffix))
            field_name.chop(suffix.size());
        auto panel = std::ranges::find(panels_, field.unit, &Panel::unit);
        if (panel == panels_.end()) {
            panels_.push_back({field.unit, field_name, {}, {}});
            panel = std::prev(panels_.end());
        } else
            panel->title += " / " + field_name;
        panel->fraction |= field.id == "lighting.fraction";
        const auto panel_index = static_cast<std::size_t>(panel - panels_.begin());
        const bool cyclic = field.id == "geodetic.longitude" || field.id == "elements.raan" ||
                            field.id == "elements.argp" || field.id == "elements.nu" ||
                            field.id == "aer.azimuth";
        const auto half_turn = field.unit == "deg" ? 180.0 : std::numbers::pi;
        std::map<std::string, std::size_t> indices;
        std::set<std::string> missing;
        std::string_view previous_id;
        const auto no_series = std::numeric_limits<std::size_t>::max();
        std::size_t current_series = no_series;
        bool have_previous = false;
        for (std::size_t r = 0; r < result_->table.rows().size(); ++r) {
            const auto &row = result_->table.rows()[r];
            const auto *id = std::get_if<std::string>(&row[id_column - columns.begin()]);
            const auto *epoch = std::get_if<core::Epoch>(&row[time_column_]);
            if (!id || !epoch)
                continue;
            if (have_previous && previous_id == *id && current_series == no_series)
                continue;
            const auto value = result_->numeric_value(r, field.column);
            if (!value) {
                missing.insert(*id);
                continue;
            }
            // Reports normally group samples by object. Resolve its stable color
            // and series once per run, while still accepting interleaved rows.
            if (!have_previous || previous_id != *id) {
                have_previous = true;
                previous_id = *id;
                const auto color =
                    color_indices.try_emplace(*id, color_indices.size()).first->second;
                if (!object_.empty() && object_ != *id) {
                    current_series = no_series;
                    continue;
                }
                const auto [entry, inserted] = indices.try_emplace(*id, series_.size());
                current_series = entry->second;
                if (inserted) {
                    auto label = str(std::get<std::string>(row[name_column - columns.begin()]));
                    if (fields_.size() > 1)
                        label = field_name + " · " + label;
                    const bool duplicate =
                        std::any_of(series_.begin(), series_.end(),
                                    [&](const auto &s) { return s.label == label; });
                    if (duplicate)
                        label += " [" + str(*id) + "]";
                    const auto tone = color + (fields_.size() > 1 ? field.column : 0);
                    panel->series.push_back(series_.size());
                    series_.push_back({label,
                                       QColor(colors[tone % std::size(colors)]),
                                       styles[f % std::size(styles)],
                                       field.column,
                                       panel_index,
                                       {},
                                       {},
                                       {}});
                }
            }
            auto &points = series_[current_series].points;
            const bool gap =
                missing.erase(*id) != 0 ||
                (cyclic && !points.empty() && std::abs(points.back().y - *value) > half_turn);
            points.push_back({epoch->seconds_since(result_->first), *value, r, gap});
        }
    }
    // Exact block extrema avoid rescanning every visible sample for axis limits
    // on each wheel/drag event. Partial edge blocks still inspect the raw values.
    for (auto &series : series_) {
        series.extents.reserve((series.points.size() + extent_block_size - 1) / extent_block_size);
        for (std::size_t i = 0; i < series.points.size(); ++i) {
            const auto value = series.points[i].y;
            if (i % extent_block_size == 0)
                series.extents.emplace_back(value, value);
            else {
                auto &[low, high] = series.extents.back();
                low = std::min(low, value);
                high = std::max(high, value);
            }
        }
    }
    setMinimumHeight(std::max(220, static_cast<int>(panels_.size()) * 210 + 50));
    full_left_ = 0;
    full_right_ = result_->last.seconds_since(result_->first);
    if (full_right_ == 0) {
        full_left_ = -0.5;
        full_right_ = 0.5;
    }
    if (preserve_view)
        set_interval(left_, right_);
    else
        reset_view();
}
void ReportPlot::reset_view() {
    dragging_ = hover_ = false;
    setCursor(pan_mode_ ? Qt::OpenHandCursor : Qt::CrossCursor);
    QToolTip::hideText();
    set_interval(full_left_, full_right_);
}
void ReportPlot::set_pan_mode(bool enabled) {
    pan_mode_ = enabled;
    dragging_ = hover_ = false;
    setCursor(pan_mode_ ? Qt::OpenHandCursor : Qt::CrossCursor);
    QToolTip::hideText();
    update();
}
void ReportPlot::zoom(double factor) {
    if (!std::isfinite(factor) || factor <= 0)
        return;
    const auto center = (left_ + right_) / 2;
    set_interval(center + (left_ - center) * factor, center + (right_ - center) * factor);
}
void ReportPlot::set_interval(double left, double right) {
    const auto min_span = std::max(1e-9, (full_right_ - full_left_) * 1e-9);
    if (right - left < min_span)
        return;
    const auto span = std::min(right - left, full_right_ - full_left_);
    left_ = std::clamp(left, full_left_, full_right_ - span);
    right_ = left_ + span;
    layout_plot();
    update();
}
QPointF ReportPlot::pixel(const Point &point, const Series &series) const {
    const auto &panel = panels_[series.panel];
    return {panel.rect.left() + (point.x - left_) / (right_ - left_) * panel.rect.width(),
            panel.rect.bottom() -
                (point.y - panel.low) / (panel.high - panel.low) * panel.rect.height()};
}
bool ReportPlot::inside_plot(QPointF point) const {
    return std::ranges::any_of(panels_,
                               [&](const auto &panel) { return panel.rect.contains(point); });
}
double ReportPlot::time_at(double x) const {
    return left_ + std::clamp((x - plot_.left()) / plot_.width(), 0.0, 1.0) * (right_ - left_);
}
void ReportPlot::layout_plot() {
    plot_image_ = {};
    const auto fm = fontMetrics();
    const auto per_row = std::max(1, (width() - 32) / 180);
    int axis_width = 48;
    for (auto &panel : panels_) {
        auto &low = panel.low;
        auto &high = panel.high;
        low = std::numeric_limits<double>::infinity();
        high = -low;
        for (const auto index : panel.series) {
            const auto &series = series_[index];
            // Include the two neighbouring samples so a zoom between samples remains meaningful.
            auto begin = std::lower_bound(series.points.begin(), series.points.end(), left_,
                                          [](const auto &p, double x) { return p.x < x; });
            if (begin != series.points.begin())
                --begin;
            const auto end = std::upper_bound(series.points.begin(), series.points.end(), right_,
                                              [](double x, const auto &p) { return x < p.x; });
            auto extended = end;
            if (extended != series.points.end())
                ++extended;
            const auto stop = static_cast<std::size_t>(extended - series.points.begin());
            for (auto i = static_cast<std::size_t>(begin - series.points.begin()); i < stop;) {
                if (i % extent_block_size == 0 && stop - i >= extent_block_size) {
                    const auto &extent = series.extents[i / extent_block_size];
                    low = std::min(low, extent.first);
                    high = std::max(high, extent.second);
                    i += extent_block_size;
                } else {
                    low = std::min(low, series.points[i].y);
                    high = std::max(high, series.points[i].y);
                    ++i;
                }
            }
        }
        if (!std::isfinite(low)) {
            low = 0;
            high = 1;
        }
        const auto padding = std::max((high - low) * 0.06, std::max(std::abs(low), 1.0) * 1e-8);
        const auto data_low = low, data_high = high;
        low -= padding;
        high += padding;
        // Keep the data extent independent of tick spacing: rounding both ends to a coarse tick
        // used to waste up to half the plot on empty space. Anchor near-zero unipolar data at zero.
        if (data_low >= 0 && data_high > 0 && low < 0)
            low = 0;
        if (data_high <= 0 && data_low < 0 && high > 0)
            high = 0;
        panel.step = nice_step((high - low) / 5);
        if (panel.fraction) {
            low = 0;
            high = panel.unit == "%" ? 100 : 1;
            panel.step = high / 4;
        }
        const auto first_y = std::ceil(low / panel.step) * panel.step;
        for (double tick = first_y; tick <= high + panel.step * 1e-9; tick += panel.step)
            axis_width =
                std::max(axis_width, fm.horizontalAdvance(axis_number(tick, panel.step)) + 18);
    }
    axis_width = std::min(axis_width, std::max(48, width() / 3));
    const auto panel_height = (height() - 62.0) / std::max<std::size_t>(1, panels_.size());
    for (std::size_t i = 0; i < panels_.size(); ++i) {
        auto &panel = panels_[i];
        const auto rows =
            std::min(3, (static_cast<int>(panel.series.size()) + per_row - 1) / per_row);
        const double top = i * panel_height + 38 + rows * (fm.height() + 5);
        panel.rect =
            QRectF(axis_width, top, std::max(1, width() - axis_width - 22),
                   std::max(1.0, (i + 1) * panel_height - top - (i + 1 < panels_.size() ? 12 : 0)));
    }
    plot_ = panels_.empty() ? QRectF{} : panels_.front().rect.united(panels_.back().rect);
    // Keep first, extrema and last per horizontal pixel bucket, in chronological order.
    // This bounds painted vertices while retaining narrow peaks; raw rows remain untouched.
    for (auto &series : series_) {
        series.path.clear();
        series.path.reserve(static_cast<int>(
            std::min(series.points.size(), static_cast<std::size_t>(plot_.width() * 4 + 8))));
        const Point *first = nullptr, *minimum = nullptr, *maximum = nullptr, *last = nullptr;
        int previous_pixel = std::numeric_limits<int>::min();
        bool connected = false;
        const auto flush = [&] {
            if (!first)
                return;
            std::array<const Point *, 4> keep{first, minimum, maximum, last};
            std::sort(keep.begin(), keep.end(), [](auto a, auto b) { return a->row < b->row; });
            const Point *previous = nullptr;
            for (auto p : keep) {
                if (p == previous)
                    continue;
                if (!connected) {
                    series.path.moveTo(pixel(*p, series));
                    connected = true;
                } else
                    series.path.lineTo(pixel(*p, series));
                previous = p;
            }
            first = nullptr;
        };
        auto begin = std::lower_bound(series.points.begin(), series.points.end(), left_,
                                      [](const auto &p, double x) { return p.x < x; });
        if (begin != series.points.begin())
            --begin;
        for (auto it = begin; it != series.points.end(); ++it) {
            const auto &area = panels_[series.panel].rect;
            const auto x = area.left() + (it->x - left_) / (right_ - left_) * area.width();
            const auto px = static_cast<int>(std::clamp(x, -1e6, 1e6));
            if (px != previous_pixel || it->gap) {
                flush();
                previous_pixel = px;
            }
            if (it->gap)
                connected = false;
            if (!first)
                first = minimum = maximum = &*it;
            else {
                if (it->y < minimum->y)
                    minimum = &*it;
                // Match minmax_element: first minimum, last maximum on ties.
                if (it->y >= maximum->y)
                    maximum = &*it;
            }
            last = &*it;
            if (it->x > right_)
                break;
        }
        flush();
    }
}
void ReportPlot::draw(QPainter &p) const {
    p.fillRect(rect(), theme::color(theme::chrome));
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(font());
    const auto fm = fontMetrics();
    if (series_.empty()) {
        p.setPen(theme::color(theme::text_muted));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("此结果没有可绘制的时间序列"));
        return;
    }
    const auto per_row = std::max(1, (width() - 32) / 180);
    const auto panel_height = (height() - 62.0) / panels_.size();
    const double time_scale = right_ - left_ >= 7200 ? 3600 : right_ - left_ >= 120 ? 60 : 1;
    const auto x_step = nice_step((right_ - left_) / time_scale / (width() < 500 ? 3 : 5));
    const auto first_tick = std::ceil(left_ / time_scale / x_step) * x_step;
    for (std::size_t group = 0; group < panels_.size(); ++group) {
        const auto &panel = panels_[group];
        const auto &area = panel.rect;
        const auto origin_y = group * panel_height;
        p.setPen(theme::color(theme::text));
        p.drawText(QRectF(16, origin_y + 4, width() - 32, 26), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(panel.title +
                                     (panel.unit.isEmpty() ? QString{} : " (" + panel.unit + ")"),
                                 Qt::ElideRight, width() - 32));
        const auto legend_count =
            std::min(panel.series.size(), static_cast<std::size_t>(per_row * 3));
        for (std::size_t i = 0; i < legend_count; ++i) {
            const auto &series = series_[panel.series[i]];
            const auto x = 16 + static_cast<int>(i) % per_row * 180;
            const auto y = origin_y + 32 + static_cast<int>(i) / per_row * (fm.height() + 5);
            p.setPen(QPen(series.color, 2, series.style));
            p.drawLine(QPointF(x, y + fm.height() / 2), QPointF(x + 16, y + fm.height() / 2));
            p.setPen(theme::color(theme::text_soft));
            p.drawText(
                QRectF(x + 24, y, 148, fm.height()), Qt::AlignVCenter,
                i + 1 == legend_count && legend_count < panel.series.size()
                    ? QStringLiteral("另 %1 条曲线…").arg(panel.series.size() - legend_count + 1)
                    : fm.elidedText(series.label, Qt::ElideRight, 148));
        }
        p.fillRect(area, theme::color(theme::field));
        const auto first_y = std::ceil(panel.low / panel.step) * panel.step;
        for (double tick = first_y; tick <= panel.high + panel.step * 1e-9; tick += panel.step) {
            const auto y =
                area.bottom() - area.height() * (tick - panel.low) / (panel.high - panel.low);
            p.setPen(QPen(theme::color(theme::line, 160), 1));
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            p.setPen(theme::color(theme::text_muted));
            p.drawText(QRectF(2, y - fm.height() / 2.0, area.left() - 10, fm.height()),
                       Qt::AlignRight | Qt::AlignVCenter, axis_number(tick, panel.step));
        }
        p.setPen(QPen(theme::color(theme::border), 1));
        p.drawLine(area.topLeft(), area.bottomLeft());
        p.drawLine(area.bottomLeft(), area.bottomRight());
        for (int i = 0; i < 10; ++i) {
            const auto value = first_tick + i * x_step;
            if (value > right_ / time_scale + x_step * 1e-9)
                break;
            const auto x =
                area.left() + area.width() * (value * time_scale - left_) / (right_ - left_);
            p.setPen(QPen(theme::color(theme::line, 150), 1));
            p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            if (group + 1 == panels_.size()) {
                p.setPen(theme::color(theme::text_muted));
                const auto align = x - area.left() < 30    ? Qt::AlignLeft
                                   : area.right() - x < 30 ? Qt::AlignRight
                                                           : Qt::AlignHCenter;
                const auto start = align == Qt::AlignLeft    ? x
                                   : align == Qt::AlignRight ? x - 100
                                                             : x - 50;
                p.drawText(QRectF(start, area.bottom() + 8, 100, fm.height()),
                           align | Qt::AlignVCenter, axis_number(value, x_step));
            }
        }
        p.save();
        p.setClipRect(area.adjusted(-2, -2, 2, 2));
        for (const auto index : panel.series) {
            const auto &series = series_[index];
            p.setPen(QPen(series.color, 1.8, series.style));
            p.setBrush(Qt::NoBrush);
            p.drawPath(series.path);
            if (series.points.size() <= 80) {
                p.setBrush(series.color);
                for (const auto &point : series.points)
                    p.drawEllipse(pixel(point, series), 2.4, 2.4);
            }
        }
        p.restore();
    }
    auto origin = str(result_->cell_text(series_.front().points.front().row, time_column_));
    if (origin.endsWith('Z'))
        origin.chop(1);
    while (origin.endsWith('0'))
        origin.chop(1);
    if (origin.endsWith('.'))
        origin.chop(1);
    origin.replace('T', ' ');
    const auto axis = QStringLiteral("自 %1 %2 起 / %3")
                          .arg(origin, str(result_->column_unit(time_column_)),
                               time_scale == 3600 ? "h"
                               : time_scale == 60 ? "min"
                                                  : "s");
    p.setPen(theme::color(theme::text_muted));
    p.drawText(QRectF(12, height() - 42, width() - 24, 34), Qt::AlignCenter | Qt::TextWordWrap,
               axis);
}
void ReportPlot::draw_interaction(QPainter &p) const {
    const Series *series = nullptr;
    const auto *point = hover_ && !dragging_ ? nearest(cursor_, &series) : nullptr;
    p.setRenderHint(QPainter::Antialiasing);
    for (std::size_t group = 0; group < panels_.size(); ++group) {
        const auto &area = panels_[group].rect;
        p.save();
        p.setClipRect(area.adjusted(-2, -2, 2, 2));
        if (point) {
            const auto pos = pixel(*point, *series);
            p.setPen(QPen(theme::color(theme::text_muted), 1, Qt::DashLine));
            p.drawLine(QPointF(pos.x(), area.top()), QPointF(pos.x(), area.bottom()));
            if (series->panel == group) {
                p.drawLine(QPointF(area.left(), pos.y()), QPointF(area.right(), pos.y()));
                p.setBrush(theme::color(theme::text));
                p.drawEllipse(pos, 4, 4);
            }
        }
        if (dragging_ && !panning_)
            p.fillRect(QRectF(QPointF(std::min(drag_start_.x(), cursor_.x()), area.top()),
                              QPointF(std::max(drag_start_.x(), cursor_.x()), area.bottom())),
                       theme::color(theme::accent, 50));
        p.restore();
    }
}
QImage ReportPlot::image(qreal scale) const {
    scale = std::clamp(scale, 1.0, 4.0);
    QImage output(QSize(qRound(width() * scale), qRound(height() * scale)),
                  QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.scale(scale, scale);
    draw(painter);
    return output;
}
void ReportPlot::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto ratio = devicePixelRatioF();
    const QSize pixels(qRound(width() * ratio), qRound(height() * ratio));
    // Hover and range selection only change the overlay. Bound the static raster
    // to one viewport and 64 MiB; exports still render vectors at their own scale.
    if (static_cast<qint64>(pixels.width()) * pixels.height() * 4 <= 64 * 1024 * 1024) {
        if (plot_image_.size() != pixels || plot_image_.devicePixelRatio() != ratio) {
            plot_image_ = QImage(pixels, QImage::Format_RGB32);
            plot_image_.setDevicePixelRatio(ratio);
            QPainter cache(&plot_image_);
            draw(cache);
        }
        if (plot_image_.isNull())
            draw(p);
        else
            p.drawImage(QPointF{}, plot_image_);
    } else {
        plot_image_ = {};
        draw(p);
    }
    draw_interaction(p);
}
void ReportPlot::resizeEvent(QResizeEvent *) { layout_plot(); }
void ReportPlot::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::PaletteChange) {
        layout_plot();
        update();
    }
}
const ReportPlot::Point *ReportPlot::nearest(QPointF position, const Series **source) const {
    const Point *best = nullptr;
    double distance = std::numeric_limits<double>::infinity();
    const auto epoch = time_at(position.x());
    for (const auto &series : series_) {
        if (!panels_[series.panel].rect.contains(position))
            continue;
        auto it = std::lower_bound(series.points.begin(), series.points.end(), epoch,
                                   [](const auto &p, double x) { return p.x < x; });
        for (int i = 0; i < 2; ++i) {
            if (it != series.points.end() && it->x >= left_ && it->x <= right_) {
                const auto d = pixel(*it, series) - position;
                const auto length = d.x() * d.x() + d.y() * d.y();
                if (length < distance) {
                    distance = length;
                    best = &*it;
                    if (source)
                        *source = &series;
                }
            }
            if (it == series.points.begin())
                break;
            --it;
        }
    }
    return best;
}
void ReportPlot::mouseMoveEvent(QMouseEvent *event) {
    cursor_ = event->position();
    hover_ = inside_plot(cursor_);
    if (dragging_ && panning_) {
        const auto shift =
            (drag_start_.x() - cursor_.x()) / drag_width_ * (drag_right_ - drag_left_);
        set_interval(drag_left_ + shift, drag_right_ + shift);
    }
    const Series *series = nullptr;
    if (hover_ && !dragging_) {
        if (const auto *point = nearest(cursor_, &series)) {
            QToolTip::showText(
                event->globalPosition().toPoint(),
                "<b>" + series->label.toHtmlEscaped() + "</b><br>" +
                    str(result_->cell_text(point->row, time_column_)).toHtmlEscaped() + " " +
                    str(result_->column_unit(time_column_)) + "<br>" +
                    str(result_->cell_text(point->row, series->column)) + " " +
                    str(result_->column_unit(series->column)),
                this);
        }
    } else
        QToolTip::hideText();
    update();
}
void ReportPlot::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) {
        reset_view();
        return;
    }
    if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) &&
        inside_plot(event->position())) {
        drag_start_ = cursor_ = event->position();
        dragging_ = true;
        drag_button_ = event->button();
        panning_ = pan_mode_ || drag_button_ == Qt::MiddleButton;
        drag_left_ = left_;
        drag_right_ = right_;
        drag_width_ = plot_.width();
        if (panning_)
            setCursor(Qt::ClosedHandCursor);
        QToolTip::hideText();
        setFocus();
    }
}
void ReportPlot::mouseReleaseEvent(QMouseEvent *event) {
    if (!dragging_ || event->button() != drag_button_)
        return;
    dragging_ = false;
    setCursor(pan_mode_ ? Qt::OpenHandCursor : Qt::CrossCursor);
    cursor_ = event->position();
    if (!panning_ && std::abs(cursor_.x() - drag_start_.x()) > 8)
        set_interval(time_at(std::min(cursor_.x(), drag_start_.x())),
                     time_at(std::max(cursor_.x(), drag_start_.x())));
    update();
}
void ReportPlot::mouseDoubleClickEvent(QMouseEvent *event) {
    dragging_ = false;
    setCursor(pan_mode_ ? Qt::OpenHandCursor : Qt::CrossCursor);
    if (event->button() == Qt::LeftButton && inside_plot(event->position()) && seek)
        if (const auto *point = nearest(event->position()))
            seek(std::get<core::Epoch>(result_->table.rows()[point->row][time_column_]));
}
void ReportPlot::wheelEvent(QWheelEvent *event) {
    if (!inside_plot(event->position()) || series_.empty()) {
        event->ignore();
        return;
    }
    const auto anchor = time_at(event->position().x());
    const auto factor = std::pow(1.25, -event->angleDelta().y() / 120.0);
    set_interval(anchor + (left_ - anchor) * factor, anchor + (right_ - anchor) * factor);
    event->accept();
}
void ReportPlot::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape && dragging_) {
        dragging_ = false;
        setCursor(pan_mode_ ? Qt::OpenHandCursor : Qt::CrossCursor);
        set_interval(drag_left_, drag_right_);
    } else if (event->key() == Qt::Key_Home || event->key() == Qt::Key_Escape)
        reset_view();
    else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)
        zoom(.8);
    else if (event->key() == Qt::Key_Minus)
        zoom(1.25);
    else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const auto shift = (right_ - left_) * 0.1 * (event->key() == Qt::Key_Left ? -1 : 1);
        set_interval(left_ + shift, right_ + shift);
    } else
        QWidget::keyPressEvent(event);
}
void ReportPlot::leaveEvent(QEvent *) {
    hover_ = false;
    QToolTip::hideText();
    update();
}
} // namespace orbitforge::desktop
