#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <vector>

namespace orbelle::desktop {
struct UpdateFile {
    QString path;
    QString bundle;
    quint64 size = 0;
    QByteArray sha256;
};

struct UpdateBundle {
    QString id;
    QString name;
    quint64 size = 0;
    QByteArray sha256;
};

struct UpdateManifest {
    QString version;
    std::vector<UpdateBundle> bundles;
    std::vector<UpdateFile> files;
};

enum class UpdatePhase { verifying, reusing, extracting, finalizing };
struct UpdateProgress {
    UpdatePhase phase;
    QString path;
    quint64 completed_bytes = 0;
    quint64 total_bytes = 0;
};
using UpdateProgressCallback = std::function<void(const UpdateProgress &)>;

class UpdateCancelled final : public std::runtime_error {
  public:
    UpdateCancelled() : std::runtime_error("Update cancelled") {}
};

// Digests in the parsed manifest are raw 32-byte SHA-256 values, not hexadecimal strings.
UpdateManifest parse_update_manifest(const QByteArray &json, const QString &expected_version);

// Returns bundle IDs requiring a download. Local files are verified individually, without
// modifying the current installation. Callbacks run synchronously on the caller's thread.
QStringList plan_update(const UpdateManifest &manifest, const QString &current_dir,
                        const std::atomic_bool &cancel,
                        const UpdateProgressCallback &progress = {});

// Downloads are indexed by bundle ID. Builds and verifies a unique staging directory, then
// publishes versions/<version> without replacing any existing version or changing activation.
// Throws UpdateCancelled on cancellation and runtime_error for invalid packages or I/O errors.
QString stage_update(const UpdateManifest &manifest, const QString &current_dir,
                     const QString &install_root, const QMap<QString, QString> &downloads,
                     const std::atomic_bool &cancel, const UpdateProgressCallback &progress = {});

// Returns an empty string unless app_dir is a version directory of a valid portable layout.
// A newly launched, verified version may precede the active-version pointer switch.
QString determine_install_root(const QString &app_dir);
} // namespace orbelle::desktop
