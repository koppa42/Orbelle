#include "../src/ui_text.hpp"
#include <QDir>
#include <QTemporaryDir>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>

namespace orbelle::desktop {
TEST(DesktopText, EmbeddedCatalogueDoesNotDependOnWorkingDirectory) {
    const auto previous = QDir::currentPath();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir::setCurrent(directory.path()));
    EXPECT_EQ(ui::text("common.close_page"), QString::fromUtf8("关闭页面"));
    EXPECT_EQ(ui::text("window.maximize"), QString::fromUtf8("最大化"));
    EXPECT_TRUE(QDir::setCurrent(previous));
}
TEST(DesktopText, Utf8StorageAndConcurrentLookupsStayStable) {
    const auto *original = ui::utf8("window.restore");
    auto lookup = std::async(std::launch::async, [] {
        for (int i = 0; i < 1000; ++i)
            if (ui::text("window.restore") != QString::fromUtf8(ui::utf8("window.restore")))
                return false;
        return true;
    });
    for (int i = 0; i < 1000; ++i)
        EXPECT_EQ(ui::utf8("window.restore"), original);
    EXPECT_TRUE(lookup.get());
    EXPECT_STREQ(original, "还原");
}
TEST(DesktopText, MissingKeyFailsExplicitly) {
    EXPECT_THROW((void)ui::text("missing.key"), std::out_of_range);
    EXPECT_THROW((void)ui::utf8("missing.key"), std::out_of_range);
}
TEST(DesktopText, PlaceholdersUnicodeAndLineBreaksArePreserved) {
    EXPECT_EQ(ui::text("coverage_map.label_4").arg("12", "34", "h", "5", "678"),
              QString::fromUtf8("经度 12° · 纬度 34°\nh 5\n面积 678 km²"));
    EXPECT_EQ(ui::text("common.object_id_hint").arg("station-1"),
              QString::fromUtf8("ID：station-1"));
}
} // namespace orbelle::desktop
