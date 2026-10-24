// Included after Workbench in graphics_workbench.cpp; these helpers need its private state.

void Workbench::smoke_require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int Workbench::smoke_report_column(const QAbstractItemModel *model, const char *id) {
    for (int c = 0; c < model->columnCount(); ++c)
        if (model->headerData(c, Qt::Horizontal, Qt::UserRole).toString() == id)
            return c;
    throw std::runtime_error(std::string("Missing report field: ") + id);
}

void Workbench::smoke_prepare_report_creation(const QString &key, bool all) {
    QTimer::singleShot(0, this, [this, key, all] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        smoke_require(dialog && dialog->objectName() == "reportCreationDialog",
                      "New report did not start with the report type selector");
        auto *list = dialog->findChild<QListWidget *>("reportSelectionList");
        auto *next = dialog->findChild<QPushButton *>("reportCreationNext");
        smoke_require(list && next, "Report creation controls are missing");
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toString() == key)
                list->setCurrentRow(row);
        smoke_require(next->isEnabled(), "Requested report type is unavailable");
        dialog->grab().save(smoke_dir_ +
                            (all ? "/report-create-types.png" : "/report-create-context.png"));
        next->click();
        if (dialog->isVisible()) {
            if (all) {
                auto *field = dialog->findChild<ObjectSelectionField *>("reportCreationObject0");
                smoke_require(field, "Report object field is missing");
                QTimer::singleShot(0, this, [this] {
                    auto *picker =
                        qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
                    smoke_require(picker, "Report object field did not open the shared selector");
                    auto *scope = picker->findChild<QCheckBox *>("objectSelectionAll");
                    smoke_require(scope, "Explicit report scope option is missing");
                    scope->setChecked(true);
                    picker->grab().save(smoke_dir_ + "/report-create-picker.png");
                    picker->accept();
                });
                field->click();
            }
            dialog->grab().save(smoke_dir_ + (all ? "/report-create-objects.png"
                                                  : "/report-create-context-objects.png"));
            smoke_require(next->isEnabled(), "Explicit report object selection was rejected");
            next->click();
        }
        smoke_require(dialog->result() == QDialog::Accepted, "Report creation did not finish");
    });
}

void Workbench::smoke_choose_report(const QString &key) {
    auto *button = report_dialog_->findChild<QPushButton *>("reportLibrary");
    smoke_require(button != nullptr, "Report selection button is missing");
    bool selected = false;
    QTimer::singleShot(0, this, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto *list = dialog->findChild<QListWidget *>("reportSelectionList");
        auto *confirm = dialog->findChild<QPushButton *>("reportSelectionConfirm");
        if (list && confirm)
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toString() == key) {
                    list->setCurrentRow(i);
                    if (confirm->isEnabled()) {
                        selected = true;
                        confirm->click();
                        return;
                    }
                }
        dialog->reject();
    });
    timer_.stop();
    button->click();
    timer_.start();
    smoke_require(selected, "Report selection window did not accept the requested choice");
}

void Workbench::smoke_edit_covariance(double variance, bool accept) {
    bool visited = false;
    QTimer::singleShot(0, this, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        smoke_require(dialog && dialog->objectName() == "hpopSettingsDialog",
                      "HPOP settings did not open");
        visited = true;
        dialog->findChild<QCheckBox *>("hpopComputeCovariance")->setChecked(true);
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(2);
        auto *matrix = dialog->findChild<QTableWidget *>("hpopCovarianceMatrix");
        for (int i = 0; i < 6; ++i)
            matrix->item(i, i)->setText(i < 3 ? QString::number(variance) : "1");
        dialog->findChild<QDialogButtonBox *>()
            ->button(accept ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)
            ->click();
    });
    timer_.stop();
    editor_->open_propagator_details();
    timer_.start();
    smoke_require(visited, "Covariance editor was not visited");
}

void Workbench::smoke_record(const char *kind) {
    interaction_report_.append(QJsonObject{
        {"kind", kind},
        {"passed", true},
        {"revision",
         static_cast<qint64>(orbit_ ? orbit_->track.samples().provenance().input_revision : 0)},
        {"rendered_tai_ns", QString::number(snapshot_.epoch.tai_nanoseconds())}});
}

bool Workbench::smoke_caches_ready() {
    return std::all_of(satellites_.begin(), satellites_.end(), [&](const auto &satellite) {
        const auto wanted = display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
        return cache_covers(*satellite.orbit, wanted.first, wanted.second);
    });
}

bool Workbench::smoke_action_in_workspace(const char *name) {
    for (auto *button : editor_->findChildren<QPushButton *>(name))
        if (button->isVisible() &&
            workspace_->rect().contains(
                QRect(button->mapTo(workspace_, QPoint{}), button->size())) &&
            button->visibleRegion().contains(button->rect()))
            return true;
    return false;
}

void Workbench::smoke_confirm_delete(bool accepted) {
    QTimer::singleShot(0, [accepted] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(accepted ? QMessageBox::Yes : QMessageBox::Cancel)->click();
    });
    delete_action_->trigger();
}

void Workbench::smoke_answer_data(bool accepted, const char *name) {
    QTimer::singleShot(0, [accepted, name] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        smoke_require(box && box->objectName() == name, "Missing data approval/coverage dialog");
        box->button(accepted ? QMessageBox::Yes : QMessageBox::Cancel)->click();
    });
}

EpochEditor *Workbench::smoke_scenario_epoch_control(const char *name) {
    return dynamic_cast<EpochEditor *>(scenario_properties_->findChild<QWidget *>(name));
}

void Workbench::smoke_answer_property(const char *name, QMessageBox::StandardButton choice,
                                      bool save_available) {
    QTimer::singleShot(0, [this, name, choice, save_available] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        smoke_require(box && box->objectName() == name, "Missing property draft confirmation");
        smoke_require(bool(box->button(QMessageBox::Save)) == save_available,
                      "Draft confirmation offered the wrong save action");
        smoke_require(box->defaultButton() == box->button(QMessageBox::Cancel),
                      "Discarding a draft must not be the default action");
        smoke_require(box->grab().save(smoke_dir_ + QString("/draft-confirmation-%1-%2.png")
                                                        .arg(name)
                                                        .arg(save_available ? "save" : "discard")),
                      "Cannot capture the property draft confirmation");
        box->button(choice)->click();
    });
}
