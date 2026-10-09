#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <map>
#include <vector>

namespace orbitforge::desktop {
// Opt-in GUI-thread measurements; normal application runs collect no samples.
class WorkbenchProfile {
  public:
    explicit WorkbenchProfile(QString output);
    ~WorkbenchProfile();
    void phase(const QString &name, bool recording);
    void tick();
    void finish(const QJsonObject &metadata);
    static WorkbenchProfile *active;
    void record(const char *name, double milliseconds);

  private:
    QString output_, phase_;
    bool recording_ = false;
    QElapsedTimer cadence_;
    std::map<QString, std::map<QString, std::vector<double>>> samples_;
};
class ProfileScope {
  public:
    explicit ProfileScope(const char *name) : profile_(WorkbenchProfile::active), name_(name) {
        if (profile_)
            timer_.start();
    }
    ~ProfileScope() {
        if (profile_)
            profile_->record(name_, timer_.nsecsElapsed() * 1e-6);
    }

  private:
    WorkbenchProfile *profile_;
    const char *name_;
    QElapsedTimer timer_;
};
} // namespace orbitforge::desktop
