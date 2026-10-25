#pragma once
#include <cstdint>
#include <memory>
#include <orbelle/visualization/region_view.hpp>
#include <vsg/nodes/Node.h>

namespace orbelle::render_vsg {
// Static surface fill and screen-space outline; selection updates only a small
// appearance buffer. Geometry is rebuilt only when the region is edited.
class RegionLayer {
  public:
    RegionLayer(const visualization::RegionMesh &, std::uint32_t color, double opacity,
                double device_pixel_ratio, bool srgb_framebuffer);
    ~RegionLayer();
    [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const;
    void set_selected(bool);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbelle::render_vsg
