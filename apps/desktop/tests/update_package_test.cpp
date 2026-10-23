#include "../src/viewer/update_package.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <gtest/gtest.h>
#include <minizip/zip.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace orbelle::desktop {
namespace {
QByteArray hash(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
void write(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        throw std::runtime_error("Cannot create fixture directory");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write fixture file");
}
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read fixture file");
    return file.readAll();
}
void ownership(QMap<QString, QByteArray> &contents) {
    contents.remove("orbelle-managed-files.txt");
    contents.remove("orbelle-managed.txt");
    QByteArray list;
    for (auto it = contents.cbegin(); it != contents.cend(); ++it)
        list += hash(it.value()).toHex() + "  " + it.key().toUtf8() + '\n';
    contents.insert("orbelle-managed-files.txt", list);
    contents.insert("orbelle-managed.txt", "1\n" + hash(list).toHex() + '\n');
}
struct ZipEntry {
    QString path;
    QByteArray bytes;
    uLong attributes = uLong(0100644) << 16;
    uLong flags = 0;
};
void archive(const QString &path, const std::vector<ZipEntry> &entries) {
    const auto encoded = QFile::encodeName(path);
    const auto handle = zipOpen64(encoded.constData(), APPEND_STATUS_CREATE);
    if (!handle)
        throw std::runtime_error("Cannot create fixture archive");
    for (const auto &entry : entries) {
        zip_fileinfo info{};
        info.external_fa = entry.attributes;
        const auto name = entry.path.toUtf8();
        const auto status = zipOpenNewFileInZip4_64(
            handle, name.constData(), &info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED,
            Z_DEFAULT_COMPRESSION, 0, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY, nullptr, 0, (3 << 8) | 45,
            entry.flags | (1 << 11), 1);
        if (status != ZIP_OK ||
            zipWriteInFileInZip(handle, entry.bytes.constData(), unsigned(entry.bytes.size())) !=
                ZIP_OK ||
            zipCloseFileInZip(handle) != ZIP_OK) {
            zipClose(handle, nullptr);
            throw std::runtime_error("Cannot write fixture archive");
        }
    }
    if (zipClose(handle, nullptr) != ZIP_OK)
        throw std::runtime_error("Cannot close fixture archive");
}
class UpdatePackage : public testing::Test {
  protected:
    QTemporaryDir temp;
    QString root;
    QString current;
    QMap<QString, QByteArray> contents;
    UpdateManifest manifest;
    QMap<QString, QString> downloads;
    std::atomic_bool cancel{false};

    void SetUp() override {
        ASSERT_TRUE(temp.isValid());
        root = temp.filePath("portable");
        current = QDir(root).filePath("versions/0.3.1");
        contents = {{"OrbelleApp.exe", "new application"},
                    {"OrbelleUpdater.exe", "new helper"},
                    {"OrbelleLauncher.exe", "new launcher"},
                    {"qt.conf", "[Paths]\nPlugins=plugins\n"},
                    {"manifest.json", "{\"version\":\"0.3.2\"}\n"},
                    {"Qt6Core.dll", "shared runtime"},
                    {"plugins/platforms/qwindows.dll", "shared platform plugin"},
                    {"resources/earth/map.png", "shared earth image"}};
        ownership(contents);
        manifest.version = "0.3.2";
        for (auto it = contents.cbegin(); it != contents.cend(); ++it) {
            const auto bundle = it.key().startsWith("resources/") ? "resources"
                                : it.key().startsWith("plugins/") || it.key().endsWith(".dll")
                                    ? "runtime"
                                    : "app";
            manifest.files.push_back(
                {it.key(), bundle, quint64(it.value().size()), hash(it.value())});
        }
        for (const auto *id : {"app", "runtime", "resources"}) {
            manifest.bundles.push_back(
                {id, QString("Orbelle-0.3.2-win-x64-%1.zip").arg(id), 0, {}});
            rebuild(id, entries(id));
        }
        auto old = contents;
        old["OrbelleApp.exe"] = "old application";
        old["manifest.json"] = "{\"version\":\"0.3.1\"}\n";
        ownership(old);
        for (auto it = old.cbegin(); it != old.cend(); ++it)
            write(QDir(current).filePath(it.key()), it.value());
        write(QDir(root).filePath("Orbelle.exe"), "stable launcher");
        write(QDir(root).filePath("orbelle-layout.txt"), "1\n");
        write(QDir(root).filePath("active-version.txt"), "0.3.1\n");
        write(QDir(root).filePath("user-project.orbelle"), "user project");
    }
    std::vector<ZipEntry> entries(const QString &bundle) const {
        std::vector<ZipEntry> result;
        for (const auto &file : manifest.files)
            if (file.bundle == bundle)
                result.push_back({file.path, contents.value(file.path)});
        return result;
    }
    void rebuild(const QString &id, const std::vector<ZipEntry> &items) {
        const auto path = temp.filePath(id + ".zip");
        archive(path, items);
        downloads[id] = path;
        refresh_digest(id);
    }
    void refresh_digest(const QString &id) {
        const auto bytes = read(downloads.value(id));
        for (auto &bundle : manifest.bundles)
            if (bundle.id == id) {
                bundle.size = quint64(bytes.size());
                bundle.sha256 = hash(bytes);
            }
    }
    static QByteArray json(const UpdateManifest &value) {
        QJsonArray bundles, files;
        for (const auto &bundle : value.bundles)
            bundles.push_back(QJsonObject{{"id", bundle.id},
                                          {"name", bundle.name},
                                          {"size", qint64(bundle.size)},
                                          {"sha256", QString::fromLatin1(bundle.sha256.toHex())}});
        for (const auto &file : value.files)
            files.push_back(QJsonObject{{"path", file.path},
                                        {"bundle", file.bundle},
                                        {"size", qint64(file.size)},
                                        {"sha256", QString::fromLatin1(file.sha256.toHex())}});
        return QJsonDocument(QJsonObject{{"schema", 1},
                                         {"layout", 1},
                                         {"platform", "win-x64"},
                                         {"version", value.version},
                                         {"files", files},
                                         {"bundles", bundles}})
            .toJson(QJsonDocument::Compact);
    }
    void unchanged() const {
        EXPECT_EQ(read(QDir(current).filePath("OrbelleApp.exe")), "old application");
        EXPECT_EQ(read(QDir(root).filePath("Orbelle.exe")), "stable launcher");
        EXPECT_EQ(read(QDir(root).filePath("active-version.txt")), "0.3.1\n");
        EXPECT_EQ(read(QDir(root).filePath("user-project.orbelle")), "user project");
        EXPECT_TRUE(QDir(QDir(root).filePath("versions"))
                        .entryList({".staging-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                        .isEmpty());
    }
};

TEST_F(UpdatePackage, ParsesManifestAndRejectsUnsafeOrAmbiguousPaths) {
    const auto parsed = parse_update_manifest(json(manifest), "0.3.2");
    EXPECT_EQ(parsed.files.size(), contents.size());
    EXPECT_EQ(parsed.bundles.size(), 3u);
    EXPECT_THROW(parse_update_manifest(json(manifest), "0.3.3"), std::runtime_error);
    auto missing_launcher = manifest;
    std::erase_if(missing_launcher.files,
                  [](const auto &file) { return file.path == "OrbelleLauncher.exe"; });
    EXPECT_THROW(parse_update_manifest(json(missing_launcher), "0.3.2"), std::runtime_error);
    for (const auto *path : {"../escape", "/absolute", "C:/drive", "a\\b", "a//b", "a/../b",
                             "CON.txt", "folder/COM1.bin", "folder/name.", "folder/name ",
                             "folder/file:stream", "folder/?.dll"}) {
        SCOPED_TRACE(path);
        auto bad = manifest;
        bad.files.back().path = path;
        EXPECT_THROW(parse_update_manifest(json(bad), "0.3.2"), std::runtime_error);
    }
    auto duplicate = manifest;
    duplicate.files.push_back(duplicate.files.front());
    duplicate.files.back().path = duplicate.files.front().path.toLower();
    EXPECT_THROW(parse_update_manifest(json(duplicate), "0.3.2"), std::runtime_error);
    auto collision = manifest;
    collision.files.back().path = "Qt6Core.dll/child";
    EXPECT_THROW(parse_update_manifest(json(collision), "0.3.2"), std::runtime_error);
    auto oversized = manifest;
    oversized.files.back().size = quint64(8) * 1024 * 1024 * 1024;
    EXPECT_THROW(parse_update_manifest(json(oversized), "0.3.2"), std::runtime_error);
}

TEST_F(UpdatePackage, RequiresPackageInventoryInApplicationBundleForNativeCleanup) {
    auto missing = manifest;
    std::erase_if(missing.files, [](const auto &file) { return file.path == "manifest.json"; });
    EXPECT_THROW(parse_update_manifest(json(missing), "0.3.2"), std::runtime_error);
    auto wrong_bundle = manifest;
    for (auto &file : wrong_bundle.files)
        if (file.path == "manifest.json")
            file.bundle = "runtime";
    EXPECT_THROW(parse_update_manifest(json(wrong_bundle), "0.3.2"), std::runtime_error);
    ASSERT_TRUE(QFile::remove(QDir(current).filePath("manifest.json")));
    EXPECT_TRUE(determine_install_root(current).isEmpty());
    unchanged();
}

TEST_F(UpdatePackage, PlansReuseByHashRatherThanFileNameOrSize) {
    EXPECT_EQ(plan_update(manifest, current, cancel), QStringList{"app"});
    const auto path = QDir(current).filePath("resources/earth/map.png");
    auto damaged = read(path);
    damaged[0] = char(damaged[0] ^ 1);
    write(path, damaged);
    EXPECT_EQ(plan_update(manifest, current, cancel), (QStringList{"app", "resources"}));
    unchanged();
}

TEST_F(UpdatePackage, StagesOnlyChangedBundleAndLeavesActivationAndUserFilesUntouched) {
    std::vector<UpdatePhase> phases;
    const auto target =
        stage_update(manifest, current, root, {{"app", downloads.value("app")}}, cancel,
                     [&](const auto &progress) { phases.push_back(progress.phase); });
    EXPECT_EQ(target, QDir(root).filePath("versions/0.3.2"));
    for (auto it = contents.cbegin(); it != contents.cend(); ++it)
        EXPECT_EQ(read(QDir(target).filePath(it.key())), it.value());
    EXPECT_NE(std::find(phases.begin(), phases.end(), UpdatePhase::reusing), phases.end());
    EXPECT_NE(std::find(phases.begin(), phases.end(), UpdatePhase::extracting), phases.end());
    EXPECT_EQ(determine_install_root(current), root);
    EXPECT_EQ(determine_install_root(target), root);
    EXPECT_EQ(stage_update(manifest, current, root, {}, cancel), target);
    unchanged();
}

TEST_F(UpdatePackage, RejectsChangedDownloadBeforeExtractionAndCleansStaging) {
    const auto path = downloads.value("app");
    write(path, read(path) + "corrupt suffix");
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    EXPECT_FALSE(QFileInfo::exists(QDir(root).filePath("versions/0.3.2")));
    unchanged();
}

TEST_F(UpdatePackage, ExtractsNestedRuntimeAndResourcesFromUnicodeDownloadPaths) {
    write(QDir(current).filePath("Qt6Core.dll"), "old runtime");
    write(QDir(current).filePath("resources/earth/map.png"), "old earth image");
    EXPECT_EQ(plan_update(manifest, current, cancel), (QStringList{"app", "runtime", "resources"}));
    for (auto it = downloads.begin(); it != downloads.end(); ++it) {
        const auto destination = temp.filePath(it.key() + QString::fromUtf8("-\xc3\xa9.zip"));
        ASSERT_TRUE(QFile::rename(it.value(), destination));
        it.value() = destination;
    }
    const auto target = stage_update(manifest, current, root, downloads, cancel);
    for (auto it = contents.cbegin(); it != contents.cend(); ++it)
        EXPECT_EQ(read(QDir(target).filePath(it.key())), it.value());
    EXPECT_EQ(read(QDir(current).filePath("Qt6Core.dll")), "old runtime");
    EXPECT_EQ(read(QDir(current).filePath("resources/earth/map.png")), "old earth image");
    unchanged();
}

TEST_F(UpdatePackage, RejectsExcessiveExpansionEvenWithMatchingManifestHashes) {
    contents["OrbelleApp.exe"] = QByteArray(2 * 1024 * 1024, '\0');
    ownership(contents);
    for (auto &file : manifest.files) {
        file.size = quint64(contents.value(file.path).size());
        file.sha256 = hash(contents.value(file.path));
    }
    rebuild("app", entries("app"));
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    unchanged();
}

TEST_F(UpdatePackage, RejectsUnsafeArchiveNamesTypesEncryptionAndDuplicates) {
    const auto valid = entries("app");
    for (const auto *path : {"../escape", "C:/escape", "folder\\escape", "AUX.txt"}) {
        auto bad = valid;
        bad.front().path = path;
        rebuild("app", bad);
        EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
        unchanged();
    }
    for (const auto attributes : {uLong(0120777) << 16, uLong(0100644) << 16 | 0x400}) {
        auto bad = valid;
        bad.front().attributes = attributes;
        rebuild("app", bad);
        EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
        unchanged();
    }
    auto encrypted = valid;
    encrypted.front().flags = 1;
    rebuild("app", encrypted);
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    auto duplicate = valid;
    duplicate.back().path = valid.front().path.toLower();
    rebuild("app", duplicate);
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    unchanged();
}

TEST_F(UpdatePackage, ChecksFileHashAndZipCrcEvenWhenArchiveDigestMatches) {
    auto bad = entries("app");
    bad.front().bytes[0] = char(bad.front().bytes[0] ^ 1);
    rebuild("app", bad);
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    unchanged();
    rebuild("app", entries("app"));
    const auto path = downloads.value("app");
    auto bytes = read(path);
    const auto central = bytes.indexOf(QByteArray("PK\x01\x02", 4));
    ASSERT_GE(central, 0);
    bytes[central + 16] = char(bytes[central + 16] ^ 1);
    write(path, bytes);
    refresh_digest("app");
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    unchanged();
}

TEST_F(UpdatePackage, RejectsIncompleteOwnershipAndExistingDamagedVersions) {
    contents["orbelle-managed-files.txt"] =
        contents.value("orbelle-managed-files.txt").split('\n').front() + '\n';
    contents["orbelle-managed.txt"] =
        "1\n" + hash(contents.value("orbelle-managed-files.txt")).toHex() + '\n';
    for (auto &file : manifest.files) {
        file.size = quint64(contents.value(file.path).size());
        file.sha256 = hash(contents.value(file.path));
    }
    rebuild("app", entries("app"));
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    const auto target = QDir(root).filePath("versions/0.3.2");
    write(QDir(target).filePath("OrbelleApp.exe"), "do not overwrite");
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    EXPECT_EQ(read(QDir(target).filePath("OrbelleApp.exe")), "do not overwrite");
    unchanged();
}

TEST_F(UpdatePackage, CancellationDuringExtractionRemovesOnlyItsStagingDirectory) {
    const auto unrelated = QDir(root).filePath("versions/.staging-unrelated/keep.txt");
    write(unrelated, "other operation");
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel,
                              [&](const auto &progress) {
                                  if (progress.phase == UpdatePhase::extracting)
                                      cancel.store(true);
                              }),
                 UpdateCancelled);
    EXPECT_EQ(read(unrelated), "other operation");
    EXPECT_FALSE(QFileInfo::exists(QDir(root).filePath("versions/0.3.2")));
    ASSERT_TRUE(QFile::remove(unrelated));
    ASSERT_TRUE(QDir().rmdir(QFileInfo(unrelated).absolutePath()));
    unchanged();
    EXPECT_THROW(plan_update(manifest, current, cancel), UpdateCancelled);
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), UpdateCancelled);
}

TEST_F(UpdatePackage, PortableDetectionRejectsUnmarkedAndInvalidLayouts) {
    EXPECT_TRUE(determine_install_root(temp.path()).isEmpty());
    write(QDir(root).filePath("orbelle-layout.txt"), "2\n");
    EXPECT_TRUE(determine_install_root(current).isEmpty());
    write(QDir(root).filePath("orbelle-layout.txt"), "1\n");
    ASSERT_TRUE(QFile::remove(QDir(root).filePath("Orbelle.exe")));
    EXPECT_TRUE(determine_install_root(current).isEmpty());
}

TEST_F(UpdatePackage, RejectsLinkedReuseAndDestinationDirectories) {
    const auto outside = temp.filePath("outside");
    write(QDir(outside).filePath("map.png"), contents.value("resources/earth/map.png"));
    const auto link = [](const QString &source, const QString &destination) {
#ifdef Q_OS_WIN
        return CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(destination.utf16()),
                                   reinterpret_cast<LPCWSTR>(source.utf16()),
                                   SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) != 0;
#else
        return QFile::link(source, destination);
#endif
    };
    const auto alias = QDir(current).filePath("resources-alias");
    if (!link(outside, alias))
        GTEST_SKIP() << "Creating directory symbolic links is unavailable";
    for (auto &file : manifest.files)
        if (file.path == "resources/earth/map.png")
            file.path = "resources-alias/map.png";
    EXPECT_THROW(plan_update(manifest, current, cancel), std::runtime_error);
    ASSERT_TRUE(link(outside, QDir(root).filePath("versions/0.3.2")));
    EXPECT_THROW(stage_update(manifest, current, root, downloads, cancel), std::runtime_error);
    EXPECT_EQ(read(QDir(outside).filePath("map.png")), "shared earth image");
    unchanged();
}
} // namespace
} // namespace orbelle::desktop
