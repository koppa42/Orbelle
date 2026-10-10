#pragma once
#include "workbench_style.hpp"
#include <QAbstractSpinBox>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSpinBox>

namespace orbitforge::desktop {
// Keep native stepping and keyboard behavior; paint crisp arrows over the QSS button surfaces.
template <class Base> class WorkbenchSpinBox final : public Base {
  public:
    using Base::Base;

  protected:
    void paintEvent(QPaintEvent *event) override {
        Base::paintEvent(event);
        QStyleOptionSpinBox option;
        this->initStyleOption(&option);
        QPainter painter(this);
        static const auto up = workbench_icon(WorkbenchIcon::chevron_up);
        static const auto down = workbench_icon(WorkbenchIcon::chevron_down);
        for (const bool increase : {true, false}) {
            const auto area = this->style()->subControlRect(
                QStyle::CC_SpinBox, &option,
                increase ? QStyle::SC_SpinBoxUp : QStyle::SC_SpinBoxDown, this);
            const auto step =
                increase ? QAbstractSpinBox::StepUpEnabled : QAbstractSpinBox::StepDownEnabled;
            const bool enabled = this->isEnabled() && (option.stepEnabled & step);
            (increase ? up : down)
                .paint(&painter, QRect(area.center() - QPoint(8, 8), QSize(16, 16)),
                       Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
        }
    }
};
} // namespace orbitforge::desktop
