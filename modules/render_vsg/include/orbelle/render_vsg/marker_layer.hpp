#pragma once
#include <memory>
#include <orbelle/visualization/snapshot.hpp>
#include <span>
#include <vsg/nodes/Node.h>

namespace orbelle::render_vsg {
// Ground annotations require per-view CPU horizon culling before update(). They
// overlay the visible surface so screen-space glyph edges are not depth-clipped.
enum class MarkerStyle { satellite, selection, ground_station, ground_selection };
// Shared geometry for all views. update() is called on the GUI/update thread,
// before record/submit; it copies display values and never writes into results.
class MarkerLayer {
  public:
    explicit MarkerLayer(std::size_t capacity, MarkerStyle style = MarkerStyle::satellite);
    ~MarkerLayer();
    MarkerLayer(const MarkerLayer &) = delete;
    MarkerLayer &operator=(const MarkerLayer &) = delete;
    [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const;
    void update(const visualization::RenderSnapshot &snapshot,
                std::span<const std::uint32_t> colors = {}, bool srgb_framebuffer = true);
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Immutable segment buffers; interval changes only adjust the instance range.
// Screen-space triangles support width and stipple without optional Vulkan line features.
class TrackLayer {
  public:
    explicit TrackLayer(std::span<const visualization::RenderSnapshot> samples);
    ~TrackLayer();
    [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const;
    void set_interval(core::Epoch first, core::Epoch last);
    // 0 solid, 1 dashed, 2 dotted, 3 dash-dot; logical screen pixels, opaque sRGB.
    // Updates a small uniform buffer, leaving sampled geometry and pipelines intact.
    void set_appearance(std::uint32_t rgb, double width, unsigned pattern,
                        double device_pixel_ratio = 1, bool srgb_framebuffer = true);
    [[nodiscard]] std::size_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] vsg::ref_ptr<vsg::Node>
make_track(std::span<const visualization::RenderSnapshot> samples);
} // namespace orbelle::render_vsg
