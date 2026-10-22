#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <orbelle/application/scenario.hpp>
#include <stdexcept>

namespace orbelle::application {
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
Scenario Scenario::with_objects(std::vector<Session> sessions) const {
    auto result = *this;
    if (sessions.empty())
        return result;
    if (sessions.size() > result.objects_.max_size() - result.objects_.size())
        throw std::length_error("Scenario object capacity exceeded");
    result.objects_.reserve(result.objects_.size() + sessions.size());
    // Own the keys: replacing a Session may destroy its previous provenance strings.
    std::map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < result.objects_.size(); ++i)
        indices.emplace(result.objects_[i].provenance().object_id, i);
    for (auto &session : sessions) {
        const auto &p = session.provenance();
        const auto found = indices.find(p.object_id);
        if (found == indices.end()) {
            if (p.input_revision != 1)
                throw std::invalid_argument("New scenario objects must start at revision 1");
        } else {
            const auto previous = result.objects_[found->second].provenance().input_revision;
            if (previous == std::numeric_limits<std::uint64_t>::max() ||
                p.input_revision != previous + 1)
                throw std::invalid_argument("Duplicate object ID or stale object revision");
        }
        if (result.revision_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Scenario revision exhausted");
        if (found == indices.end()) {
            indices.emplace(p.object_id, result.objects_.size());
            result.objects_.push_back(std::move(session));
        } else
            result.objects_[found->second] = std::move(session);
        ++result.revision_;
    }
    return result;
}
Scenario Scenario::without_object(const std::string &object_id) const {
    const auto existing = std::find_if(objects_.begin(), objects_.end(), [&](const auto &object) {
        return object.provenance().object_id == object_id;
    });
    if (existing == objects_.end())
        throw std::invalid_argument("Scenario object ID does not exist");
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Scenario revision exhausted");
    auto result = *this;
    result.objects_.erase(result.objects_.begin() + (existing - objects_.begin()));
    ++result.revision_;
    return result;
}
Scenario Scenario::with_settings(std::string name, core::Epoch first, core::Epoch last) const {
    const Scenario validated(std::move(name), first, last);
    if (validated.name_ == name_ && first == first_ && last == last_)
        return *this;
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Scenario revision exhausted");
    auto result = *this;
    result.name_ = validated.name_;
    result.first_ = first;
    result.last_ = last;
    ++result.revision_;
    return result;
}
Scenario Scenario::with_data_lock(DataLock lock) const {
    if (lock == data_lock_)
        return *this;
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Scenario revision exhausted");
    auto result = *this;
    result.data_lock_ = std::move(lock);
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
} // namespace orbelle::application
