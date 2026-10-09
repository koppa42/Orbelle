#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/render_vsg/earth_display.hpp>

namespace orbitforge::geo_rocky {
// The caller sets Rocky's resource path before creation. An empty imagery path
// explicitly selects an untextured ellipsoid, never an online default layer.
[[nodiscard]] std::unique_ptr<render_vsg::IEarthDisplay>
make_globe(vsg::ref_ptr<vsg::Viewer> viewer, const std::filesystem::path &imagery = {});
} // namespace orbitforge::geo_rocky
