#include "coverage_map.hpp"
#include "workbench_style.hpp"
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbitforge::desktop {
namespace {
QColor ramp(double x) {
    const QColor a("#24364e"), b("#43b5be"), c("#e6d68a");
    x = std::clamp(x, 0., 1.);
    const QColor left = x < .5 ? a : b, right = x < .5 ? b : c;
    const double t = x < .5 ? x * 2 : (x - .5) * 2;
    return QColor::fromRgbF(left.redF() * (1 - t) + right.redF() * t,
                            left.greenF() * (1 - t) + right.greenF() * t,
                            left.blueF() * (1 - t) + right.blueF() * t);
}
constexpr double degrees = 180. / std::numbers::pi;
} // namespace
CoverageMap::CoverageMap(QWidget *parent) : QWidget(parent) {
    setObjectName("coverageMap");
    setMinimumSize(180, 180);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("覆盖热力图；左右方向键选择网格点"));
}
void CoverageMap::set_result(std::shared_ptr<const runtime::ReportResult> result,
                             bool reset_selection) {
    result_ = std::move(result);
    if (reset_selection || !result_ || !result_->coverage_statistics ||
        selected_ >= result_->coverage_statistics->points.size())
        selected_ = 0;
    update();
}
void CoverageMap::set_metric(int metric) {
    metric_ = metric;
    update();
}
void CoverageMap::select_point(std::size_t index) {
    if (!result_ || !result_->coverage_statistics ||
        index >= result_->coverage_statistics->points.size())
        return;
    selected_ = index;
    update();
    if (selected)
        selected(index);
}
QRectF CoverageMap::map_rect() const {
    QRectF rect(44, 38, std::max(1, width() - 60), std::max(1, height() - 112));
    if (!result_)
        return rect;
    const auto &g = result_->definition.query.coverage;
    const double aspect = core::coverage_longitude_span(g) / (g.north_rad - g.south_rad);
    const double w = std::min(rect.width(), rect.height() * aspect);
    const double h = std::min(rect.height(), rect.width() / aspect);
    return {rect.center().x() - w / 2, rect.center().y() - h / 2, w, h};
}
QRectF CoverageMap::cell_rect(const core::CoverageCell &c) const {
    const auto &g = result_->definition.query.coverage;
    const auto r = map_rect();
    const double x = r.width() / core::coverage_longitude_span(g);
    const double y = r.height() / (g.north_rad - g.south_rad);
    return {r.left() + (c.west_rad - g.west_rad) * x, r.top() + (g.north_rad - c.north_rad) * y,
            (c.east_rad - c.west_rad) * x, (c.north_rad - c.south_rad) * y};
}
double CoverageMap::value(const results::CoveragePoint &point) const {
    return metric_ == 0 ? 100 * point.statistics.covered_seconds /
                              result_->last.seconds_since(result_->first)
                        : point.statistics.maximum_gap_seconds / 3600.;
}
double CoverageMap::maximum() const {
    if (metric_ == 0)
        return 100;
    return result_ && result_->coverage_statistics
               ? std::max(1e-9, result_->coverage_statistics->maximum_gap_seconds / 3600.)
               : 1.;
}
void CoverageMap::draw(QPainter &p) const {
    p.fillRect(rect(), theme::color(theme::surface));
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(theme::color(theme::text));
    p.drawText(QRectF(16, 9, width() - 32, 24), Qt::AlignLeft | Qt::AlignVCenter,
               metric_ == 0 ? QStringLiteral("累计覆盖时长占比  ·  %")
                            : QStringLiteral("最长无覆盖空档  ·  h"));
    if (!result_ || !result_->coverage_statistics)
        return;
    const auto &points = result_->coverage_statistics->points;
    p.setRenderHint(QPainter::Antialiasing, false);
    for (const auto &point : points) {
        const auto r = cell_rect(point.cell);
        p.fillRect(r, ramp(value(point) / maximum()));
        if (r.width() >= 6 && r.height() >= 6) {
            p.setPen(QColor(255, 255, 255, 35));
            p.drawRect(r);
        }
    }
    const auto r = map_rect();
    p.setPen(theme::color(theme::text_muted));
    const auto &g = result_->definition.query.coverage;
    const auto deg = [](double rad) {
        return QString::number(std::remainder(rad, 2 * std::numbers::pi) * degrees, 'g', 5) +
               QChar(0x00b0);
    };
    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width() / 2, 20), Qt::AlignLeft, deg(g.west_rad));
    p.drawText(QRectF(r.center().x(), r.bottom() + 7, r.width() / 2, 20), Qt::AlignRight,
               deg(g.east_rad));
    p.drawText(QRectF(0, r.top(), r.left() - 5, 20), Qt::AlignRight, deg(g.north_rad));
    p.drawText(QRectF(0, r.bottom() - 20, r.left() - 5, 20), Qt::AlignRight, deg(g.south_rad));
    const QRectF legend(44, height() - 36, std::max(1, width() - 60), 7);
    QLinearGradient gradient(legend.topLeft(), legend.topRight());
    gradient.setColorAt(0, ramp(0));
    gradient.setColorAt(.5, ramp(.5));
    gradient.setColorAt(1, ramp(1));
    p.fillRect(legend, gradient);
    p.drawText(QRectF(legend.left(), legend.bottom() + 3, legend.width() / 2, 18), Qt::AlignLeft,
               QStringLiteral("0"));
    p.drawText(QRectF(legend.center().x(), legend.bottom() + 3, legend.width() / 2, 18),
               Qt::AlignRight, QString::number(maximum(), 'g', 5));
    if (selected_ < points.size()) {
        p.setPen(QPen(QColor("#ffffff"), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cell_rect(points[selected_].cell).adjusted(1, 1, -1, -1));
    }
}
void CoverageMap::paintEvent(QPaintEvent *) {
    QPainter p(this);
    draw(p);
}
QImage CoverageMap::image() const {
    QImage output(size() * 2, QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(2);
    QPainter p(&output);
    draw(p);
    return output;
}
void CoverageMap::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && result_ && result_->coverage_statistics) {
        const auto &points = result_->coverage_statistics->points;
        for (std::size_t i = 0; i < points.size(); ++i)
            if (cell_rect(points[i].cell).contains(event->position())) {
                select_point(i);
                return;
            }
    }
    QWidget::mousePressEvent(event);
}
void CoverageMap::mouseMoveEvent(QMouseEvent *event) {
    if (result_ && result_->coverage_statistics)
        for (const auto &point : result_->coverage_statistics->points)
            if (cell_rect(point.cell).contains(event->position())) {
                QToolTip::showText(event->globalPosition().toPoint(),
                                   QStringLiteral("经度 %1° · 纬度 %2°\n%3 %4\n面积 %5 km²")
                                       .arg(point.cell.location.longitude_rad * degrees, 0, 'f', 3)
                                       .arg(point.cell.location.latitude_rad * degrees, 0, 'f', 3)
                                       .arg(value(point), 0, 'f', 3)
                                       .arg(metric_ == 0 ? "%" : "h")
                                       .arg(point.cell.area_m2 / 1e6, 0, 'f', 1),
                                   this);
                return;
            }
    QToolTip::hideText();
}
void CoverageMap::keyPressEvent(QKeyEvent *event) {
    if (result_ && result_->coverage_statistics && !result_->coverage_statistics->points.empty()) {
        const auto count = result_->coverage_statistics->points.size();
        if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Down) {
            select_point((selected_ + 1) % count);
            return;
        }
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Up) {
            select_point((selected_ + count - 1) % count);
            return;
        }
    }
    QWidget::keyPressEvent(event);
}
} // namespace orbitforge::desktop
