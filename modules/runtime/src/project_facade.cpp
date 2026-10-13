#include <orbitforge/project.hpp>

namespace orbitforge {
Project::Project(std::string name, Epoch first, Epoch last)
    : document_(application::Project{std::move(name), first, last}) {}
Project::Project(application::ProjectDocument document) : document_(std::move(document)) {}
Project Project::from_data(application::Project data) {
    return Project{application::ProjectDocument{std::move(data)}};
}
Project Project::from_document(application::ProjectDocument document) {
    return Project{std::move(document)};
}
application::Project Project::to_data() const { return *snapshot().project; }
ProjectOpenPlan Project::plan_open(const std::filesystem::path &path, DataSnapshot data) {
    return ProjectOpenPlan{runtime::ProjectOpenPlan{path, std::move(data)}};
}
ProjectOpenPlan Project::plan_open(const std::filesystem::path &path, const DataCatalog &catalog) {
    return ProjectOpenPlan{runtime::ProjectOpenPlan{path, catalog}};
}
Project Project::open(const std::filesystem::path &path, DataSnapshot data) {
    return plan_open(path, std::move(data)).open();
}
Project Project::open(const std::filesystem::path &path, const DataCatalog &catalog) {
    return plan_open(path, catalog).open();
}
void Project::update(const ProjectUpdate &patch) {
    auto transaction = edit();
    transaction.settings(patch.name, patch.first, patch.last, patch.current);
    transaction.commit();
}
void Project::set_reports(std::vector<application::ReportDefinition> definitions) {
    auto transaction = edit();
    transaction.reports(std::move(definitions));
    transaction.commit();
}
AnalysisPlan Project::plan_prepare(DataSnapshot data) const {
    return AnalysisPlan{document_, std::move(data)};
}
AnalysisPlan Project::plan_prepare(const DataCatalog &catalog) const {
    return AnalysisPlan{document_, catalog};
}
Analysis Project::prepare(DataSnapshot data) const {
    return plan_prepare(std::move(data)).prepare();
}
Analysis Project::prepare(const DataCatalog &catalog) const {
    return plan_prepare(catalog).prepare();
}
void Project::save(const std::filesystem::path &path, bool overwrite) const {
    document_.save(path, overwrite);
}
void Project::save(const std::filesystem::path &path, const Analysis &analysis,
                   bool overwrite) const {
    runtime::save_project(document_, path, analysis, overwrite);
}
std::uint64_t Project::subscribe(std::function<void(const ChangeSet &)> observer) {
    return document_.subscribe(std::move(observer));
}
void Project::unsubscribe(std::uint64_t token) { document_.unsubscribe(token); }
std::vector<std::string> Project::observer_errors() const { return document_.observer_errors(); }
Project ProjectOpenPlan::open(bool accept_data_changes) const {
    try {
        return Project::from_document(plan_.open(accept_data_changes));
    } catch (const runtime::OpenDataChangedError &) {
        throw OpenDataChangedError{*this};
    }
}
OpenDataChangedError::OpenDataChangedError(ProjectOpenPlan plan)
    : std::runtime_error("Selected calculation data differ from the project's recorded data"),
      plan_(std::make_shared<const ProjectOpenPlan>(std::move(plan))) {}
} // namespace orbitforge
