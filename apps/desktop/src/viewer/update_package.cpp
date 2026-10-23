#include "update_package.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <minizip/unzip.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#elif defined(Q_OS_LINUX)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace orbelle::desktop {
namespace {
constexpr quint64 max_bytes = quint64(8) * 1024 * 1024 * 1024;
constexpr qsizetype max_files = 50000;
constexpr qsizetype max_metadata_bytes = 16 * 1024 * 1024;
constexpr qsizetype buffer_bytes = 256 * 1024;
const QString managed_list = QStringLiteral("orbelle-managed-files.txt");
const QString managed_marker = QStringLiteral("orbelle-managed.txt");

[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
void cancelled(const std::atomic_bool &cancel) {
    if (cancel.load(std::memory_order_relaxed))
        throw UpdateCancelled();
}
void report(const UpdateProgressCallback &progress, UpdatePhase phase, const QString &path,
            quint64 done, quint64 total) {
    if (progress)
        progress({phase, path, done, total});
}
bool valid_version(const QString &version) {
    static const QRegularExpression expression(
        QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
    return version.size() <= 64 && expression.match(version).hasMatch();
}
QByteArray digest_text(const QString &text) {
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    if (!expression.match(text).hasMatch())
        fail("Invalid SHA-256 digest");
    return QByteArray::fromHex(text.toLatin1());
}
void check_relative_path(const QString &path) {
    if (path.isEmpty() || path.size() > 1024 || path.contains('\\') || path.startsWith('/') ||
        path.normalized(QString::NormalizationForm_C) != path)
        fail("Unsafe update path: " + path);
    static const QRegularExpression device(
        QStringLiteral("^(CON|PRN|AUX|NUL|CLOCK\\$|CONIN\\$|CONOUT\\$|COM[1-9¹²³]|LPT[1-9¹²³])$"),
        QRegularExpression::CaseInsensitiveOption);
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.size() > 255 ||
            part.endsWith('.') || part.endsWith(' ') ||
            device.match(part.section('.', 0, 0)).hasMatch())
            fail("Unsafe update path: " + path);
        for (const auto ch : part)
            if (ch.unicode() < 32 || ch.unicode() == 127 || QStringLiteral("<>:\"|?*").contains(ch))
                fail("Unsafe update path: " + path);
    }
}
QString absolute(const QString &path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
void plain_path(const QString &path) {
    auto current = absolute(path);
    for (;;) {
        const QFileInfo info(current);
        if (info.isSymLink())
            fail("Symbolic link in update path: " + current);
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            fail("Reparse point in update path: " + current);
#endif
        const auto parent = info.absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
}
void directory(const QString &path) {
    plain_path(path);
    if (!QFileInfo(path).isDir())
        fail("Missing update directory: " + path);
}
QByteArray read_small(const QString &path, qsizetype limit = max_metadata_bytes) {
    plain_path(path);
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly) || file.size() > limit)
        fail("Cannot read update metadata: " + path);
    const auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || !file.atEnd() || file.error() != QFileDevice::NoError)
        fail("Invalid update metadata: " + path);
    return bytes;
}
quint64 json_size(const QJsonValue &value) {
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > double(max_bytes) ||
        std::floor(number) != number)
        fail("Invalid update size");
    return quint64(number);
}
quint64 total_size(const UpdateManifest &manifest) {
    quint64 total = 0;
    for (const auto &file : manifest.files)
        total += file.size;
    return total;
}
void validate(const UpdateManifest &manifest) {
    if (!valid_version(manifest.version) || manifest.bundles.size() != 3 ||
        manifest.files.empty() || manifest.files.size() > size_t(max_files))
        fail("Invalid update manifest");
    QSet<QString> bundle_ids;
    for (const auto &bundle : manifest.bundles) {
        if ((bundle.id != "app" && bundle.id != "runtime" && bundle.id != "resources") ||
            bundle_ids.contains(bundle.id) || bundle.size == 0 || bundle.size > max_bytes ||
            bundle.sha256.size() != 32 ||
            bundle.name != QString("Orbelle-%1-win-x64-%2.zip").arg(manifest.version, bundle.id))
            fail("Invalid update bundle");
        bundle_ids.insert(bundle.id);
    }
    QMap<QString, QString> aliases;
    QSet<QString> files;
    QSet<QString> used_bundles;
    quint64 total = 0;
    for (const auto &file : manifest.files) {
        check_relative_path(file.path);
        if (!bundle_ids.contains(file.bundle) || file.sha256.size() != 32 ||
            file.size > max_bytes || total > max_bytes - file.size ||
            files.contains(file.path.toCaseFolded()))
            fail("Invalid or duplicate update file: " + file.path);
        total += file.size;
        files.insert(file.path.toCaseFolded());
        used_bundles.insert(file.bundle);
        QString prefix;
        for (const auto &part : file.path.split('/')) {
            prefix += (prefix.isEmpty() ? QString{} : QStringLiteral("/")) + part;
            const auto key = prefix.toCaseFolded();
            if (aliases.contains(key) && aliases.value(key) != prefix)
                fail("Case-colliding update path: " + file.path);
            aliases.insert(key, prefix);
        }
    }
    for (auto it = aliases.cbegin(); it != aliases.cend(); ++it) {
        auto parent = it.key();
        while (parent.contains('/')) {
            parent.truncate(parent.lastIndexOf('/'));
            if (files.contains(parent))
                fail("File/directory collision in update manifest");
        }
    }
    for (const auto &required :
         {QStringLiteral("OrbelleApp.exe"), QStringLiteral("OrbelleUpdater.exe"),
          QStringLiteral("OrbelleLauncher.exe"), QStringLiteral("qt.conf"),
          QStringLiteral("manifest.json"), managed_list, managed_marker}) {
        const auto found = std::find_if(manifest.files.begin(), manifest.files.end(),
                                        [&](const auto &file) { return file.path == required; });
        if (found == manifest.files.end() || found->bundle != "app")
            fail("Missing application update file: " + required);
        if ((required == managed_list || required == managed_marker) &&
            found->size > quint64(max_metadata_bytes))
            fail("Oversized update ownership metadata");
    }
    if (used_bundles != bundle_ids)
        fail("Empty update bundle");
}
QByteArray hash_file(QFile &file, const QString &path, const std::atomic_bool &cancel,
                     const UpdateProgressCallback &progress = {}, quint64 base = 0,
                     quint64 total = 0) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto initial_size = file.size();
    if (initial_size < 0 || quint64(initial_size) > max_bytes)
        fail("Oversized update file: " + path);
    QByteArray buffer(buffer_bytes, Qt::Uninitialized);
    for (;;) {
        cancelled(cancel);
        const auto count = file.read(buffer.data(), buffer.size());
        if (count < 0 || file.pos() > initial_size)
            fail("Cannot hash update file: " + path);
        if (!count)
            break;
        hash.addData(QByteArrayView(buffer.constData(), count));
        report(progress, UpdatePhase::verifying, path, base + quint64(file.pos()), total);
    }
    return hash.result();
}
bool matches(const QString &root, const UpdateFile &expected, const std::atomic_bool &cancel,
             const UpdateProgressCallback &progress = {}, quint64 base = 0, quint64 total = 0) {
    cancelled(cancel);
    const auto path = QDir(root).filePath(expected.path);
    plain_path(path);
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly) ||
        quint64(file.size()) != expected.size)
        return false;
    return hash_file(file, expected.path, cancel, progress, base, total) == expected.sha256;
}
void verify_managed(const UpdateManifest &manifest, const QString &root) {
    const auto bytes = read_small(QDir(root).filePath(managed_list));
    const auto marker = read_small(QDir(root).filePath(managed_marker), 128);
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    if (marker != QByteArray("1\n") + digest + '\n' || !bytes.endsWith('\n'))
        fail("Invalid update ownership marker");
    QMap<QString, QByteArray> expected;
    for (const auto &file : manifest.files)
        if (file.path != managed_list && file.path != managed_marker)
            expected.insert(file.path, file.sha256);
    auto lines = bytes.split('\n');
    lines.removeLast();
    for (const auto &line : lines) {
        if (line.size() < 67 || line.mid(64, 2) != "  ")
            fail("Invalid update ownership list");
        const auto path = QString::fromUtf8(line.mid(66));
        check_relative_path(path);
        if (path.toUtf8() != line.mid(66) || !expected.contains(path) ||
            digest_text(QString::fromLatin1(line.first(64))) != expected.take(path))
            fail("Update ownership list does not match manifest");
    }
    if (!expected.isEmpty())
        fail("Incomplete update ownership list");
}
void verify_tree(const UpdateManifest &manifest, const QString &root,
                 const std::atomic_bool &cancel, const UpdateProgressCallback &progress) {
    directory(root);
    QSet<QString> expected;
    for (const auto &file : manifest.files)
        expected.insert(file.path);
    QDirIterator entries(
        root, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        cancelled(cancel);
        const auto path = entries.next();
        plain_path(path);
        if (!entries.fileInfo().isDir() && !expected.remove(QDir(root).relativeFilePath(path)))
            fail("Unexpected file in prepared update: " + path);
    }
    if (!expected.isEmpty())
        fail("Incomplete prepared update");
    quint64 done = 0;
    const auto total = total_size(manifest);
    for (const auto &file : manifest.files) {
        if (!matches(root, file, cancel, progress, done, total))
            fail("Prepared update hash mismatch: " + file.path);
        done += file.size;
    }
    verify_managed(manifest, root);
}
void prepare_parent(const QString &path) {
    plain_path(path);
    const auto parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent))
        fail("Cannot create update directory: " + parent);
    plain_path(parent);
}
void copy_verified(const QString &source, const QString &destination, const UpdateFile &expected,
                   const std::atomic_bool &cancel, const UpdateProgressCallback &progress,
                   quint64 base, quint64 total) {
    plain_path(source);
    prepare_parent(destination);
    QFile input(source), output(destination);
    if (!QFileInfo(input).isFile() || !input.open(QIODevice::ReadOnly) ||
        quint64(input.size()) != expected.size ||
        !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        fail("Cannot reuse update file: " + expected.path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(buffer_bytes, Qt::Uninitialized);
    quint64 copied = 0;
    while (!input.atEnd()) {
        cancelled(cancel);
        const auto count = input.read(buffer.data(), buffer.size());
        if (count <= 0 || quint64(count) > expected.size - copied ||
            output.write(buffer.constData(), count) != count)
            fail("Cannot reuse update file: " + expected.path);
        copied += quint64(count);
        hash.addData(QByteArrayView(buffer.constData(), count));
        report(progress, UpdatePhase::reusing, expected.path, base + copied, total);
    }
    if (copied != expected.size || hash.result() != expected.sha256 || !output.flush())
        fail("Reused update file hash mismatch: " + expected.path);
}

voidpf ZCALLBACK zip_open(voidpf opaque, const void *, int mode) {
    auto *file = static_cast<QFile *>(opaque);
    return (mode & ZLIB_FILEFUNC_MODE_READ) && file->seek(0) ? file : nullptr;
}
uLong ZCALLBACK zip_read(voidpf, voidpf stream, void *buffer, uLong length) {
    const auto count =
        static_cast<QFile *>(stream)->read(static_cast<char *>(buffer), qint64(length));
    return count < 0 ? 0 : uLong(count);
}
ZPOS64_T ZCALLBACK zip_tell(voidpf, voidpf stream) {
    return ZPOS64_T(static_cast<QFile *>(stream)->pos());
}
long ZCALLBACK zip_seek(voidpf, voidpf stream, ZPOS64_T offset, int origin) {
    auto *file = static_cast<QFile *>(stream);
    const qint64 base = origin == ZLIB_FILEFUNC_SEEK_SET   ? 0
                        : origin == ZLIB_FILEFUNC_SEEK_CUR ? file->pos()
                        : origin == ZLIB_FILEFUNC_SEEK_END ? file->size()
                                                           : -1;
    if (base < 0 || offset > ZPOS64_T(std::numeric_limits<qint64>::max() - base))
        return -1;
    return file->seek(base + qint64(offset)) ? 0 : -1;
}
int ZCALLBACK zip_close(voidpf, voidpf) { return 0; }
int ZCALLBACK zip_error(voidpf, voidpf stream) {
    return static_cast<QFile *>(stream)->error() == QFileDevice::NoError ? 0 : 1;
}
struct Archive {
    unzFile handle = nullptr;
    bool entry_open = false;
    ~Archive() {
        if (entry_open)
            unzCloseCurrentFile(handle);
        if (handle)
            unzClose(handle);
    }
};
void extract_bundle(const UpdateManifest &manifest, const UpdateBundle &bundle, const QString &path,
                    const QString &staging, const std::atomic_bool &cancel,
                    const UpdateProgressCallback &progress, quint64 &done) {
    plain_path(path);
    QFile bytes(path);
    if (!QFileInfo(bytes).isFile() || !bytes.open(QIODevice::ReadOnly) ||
        quint64(bytes.size()) != bundle.size ||
        hash_file(bytes, bundle.name, cancel) != bundle.sha256)
        fail("Downloaded update bundle hash mismatch: " + bundle.id);
    zlib_filefunc64_def io{};
    io.zopen64_file = zip_open;
    io.zread_file = zip_read;
    io.ztell64_file = zip_tell;
    io.zseek64_file = zip_seek;
    io.zclose_file = zip_close;
    io.zerror_file = zip_error;
    io.opaque = &bytes;
    Archive archive{unzOpen2_64(path.utf16(), &io)};
    unz_global_info64 global{};
    if (!archive.handle || unzGetGlobalInfo64(archive.handle, &global) != UNZ_OK ||
        global.number_entry == 0 || global.number_entry > ZPOS64_T(max_files))
        fail("Invalid update archive: " + bundle.id);
    QMap<QString, const UpdateFile *> expected;
    for (const auto &file : manifest.files)
        if (file.bundle == bundle.id)
            expected.insert(file.path, &file);
    if (global.number_entry != ZPOS64_T(expected.size()))
        fail("Unexpected update archive entry count");
    QSet<QString> seen;
    const auto total = total_size(manifest);
    auto status = unzGoToFirstFile(archive.handle);
    for (ZPOS64_T index = 0; index < global.number_entry; ++index) {
        cancelled(cancel);
        unz_file_info64 info{};
        if (status != UNZ_OK ||
            unzGetCurrentFileInfo64(archive.handle, &info, nullptr, 0, nullptr, 0, nullptr, 0) !=
                UNZ_OK ||
            info.size_filename == 0 || info.size_filename > 4096)
            fail("Invalid update archive entry");
        QByteArray name(qsizetype(info.size_filename), Qt::Uninitialized);
        if (unzGetCurrentFileInfo64(archive.handle, &info, name.data(), uLong(name.size()), nullptr,
                                    0, nullptr, 0) != UNZ_OK)
            fail("Cannot read update archive entry");
        const auto relative = QString::fromUtf8(name);
        check_relative_path(relative);
        const auto kind = (info.external_fa >> 16) & 0170000;
        if (relative.toUtf8() != name || (info.flag & (1 | 64)) != 0 ||
            (kind != 0 && kind != 0100000) || (info.external_fa & 0x410) != 0 ||
            (info.compression_method != 0 && info.compression_method != Z_DEFLATED) ||
            seen.contains(relative.toCaseFolded()) || !expected.contains(relative))
            fail("Unsafe or unexpected update archive entry: " + relative);
        const auto &file = *expected.value(relative);
        if (info.uncompressed_size != file.size ||
            (file.size > 1024 * 1024 &&
             (info.compressed_size == 0 || file.size / info.compressed_size > 1000)))
            fail("Oversized update archive entry: " + relative);
        seen.insert(relative.toCaseFolded());
        const auto destination = QDir(staging).filePath(relative);
        prepare_parent(destination);
        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            unzOpenCurrentFile(archive.handle) != UNZ_OK)
            fail("Cannot extract update file: " + relative);
        archive.entry_open = true;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray buffer(buffer_bytes, Qt::Uninitialized);
        quint64 count = 0;
        for (;;) {
            cancelled(cancel);
            const auto read =
                unzReadCurrentFile(archive.handle, buffer.data(), unsigned(buffer.size()));
            if (read < 0 || (read > 0 && quint64(read) > file.size - count))
                fail("Corrupt update archive entry: " + relative);
            if (read == 0)
                break;
            if (output.write(buffer.constData(), read) != read)
                fail("Cannot write update file: " + relative);
            hash.addData(QByteArrayView(buffer.constData(), read));
            count += quint64(read);
            report(progress, UpdatePhase::extracting, relative, done + count, total);
        }
        const auto crc = unzCloseCurrentFile(archive.handle);
        archive.entry_open = false;
        if (crc != UNZ_OK || count != file.size || hash.result() != file.sha256 || !output.flush())
            fail("Extracted update file verification failed: " + relative);
        done += count;
        status = unzGoToNextFile(archive.handle);
    }
    if (status != UNZ_END_OF_LIST_OF_FILE || seen.size() != expected.size())
        fail("Invalid update archive directory");
}
struct StagingDirectory {
    QString path;
    bool published = false;
    ~StagingDirectory() {
        if (published)
            return;
        try {
            plain_path(path);
            QDirIterator entries(
                path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                QDirIterator::Subdirectories);
            while (entries.hasNext())
                plain_path(entries.next());
            QDir(path).removeRecursively();
        } catch (...) {
            // Never follow a replaced directory or reparse point during failure cleanup.
        }
    }
};
bool publish_without_replacement(const QString &source, const QString &target) {
#ifdef Q_OS_WIN
    return MoveFileW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(target.utf16())) != 0;
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    const auto from = QFile::encodeName(source), to = QFile::encodeName(target);
    return syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD, to.constData(),
                   RENAME_NOREPLACE) == 0;
#else
    // Do not silently weaken the no-replacement guarantee on unsupported platforms.
    Q_UNUSED(source);
    Q_UNUSED(target);
    return false;
#endif
}
} // namespace

UpdateManifest parse_update_manifest(const QByteArray &json, const QString &expected_version) {
    if (json.size() > max_metadata_bytes || !valid_version(expected_version))
        fail("Invalid update manifest input");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    const auto object = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        object.value("schema").toDouble(-1) != 1 || object.value("layout").toDouble(-1) != 1 ||
        object.value("platform").toString() != "win-x64" ||
        object.value("version").toString() != expected_version ||
        !object.value("files").isArray() || !object.value("bundles").isArray())
        fail("Unsupported update manifest");
    UpdateManifest manifest;
    manifest.version = expected_version;
    for (const auto &value : object.value("bundles").toArray()) {
        const auto entry = value.toObject();
        manifest.bundles.push_back({entry.value("id").toString(), entry.value("name").toString(),
                                    json_size(entry.value("size")),
                                    digest_text(entry.value("sha256").toString())});
    }
    for (const auto &value : object.value("files").toArray()) {
        const auto entry = value.toObject();
        manifest.files.push_back({entry.value("path").toString(), entry.value("bundle").toString(),
                                  json_size(entry.value("size")),
                                  digest_text(entry.value("sha256").toString())});
    }
    validate(manifest);
    return manifest;
}

QStringList plan_update(const UpdateManifest &manifest, const QString &current_dir,
                        const std::atomic_bool &cancel, const UpdateProgressCallback &progress) {
    validate(manifest);
    directory(current_dir);
    QStringList needed;
    quint64 done = 0;
    const auto total = total_size(manifest);
    for (const auto &bundle : manifest.bundles) {
        bool reusable = true;
        for (const auto &file : manifest.files) {
            if (file.bundle != bundle.id)
                continue;
            if (!matches(current_dir, file, cancel, progress, done, total))
                reusable = false;
            done += file.size;
        }
        if (!reusable)
            needed.push_back(bundle.id);
    }
    cancelled(cancel);
    return needed;
}

QString stage_update(const UpdateManifest &manifest, const QString &current_dir,
                     const QString &install_root, const QMap<QString, QString> &downloads,
                     const std::atomic_bool &cancel, const UpdateProgressCallback &progress) {
    validate(manifest);
    cancelled(cancel);
    const auto root = absolute(install_root);
    directory(root);
    directory(current_dir);
    const auto versions = QDir(root).filePath("versions");
    directory(versions);
    const auto target = QDir(versions).filePath(manifest.version);
    plain_path(target);
    if (QFileInfo::exists(target)) {
        verify_tree(manifest, target, cancel, progress);
        cancelled(cancel);
        return target;
    }
    const auto needed = plan_update(manifest, current_dir, cancel, progress);
    for (const auto &id : needed)
        if (!downloads.contains(id))
            fail("Missing downloaded update bundle: " + id);
    const auto stage =
        QDir(versions).filePath(".staging-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    plain_path(stage);
    if (!QDir().mkdir(stage))
        fail("Cannot create update staging directory");
    StagingDirectory staging{stage};
    quint64 done = 0;
    const auto total = total_size(manifest);
    for (const auto &bundle : manifest.bundles) {
        cancelled(cancel);
        if (needed.contains(bundle.id)) {
            extract_bundle(manifest, bundle, downloads.value(bundle.id), stage, cancel, progress,
                           done);
        } else {
            for (const auto &file : manifest.files) {
                if (file.bundle != bundle.id)
                    continue;
                copy_verified(QDir(current_dir).filePath(file.path),
                              QDir(stage).filePath(file.path), file, cancel, progress, done, total);
                done += file.size;
            }
        }
    }
    verify_tree(manifest, stage, cancel, progress);
    report(progress, UpdatePhase::finalizing, manifest.version, done, total);
    cancelled(cancel);
    plain_path(versions);
    plain_path(stage);
    plain_path(target);
    if (QFileInfo::exists(target) || !publish_without_replacement(stage, target))
        fail("Cannot publish prepared update version");
    staging.published = true;
    return target;
}

QString determine_install_root(const QString &app_dir) {
    try {
        const auto app = absolute(app_dir);
        directory(app);
        const QFileInfo current(app);
        const QDir versions(current.absolutePath());
        if (!valid_version(current.fileName()) || versions.dirName() != "versions")
            return {};
        const auto root = QFileInfo(versions.absolutePath()).absolutePath();
        if (read_small(QDir(root).filePath("orbelle-layout.txt"), 16) != "1\n")
            return {};
        for (const auto &path :
             {QDir(root).filePath("Orbelle.exe"), QDir(app).filePath("OrbelleApp.exe"),
              QDir(app).filePath("OrbelleUpdater.exe"), QDir(app).filePath("OrbelleLauncher.exe"),
              QDir(app).filePath("manifest.json"), QDir(app).filePath(managed_list),
              QDir(app).filePath(managed_marker)}) {
            plain_path(path);
            if (!QFileInfo(path).isFile())
                return {};
        }
        return root;
    } catch (const std::runtime_error &) {
        return {};
    }
}
} // namespace orbelle::desktop
