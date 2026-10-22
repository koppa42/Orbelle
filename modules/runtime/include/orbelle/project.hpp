#pragma once
#include <orbelle/runtime/data_catalog.hpp>

namespace orbelle {
using application::AnalysisRecord;
using application::AnalysisStatus;
using application::ChangeSet;
using application::EditClosedError;
using application::ObjectKind;
using application::ObjectManager;
using application::ObjectNotFoundError;
using application::ObjectRef;
using application::ObjectState;
using application::ProjectEdit;
using application::ProjectError;
using application::ProjectSnapshot;
using application::RemovalPlan;
using application::RevisionConflictError;
using core::Epoch;
using runtime::Analysis;
using runtime::AnalysisCancelled;
using runtime::AnalysisCapabilityError;
using runtime::AnalysisPlan;
using runtime::DataCatalog;
using runtime::DataChange;
using runtime::DataChangedError;
using runtime::DataSnapshot;
using runtime::EopProduct;
namespace objects = application::objects;

struct ProjectUpdate {
    std::optional<std::string> name = {};
    std::optional<Epoch> first = {}, last = {}, current = {};
};
class ProjectOpenPlan;

// Public C++ project handle. Copies share one editable document and its identity;
// from_data() makes an independent document. Snapshots and analyses are immutable.
// Methods use the same application commands and runtime plans as Python.
class Project {
  public:
    Project(std::string name, Epoch first, Epoch last);
    [[nodiscard]] static Project from_data(application::Project data);
    // Explicit bridge for adapters: shares the existing document, no copy of inputs.
    [[nodiscard]] static Project from_document(application::ProjectDocument document);
    [[nodiscard]] const application::ProjectDocument &document() const { return document_; }
    [[nodiscard]] application::Project to_data() const;

    [[nodiscard]] static ProjectOpenPlan plan_open(const std::filesystem::path &path,
                                                   DataSnapshot data = {});
    [[nodiscard]] static ProjectOpenPlan plan_open(const std::filesystem::path &path,
                                                   const DataCatalog &catalog);
    // Opens only with matching data; a change throws OpenDataChangedError with
    // the captured plan. Missing resources and coverage errors cannot be accepted.
    [[nodiscard]] static Project open(const std::filesystem::path &path, DataSnapshot data = {});
    [[nodiscard]] static Project open(const std::filesystem::path &path,
                                      const DataCatalog &catalog);

    [[nodiscard]] ObjectManager objects() const { return document_.objects(); }
    // Only edit.objects()/edit.settings() affect a draft. Project methods always
    // use the committed document. Explicit commit; destruction discards the draft.
    [[nodiscard]] ProjectEdit edit() const { return document_.edit(); }
    [[nodiscard]] ProjectSnapshot snapshot() const { return document_.snapshot(); }
    [[nodiscard]] std::uint64_t revision() const { return snapshot().revision; }
    [[nodiscard]] bool dirty() const { return document_.dirty(); }
    [[nodiscard]] std::string name() const { return snapshot().project->name; }
    [[nodiscard]] Epoch first() const { return snapshot().project->first; }
    [[nodiscard]] Epoch last() const { return snapshot().project->last; }
    [[nodiscard]] Epoch current() const { return snapshot().project->current; }
    [[nodiscard]] AnalysisStatus analysis_status() const {
        return snapshot().project->analysis_status();
    }
    void update(const ProjectUpdate &patch);
    void set_reports(std::vector<application::ReportDefinition> definitions);

    // Capture committed inputs and fully loaded resources, without network or
    // document changes. A plan detects intervening edits before acceptance.
    [[nodiscard]] AnalysisPlan plan_prepare(DataSnapshot data = {}) const;
    [[nodiscard]] AnalysisPlan plan_prepare(const DataCatalog &catalog) const;
    [[nodiscard]] Analysis prepare(DataSnapshot data = {}) const;
    [[nodiscard]] Analysis prepare(const DataCatalog &catalog) const;
    // Save current inputs; the analysis overload records matching prepared inputs
    // and data. Both are atomic and refuse overwrite unless explicitly requested.
    void save(const std::filesystem::path &path, bool overwrite = false) const;
    void save(const std::filesystem::path &path, const Analysis &analysis,
              bool overwrite = false) const;

    [[nodiscard]] std::uint64_t subscribe(std::function<void(const ChangeSet &)> observer);
    void unsubscribe(std::uint64_t token);
    [[nodiscard]] std::vector<std::string> observer_errors() const;

  private:
    explicit Project(application::ProjectDocument document);
    application::ProjectDocument document_;
};

// Owns the exact parsed project and data used during confirmation. Each open()
// returns an independent Project. Acceptance never becomes a persistent default.
class ProjectOpenPlan {
  public:
    [[nodiscard]] const std::vector<DataChange> &data_changes() const {
        return plan_.data_changes();
    }
    [[nodiscard]] const DataSnapshot &data() const { return plan_.data(); }
    [[nodiscard]] const ProjectSnapshot &snapshot() const { return plan_.snapshot(); }
    [[nodiscard]] Project open(bool accept_data_changes = false) const;

  private:
    explicit ProjectOpenPlan(runtime::ProjectOpenPlan plan) : plan_(std::move(plan)) {}
    runtime::ProjectOpenPlan plan_;
    friend class Project;
};

class OpenDataChangedError : public std::runtime_error {
  public:
    explicit OpenDataChangedError(ProjectOpenPlan plan);
    [[nodiscard]] const ProjectOpenPlan &plan() const { return *plan_; }

  private:
    std::shared_ptr<const ProjectOpenPlan> plan_;
};
} // namespace orbelle
