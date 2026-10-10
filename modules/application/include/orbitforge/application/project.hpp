#pragma once
#include <filesystem>
#include <optional>
#include <orbitforge/core/frames/eop_provider.hpp>
#include <orbitforge/core/frames/state.hpp>
#include <orbitforge/core/propagation/central_body.hpp>
#include <span>
#include <vector>

namespace orbitforge::application {
enum class OrbitPropagator { two_body, j2_secular };
// Canonical GCRF input. J2 stores the auxiliary mean ellipse, never an
// osculating state. The standard model always includes all three secular terms.
struct OrbitInput {
    core::CartesianState initial{
        core::Epoch{0}, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    OrbitPropagator propagator = OrbitPropagator::two_body;
    core::FrameId input_frame = core::FrameId::gcrf;
    [[nodiscard]] double mu() const {
        return core::central_body(initial.origin).mu.metres_cubed_per_second_squared;
    }
    std::string object_id = "satellite-1";
    core::Epoch first{0}, last{7'200'000'000'000};
    int step_seconds = 60;
};
struct OrbitDisplay {
    // 0 = one period, 1 = scenario, 2 = custom trail/lead.
    int extent = 0;
    double past_minutes = 45, future_minutes = 45;
    bool track_2d = true, labels_2d = true, track_3d = true;
    bool operator==(const OrbitDisplay &) const = default;
};
struct ProjectSatellite {
    OrbitInput orbit;
    OrbitDisplay display;
    // 0 = point, 1 = built-in schematic, 2 = external model.
    int model_kind = 0;
    double model_diameter_m = 1'000'000;
    // UTF-8 path, relative to the project file (or absolute). External assets
    // remain external; a missing asset must not silently change the project.
    std::string model_path;
};
struct Project {
    Project(std::string name, core::Epoch first, core::Epoch last);
    std::string name;
    core::Epoch first, last, current;
    std::vector<ProjectSatellite> satellites;
    // Unbound Python-created projects may select installed data on first open.
    // Once bound, the exact data and interpolation versions must match.
    std::optional<core::EopDataInfo> earth_orientation;
};
class ProjectError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
void validate_project(const Project &project);
[[nodiscard]] bool matching_project_data(const core::EopDataInfo &a, const core::EopDataInfo &b);
// Versioned, little-endian .orbitforge container. No Qt or provider dependency.
[[nodiscard]] std::vector<std::uint8_t> encode_project(const Project &project);
[[nodiscard]] Project decode_project(std::span<const std::uint8_t> bytes);
[[nodiscard]] Project load_project(const std::filesystem::path &path);
// Writes a sibling temporary file, then atomically publishes it. Failure before
// publication leaves an existing destination intact. Does not create parents.
void save_project(const std::filesystem::path &path, const Project &project,
                  bool overwrite = false);
} // namespace orbitforge::application
