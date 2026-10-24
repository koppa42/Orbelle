#pragma once
#include "object_selection.hpp"
#include "report_selection.hpp"
#include <memory>
#include <optional>

namespace orbelle::desktop {
QString report_kind_name(application::ReportKind kind);
ReportChoice report_kind_choice(application::ReportKind kind);
std::vector<ObjectChoice> report_object_choices(const application::Project &project,
                                                application::ReportKind kind);

// Type and bindings stay local until the complete flow is accepted.
class ReportCreationDialog final : public QDialog {
  public:
    ReportCreationDialog(application::Project project, std::string context = {},
                         std::optional<application::ReportKind> preset = {},
                         QWidget *parent = nullptr);
    ~ReportCreationDialog() override;
    void update_project(application::Project project);
    [[nodiscard]] application::ReportDefinition definition() const;
    void accept() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbelle::desktop
