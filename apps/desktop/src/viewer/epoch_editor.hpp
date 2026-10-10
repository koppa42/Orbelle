#pragma once
#include <QWidget>
#include <memory>
#include <optional>
#include <orbitforge/core/time/time_provider.hpp>
class QLineEdit;
class QLabel;
class QToolButton;
namespace orbitforge::desktop {
enum class CalendarScale { utc, tai, tt };
QString calendar_text(core::Epoch epoch, CalendarScale scale, bool date = true,
                      const core::ITimeProvider *time = nullptr);
class EpochEditor final : public QWidget {
  public:
    explicit EpochEditor(QWidget *parent = nullptr,
                         std::shared_ptr<const core::ITimeProvider> time = {});
    void set_time_provider(std::shared_ptr<const core::ITimeProvider> time);
    [[nodiscard]] core::Epoch epoch() const;
    void set_epoch(core::Epoch epoch);
    void set_scale(CalendarScale scale);
    void set_range(core::Epoch first, core::Epoch last);
    void set_read_only(bool value);
    void adjust_seconds(int seconds);
    void open_picker();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    std::shared_ptr<const core::ITimeProvider> time_;
    QLineEdit *value_ = nullptr;
    QToolButton *picker_button_ = nullptr;
    QLabel *error_ = nullptr;
    CalendarScale current_scale_ = CalendarScale::utc;
    core::Epoch stored_{0};
    std::optional<core::Epoch> first_, last_;
    bool updating_ = false, dirty_ = false;
};
} // namespace orbitforge::desktop
