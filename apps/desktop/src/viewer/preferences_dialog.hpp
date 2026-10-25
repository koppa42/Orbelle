#pragma once
#include "eop_catalog.hpp"
#include <QDialog>
#include <array>
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
namespace orbelle::desktop {
class NetworkSettings;
enum class PreferencesPage { network, updates, data, insertion };
class PreferencesDialog final : public QDialog {
  public:
    explicit PreferencesDialog(NetworkSettings &settings, EopCatalog *catalog = nullptr,
                               QWidget *parent = nullptr,
                               PreferencesPage page = PreferencesPage::network,
                               std::optional<DataProduct> product = {});

  private:
    bool apply();
    void changed();
    NetworkSettings &settings_;
    EopCatalog *catalog_;
    QStackedWidget *pages_;
    QComboBox *mode_;
    QLineEdit *address_;
    QLabel *error_ = nullptr;
    QPushButton *apply_ = nullptr;
    QCheckBox *startup_, *keep_open_;
    std::array<QComboBox *, 4> insertion_;
    DataPreferences data_draft_;
};
} // namespace orbelle::desktop
