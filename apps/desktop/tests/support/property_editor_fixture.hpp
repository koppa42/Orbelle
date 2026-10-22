#pragma once

#include "../../src/viewer/orbit_editor.hpp"
#include "../../src/viewer/quantity_editor.hpp"
#include "ui_test_support.hpp"
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>
#include <gtest/gtest.h>
#include <stdexcept>

namespace orbelle::desktop {
// Preserve the public GoogleTest names so existing CTest filters and reports remain valid.
class QuantityTest : public ::testing::Test {
  protected:
    void SetUp() override { (void)test_application("quantity_test"); }
    static QLineEdit *text(QuantityEditor &editor) {
        return editor.findChild<QLineEdit *>("quantityValue");
    }
    static void choose(QuantityEditor &editor, Unit unit) {
        auto *menu = editor.findChild<QToolButton *>("quantityUnit")->menu();
        for (auto *action : menu->actions())
            if (action->data().toInt() == static_cast<int>(unit)) {
                action->trigger();
                return;
            }
        FAIL() << "Requested unit was not offered";
    }
    static QuantityEditor &field(OrbitEditor &editor, const char *name) {
        auto *result = editor.findChild<QuantityEditor *>(name);
        if (!result)
            throw std::runtime_error("Missing quantity field");
        return *result;
    }
};
} // namespace orbelle::desktop
