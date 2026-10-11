#pragma once
#include <QDialog>
#include <functional>
#include <memory>
#include <optional>
#include <orbitforge/runtime/report.hpp>

namespace orbitforge::desktop {
class ReportDialog final : public QDialog {
  public:
    using Ready = std::function<void(std::optional<runtime::ReportContext>, QString)>;
    using Prepare = std::function<void(Ready)>;
    using Save = std::function<void(std::vector<application::ReportDefinition>)>;
    ReportDialog(application::Project project, std::shared_ptr<const core::ITimeProvider> time,
                 Prepare prepare, Save save, std::function<void(core::Epoch)> seek,
                 QWidget *parent = nullptr);
    ~ReportDialog() override;
    [[nodiscard]] QSize minimumSizeHint() const override { return {320, 300}; }
    void select_object(const std::string &id, application::ReportKind kind);
    void update_project(application::Project project,
                        std::shared_ptr<const core::ITimeProvider> time);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbitforge::desktop
