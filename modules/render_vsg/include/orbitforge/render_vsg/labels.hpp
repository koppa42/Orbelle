#pragma once
#include <memory>
#include <optional>
#include <orbitforge/visualization/snapshot.hpp>
#include <span>
#include <vsg/app/Camera.h>
#include <vsg/text/Font.h>

namespace orbitforge::render_vsg {
struct ProjectedMarker {
    std::size_t index;
    double x_pixels, y_pixels;
};
// Display-only visibility against the WGS84 ellipsoid, not an Access calculation.
[[nodiscard]] std::vector<ProjectedMarker>
visible_markers(const visualization::RenderSnapshot &snapshot, const vsg::Camera &camera,
                std::uint32_t width_pixels, std::uint32_t height_pixels, double margin_pixels = 0);
[[nodiscard]] std::optional<std::size_t> pick_marker(std::span<const ProjectedMarker> markers,
                                                     double x_pixels, double y_pixels,
                                                     double radius_pixels);

// Each view owns its label layouts; glyph atlas and immutable font/pipeline data
// are shared. Fixed capacities avoid reallocation/recompilation during playback.
class LabelLayer {
  public:
    LabelLayer(std::size_t capacity, vsg::ref_ptr<vsg::Font> font);
    ~LabelLayer();
    LabelLayer(const LabelLayer &) = delete;
    LabelLayer &operator=(const LabelLayer &) = delete;
    [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const;
    void update(const visualization::RenderSnapshot &snapshot,
                std::span<const ProjectedMarker> visible, const vsg::Camera &camera,
                std::uint32_t height_pixels);
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbitforge::render_vsg
