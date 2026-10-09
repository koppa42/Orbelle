#include <algorithm>
#include <cctype>
#include <limits>
#include <orbitforge/application/scenario.hpp>
#include <stdexcept>

namespace orbitforge::application {
Scenario::Scenario(std::string name, core::Epoch first, core::Epoch last)
    : name_(std::move(name)), first_(first), last_(last) {
    if (name_.empty() ||
        std::all_of(name_.begin(), name_.end(), [](unsigned char c) { return std::isspace(c); }))
        throw std::invalid_argument("Scenario name must not be empty");
    if (first_.tai_nanoseconds() >= last_.tai_nanoseconds())
        throw std::invalid_argument("Scenario analysis interval must have positive duration");
}
Scenario Scenario::with_object(Session session) const {
    const auto &provenance = session.provenance();
    const auto existing = std::find_if(objects_.begin(), objects_.end(), [&](const auto &object) {
        return object.provenance().object_id == provenance.object_id;
    });
    if (existing == objects_.end()) {
        if (provenance.input_revision != 1)
            throw std::invalid_argument("New scenario objects must start at revision 1");
    } else {
        const auto previous = existing->provenance().input_revision;
        if (previous == std::numeric_limits<std::uint64_t>::max() ||
            provenance.input_revision != previous + 1)
            throw std::invalid_argument("Duplicate object ID or stale object revision");
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Scenario revision exhausted");
    auto result = *this;
    if (existing == objects_.end())
        result.objects_.push_back(std::move(session));
    else
        result.objects_[static_cast<std::size_t>(existing - objects_.begin())] = std::move(session);
    ++result.revision_;
    return result;
}
std::vector<results::StateSeries> Scenario::sample(std::span<const core::Epoch> epochs) const {
    for (const auto epoch : epochs)
        if (epoch.tai_nanoseconds() < first_.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > last_.tai_nanoseconds())
            throw std::out_of_range("Epoch lies outside the scenario analysis interval");
    std::vector<results::StateSeries> result;
    result.reserve(objects_.size());
    for (const auto &object : objects_)
        result.push_back(object.sample(epochs));
    return result;
}
} // namespace orbitforge::application
