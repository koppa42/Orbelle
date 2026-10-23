#include "ground_station_editor.hpp"
#include "../ui_text.hpp"
#include "form_kit.hpp"
#include "project_update.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace orbelle::desktop {
GroundStationEditor::GroundStationEditor(QWidget *parent) : QDialog(parent) {
    setObjectName("groundStationProperties");
    setWindowIcon(workbench_icon(WorkbenchIcon::ground_station));
    resize(760, 640);
    setMinimumSize(320, 280);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    fields_ = new QWidget(this);
    auto *outer = new QHBoxLayout(fields_);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *compact = new QWidget(fields_);
    auto *compact_layout = new QHBoxLayout(compact);
    compact_layout->setContentsMargins(16, 12, 16, 0);
    auto *selector = new QComboBox(compact);
    selector->setObjectName("propertyPageSelector");
    selector->setAccessibleName(ui::text("facility.pages.navigation"));
    auto *tabs = new QTabBar(compact);
    tabs->setObjectName("propertyPageTabs");
    tabs->setAccessibleName(ui::text("facility.pages.navigation"));
    tabs->setDrawBase(false);
    tabs->setExpanding(false);
    tabs->setUsesScrollButtons(true);
    auto *navigation = new QTreeWidget(fields_);
    navigation->setObjectName("propertyNavigation");
    navigation->setAccessibleName(ui::text("facility.pages.navigation"));
    navigation->setFixedWidth(164);
    navigation->setHeaderHidden(true);
    navigation->setRootIsDecorated(false);
    navigation->setIndentation(16);
    navigation->setUniformRowHeights(true);
    const QStringList labels{ui::text("facility.pages.position"),
                             ui::text("facility.pages.description"),
                             ui::text("ground_station_editor.constraint_form")};
    const auto category = [&](const QString &heading) {
        auto *item = new QTreeWidgetItem(navigation, {heading});
        item->setFlags(Qt::ItemIsEnabled);
        auto font = item->font(0);
        font.setWeight(QFont::DemiBold);
        item->setFont(0, font);
        item->setExpanded(true);
        return item;
    };
    auto *basic = category(ui::text("facility.pages.basic"));
    auto *constraints = category(ui::text("facility.pages.constraints"));
    std::vector<QTreeWidgetItem *> page_items;
    for (int i = 0; i < labels.size(); ++i) {
        selector->addItem(labels[i]);
        tabs->addTab(labels[i]);
        auto *item = new QTreeWidgetItem(i == 2 ? constraints : basic,
                                         {i == 2 ? ui::text("facility.pages.basic") : labels[i]});
        item->setData(0, Qt::UserRole, i);
        page_items.push_back(item);
    }
    compact_layout->addWidget(selector, 1);
    compact_layout->addWidget(tabs);
    compact_layout->addStretch();
    auto *pages = new QStackedWidget(fields_);
    pages->setObjectName("facilityPages");
    pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    outer->addWidget(compact);
    outer->addWidget(navigation);
    outer->addWidget(pages, 1);
    outer->setDirection(QBoxLayout::TopToBottom);
    navigation->hide();
    tabs->hide();
    layout->addWidget(fields_, 1);
    connect(selector, &QComboBox::currentIndexChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(navigation, &QTreeWidget::currentItemChanged, pages, [pages](QTreeWidgetItem *item) {
        if (item && item->data(0, Qt::UserRole).isValid())
            pages->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
    });
    connect(pages, &QStackedWidget::currentChanged, this, [=](int index) {
        const QSignalBlocker a(selector), b(tabs), c(navigation);
        selector->setCurrentIndex(index);
        tabs->setCurrentIndex(index);
        navigation->setCurrentItem(page_items.at(index));
    });
    navigation->setCurrentItem(page_items.front());
    new WidthObserver(fields_, [=, mode = WidthMode{}, tight = WidthMode{}](int width) mutable {
        const bool narrow = mode.narrow(width, 680);
        const bool small = tight.narrow(width, 400);
        navigation->setVisible(!narrow);
        compact->setVisible(narrow);
        selector->setVisible(narrow && small);
        tabs->setVisible(narrow && !small);
        outer->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    });
    QWidget *body = nullptr;
    QVBoxLayout *rows = nullptr;
    const auto page = [&](const char *name, const QString &heading) {
        auto *scroll = new QScrollArea(pages);
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        body = new QWidget;
        body->setMaximumWidth(640);
        rows = new QVBoxLayout(body);
        rows->setContentsMargins(theme::space::page);
        rows->setSpacing(theme::space::sm);
        new WidthObserver(body, [rows, mode = WidthMode{}](int width) mutable {
            const auto margins = theme::space::page;
            rows->setContentsMargins(mode.narrow(width, 480)
                                         ? QMargins(16, margins.top(), 16, margins.bottom())
                                         : margins);
        });
        auto *title = new QLabel(heading, body);
        title->setObjectName("panelTitle");
        rows->addWidget(title);
        scroll->setWidget(body);
        pages->addWidget(scroll);
    };
    const auto group = [&](const QString &heading) {
        auto *frame = new FormGroup(heading, nullptr, body);
        rows->addWidget(frame);
        responsive_form(frame->form());
        return frame;
    };
    page("groundStationScroll", ui::text("ground_station_editor.title"));
    auto *identity = new QWidget(body);
    auto *identity_form = new QFormLayout(identity);
    identity_form->setContentsMargins(0, theme::space::sm, 0, theme::space::sm);
    identity_form->setHorizontalSpacing(theme::space::md);
    identity_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    responsive_form(identity_form);
    rows->addWidget(identity);
    name_ = new QLineEdit;
    name_->setObjectName("stationName");
    name_->setMaxLength(16384);
    name_->setMaximumWidth(276);
    name_->setAccessibleName(ui::text("common.object_name"));
    identity_form->addRow(ui::text("common.object_name"), name_);
    auto *position = group(ui::text("ground_station_editor.form"));
    position->set_summary(ui::text("facility.position.wgs84"));
    position->add_help(ui::text("ground_station_editor.label"));
    position->add_help(ui::text("ground_station_editor.label_2"));
    position->add_help(ui::text("facility.position.ground_help"));
    auto *form = position->form();
    position_type_box_ = new QComboBox;
    position_type_box_->setObjectName("stationPositionType");
    position_type_box_->setMaximumWidth(276);
    position_type_box_->setAccessibleName(ui::text("facility.position.type"));
    for (const auto *key : {"facility.position.geodetic", "facility.position.geocentric",
                            "facility.position.cartesian", "facility.position.spherical",
                            "facility.position.cylindrical"})
        position_type_box_->addItem(ui::text(key));
    form->addRow(ui::text("facility.position.type"), position_type_box_);
    coordinate_pages_ = new QStackedWidget;
    coordinate_pages_->setObjectName("stationCoordinatePages");
    coordinate_pages_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    form->addRow(coordinate_pages_);
    const std::array<std::array<const char *, 3>, 5> ids{
        {{"stationLatitude", "stationLongitude", "stationHeight"},
         {"stationGeocentricLatitude", "stationGeocentricLongitude", "stationGeocentricHeight"},
         {"stationX", "stationY", "stationZ"},
         {"stationSphericalLatitude", "stationSphericalLongitude", "stationRadius"},
         {"stationPolarRadius", "stationCylindricalLongitude", "stationCylindricalZ"}}};
    const std::array<std::array<QString, 3>, 5> captions{
        {{ui::text("ground_station_editor.latitude.set_label"),
          ui::text("ground_station_editor.longitude.set_label"),
          ui::text("ground_station_editor.height.set_label")},
         {ui::text("facility.position.geocentric_latitude"),
          ui::text("ground_station_editor.longitude.set_label"),
          ui::text("ground_station_editor.height.set_label")},
         {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")},
         {ui::text("facility.position.spherical_latitude"),
          ui::text("ground_station_editor.longitude.set_label"),
          ui::text("facility.position.radius")},
         {ui::text("facility.position.polar_radius"),
          ui::text("ground_station_editor.longitude.set_label"), QStringLiteral("Z")}}};
    for (int type = 0; type < 5; ++type) {
        auto *fields = new QWidget;
        fields->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
        auto *coordinates = new QFormLayout(fields);
        coordinates->setContentsMargins(0, 0, 0, 0);
        coordinates->setHorizontalSpacing(form->horizontalSpacing());
        responsive_form(coordinates);
        for (int i = 0; i < 3; ++i) {
            const bool angle = type != 2 && (i == 1 || (i == 0 && type != 4));
            auto *editor = new QuantityEditor(angle ? Unit::degree : Unit::metre);
            coordinates_[type][i] = editor;
            editor->setObjectName(ids[type][i]);
            editor->set_label(captions[type][i]);
            if (angle)
                editor->set_range(i == 0 ? -90 : -180, i == 0 ? 90 : 180, Unit::degree);
            else if (type <= 1)
                editor->set_range(-12000, 100000, Unit::metre);
            else
                editor->set_range((type == 3 || (type == 4 && i == 0)) ? 0 : -6500000, 6500000,
                                  Unit::metre);
            coordinates->addRow(captions[type][i], editor);
            connect(
                editor->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
                [this] { preview(); }, Qt::QueuedConnection);
        }
        coordinate_pages_->addWidget(fields);
    }
    above_ground_ = new QuantityEditor(Unit::metre);
    above_ground_->setObjectName("stationHeightAboveGround");
    above_ground_->set_label(ui::text("facility.position.above_ground"));
    above_ground_->set_range(0, 100000, Unit::metre);
    form->addRow(ui::text("facility.position.above_ground"), above_ground_);
    position->add_help(ui::text("facility.position.height_help"));
    preview_group_ = group(ui::text("facility.position.observer"));
    preview_group_->setObjectName("stationPreviewGroup");
    preview_group_->set_summary(QStringLiteral("ITRF"));
    preview_group_->set_collapsible(false);
    preview_ = new QLabel;
    preview_->setObjectName("stationPositionPreview");
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    preview_->setWordWrap(true);
    preview_group_->form()->addRow(preview_);
    align_form_labels(body);
    rows->addStretch();

    page("facilityDescriptionScroll", ui::text("facility.pages.description"));
    auto *help = new QLabel(ui::text("facility.description.help"), body);
    help->setWordWrap(true);
    rows->addWidget(help);
    description_ = new QPlainTextEdit(body);
    description_->setObjectName("stationDescription");
    description_->setAccessibleName(ui::text("facility.pages.description"));
    description_->setMinimumHeight(180);
    description_->setPlaceholderText(ui::text("facility.description.placeholder"));
    rows->addWidget(description_, 1);

    page("facilityConstraintsScroll", ui::text("ground_station_editor.constraint_form"));
    auto *constraint = group(ui::text("facility.pages.basic"));
    auto *los = new QLabel(ui::text("facility.constraints.los"));
    los->setWordWrap(true);
    constraint->form()->addRow(los);
    minimum_elevation_ = new QuantityEditor(Unit::degree);
    minimum_elevation_->setObjectName("stationMinimumElevation");
    minimum_elevation_->set_label(ui::text("ground_station_editor.minimum_elevation.set_label"));
    minimum_elevation_->set_range(-90, 90, Unit::degree);
    constraint->form()->addRow(ui::text("ground_station_editor.minimum_elevation.set_label"),
                               minimum_elevation_);
    constraint->add_help(ui::text("ground_station_editor.label_3"));
    align_form_labels(body);
    rows->addStretch();
    auto *footer = new QWidget;
    auto *footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(theme::space::footer);
    result_ = new QLabel;
    result_->setObjectName("stationResult");
    result_->setTextFormat(Qt::PlainText);
    result_->setWordWrap(true);
    result_->hide();
    footer_layout->addWidget(result_);
    auto *buttons = new QHBoxLayout;
    reset_ = new QPushButton(ui::text("ground_station_editor.buttons"));
    reset_->setObjectName("resetGroundStation");
    auto *close = new QPushButton(ui::text("common.close"));
    close->setAutoDefault(false);
    apply_ = new QPushButton(ui::text("common.apply"));
    apply_->setObjectName("applyGroundStation");
    apply_->setProperty("primary", true);
    apply_->setDefault(true);
    buttons->addWidget(reset_);
    buttons->addStretch();
    buttons->addWidget(close);
    footer_layout->addLayout(buttons);
    footer_layout->addWidget(apply_);
    layout->addWidget(footer);
    new WidthObserver(footer, [this, buttons, footer_layout, mode = WidthMode{},
                               previous = true](int width) mutable {
        const bool narrow = mode.narrow(width, 380);
        if (narrow == previous)
            return;
        previous = narrow;
        if (narrow) {
            buttons->removeWidget(apply_);
            footer_layout->addWidget(apply_);
        } else {
            footer_layout->removeWidget(apply_);
            buttons->addWidget(apply_);
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(reset_, &QPushButton::clicked, this, [this] { set_station(original_, creating_); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        try {
            if (apply)
                apply(station());
        } catch (const std::exception &error) {
            show_result(QString::fromUtf8(error.what()));
        }
    });
    connect(position_type_box_, &QComboBox::currentIndexChanged, this,
            &GroundStationEditor::switch_position_type);
    connect(
        above_ground_->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
        [this] { preview(); }, Qt::QueuedConnection);
    load_coordinates({});
}
void GroundStationEditor::set_station(const application::ProjectGroundStation &station,
                                      bool creating) {
    original_ = station;
    creating_ = creating;
    name_->setText(QString::fromStdString(station.name()));
    position_type_ = station.position_type;
    const QSignalBlocker block(position_type_box_);
    position_type_box_->setCurrentIndex(static_cast<int>(position_type_));
    load_coordinates(station.location);
    above_ground_->set_value(station.location.height_above_ground_m, Unit::metre);
    description_->setPlainText(QString::fromStdString(station.description));
    minimum_elevation_->set_value(station.minimum_elevation_rad, Unit::radian);
    apply_->setText(creating ? ui::text("ground_station_editor.label_4")
                             : ui::text("common.apply"));
    setWindowTitle(creating ? ui::text("ground_station_editor.label_4")
                            : QString::fromStdString(station.name()) +
                                  ui::text("ground_station_editor.label_5"));
    show_result({});
    preview();
}
application::ProjectGroundStation GroundStationEditor::station() const {
    auto value = original_;
    value.display_name = name_->text().trimmed().toStdString();
    if (value.display_name.empty())
        throw std::invalid_argument(ui::utf8("ground_station_editor.value"));
    value.location = location();
    value.position_type = position_type_;
    value.description = description_->toPlainText().toStdString();
    if (value.description.size() > 16384)
        throw std::invalid_argument(ui::utf8("facility.description.too_long"));
    value.minimum_elevation_rad = minimum_elevation_->value(Unit::radian);
    return value;
}
bool GroundStationEditor::has_unapplied_changes() const {
    try {
        return !same_station_input(station(), original_);
    } catch (const std::exception &) {
        return true;
    }
}
void GroundStationEditor::accept_station(const application::ProjectGroundStation &station) {
    original_ = station;
    creating_ = false;
    apply_->setText(ui::text("common.apply"));
}
void GroundStationEditor::sync_name(const std::string &name) {
    if (name_->text().trimmed().toStdString() == original_.name())
        name_->setText(QString::fromStdString(name));
    original_.display_name = name;
}
void GroundStationEditor::load_coordinates(core::GroundStationLocation location) {
    canonical_ = location;
    represented_ = core::ground_station_coordinates(location, position_type_);
    const auto type = static_cast<int>(position_type_);
    for (int i = 0; i < 3; ++i) {
        const bool angle = type != 2 && (i == 1 || (i == 0 && type != 4));
        coordinates_[type][i]->set_value(represented_[i], angle ? Unit::radian : Unit::metre);
    }
    coordinate_pages_->setCurrentIndex(type);
    // Hidden coordinate forms may still have their narrow-layout height. Only
    // the active form should reserve vertical space in the inspector.
    for (int i = 0; i < coordinate_pages_->count(); ++i)
        coordinate_pages_->widget(i)->setSizePolicy(
            QSizePolicy::Preferred, i == type ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    coordinate_pages_->updateGeometry();
    const auto help = position_type_ == core::GroundPositionType::geocentric
                          ? ui::text("facility.position.geocentric_help")
                          : ui::text("facility.position.ground_help");
    position_type_box_->setToolTip(help);
    position_type_box_->setAccessibleDescription(help);
}
core::GroundStationLocation GroundStationEditor::location() const {
    const auto type = static_cast<int>(position_type_);
    std::array<double, 3> values;
    for (int i = 0; i < 3; ++i) {
        const bool angle = type != 2 && (i == 1 || (i == 0 && type != 4));
        values[i] = coordinates_[type][i]->value(angle ? Unit::radian : Unit::metre);
    }
    const auto above = above_ground_->value(Unit::metre);
    // Unit/page/type switches must not introduce inverse-conversion roundoff into
    // the physical input or silently reset a longitude at the poles.
    if (values == represented_) {
        auto value = canonical_;
        value.height_above_ground_m = above;
        (void)core::ground_station_position(value);
        return value;
    }
    return core::ground_station_location(position_type_, values, above);
}
void GroundStationEditor::switch_position_type(int index) {
    try {
        const auto value = location(); // Validate the current draft before switching.
        const auto type = static_cast<core::GroundPositionType>(index);
        (void)core::ground_station_coordinates(value, type);
        position_type_ = type;
        load_coordinates(value);
        show_result({});
        preview();
    } catch (const std::exception &error) {
        const QSignalBlocker block(position_type_box_);
        position_type_box_->setCurrentIndex(static_cast<int>(position_type_));
        show_result(QString::fromUtf8(error.what()));
    }
}
void GroundStationEditor::preview() {
    try {
        const auto value = location();
        const auto xyz = core::ground_station_position(value);
        preview_->setText(QStringLiteral("X  %1 m\nY  %2 m\nZ  %3 m\n")
                              .arg(xyz[0], 0, 'f', 3)
                              .arg(xyz[1], 0, 'f', 3)
                              .arg(xyz[2], 0, 'f', 3) +
                          ui::text("facility.position.total_height")
                              .arg(value.height_m + value.height_above_ground_m, 0, 'f', 3));
    } catch (const std::exception &) {
        preview_->setText(ui::text("ground_station_editor.preview.set_text"));
    }
}
void GroundStationEditor::set_busy(bool busy) {
    fields_->setEnabled(!busy);
    apply_->setEnabled(!busy);
    reset_->setEnabled(!busy);
}
void GroundStationEditor::show_result(const QString &text) {
    result_->setText(text);
    result_->setVisible(!text.isEmpty());
}
} // namespace orbelle::desktop
