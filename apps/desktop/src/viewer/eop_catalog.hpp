#pragma once
#include <QStringList>
#include <optional>
#include <orbitforge/runtime/time_frames.hpp>
#include <vector>
class QWidget;

namespace orbitforge::desktop {
class EopCatalog {
  public:
    using Provider = std::shared_ptr<const core::IEarthOrientationProvider>;
    EopCatalog(std::filesystem::path store, std::vector<Provider> bundled);
    std::size_t import_snapshot(const std::filesystem::path &manifest);
    [[nodiscard]] const std::vector<Provider> &providers() const { return providers_; }
    [[nodiscard]] const QStringList &warnings() const { return warnings_; }
    [[nodiscard]] const std::filesystem::path &store() const { return store_; }
    std::size_t preferred = 0;

  private:
    std::size_t append(Provider provider);
    std::filesystem::path store_;
    std::vector<Provider> providers_;
    QStringList warnings_;
};
// Imports remain installed if cancelled; only acceptance selects a data version.
std::optional<std::size_t>
choose_eop_snapshot(QWidget *parent, EopCatalog &catalog, const EopCatalog::Provider &current,
                    std::optional<std::pair<core::Epoch, core::Epoch>> interval);
} // namespace orbitforge::desktop
