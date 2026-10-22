#pragma once
#include <filesystem>
#include <memory>
#include <orbelle/render_vsg/earth_display.hpp>

namespace orbelle::geo_rocky {
// The caller sets Rocky's resource path before creation. An empty imagery path
// explicitly selects an untextured ellipsoid, never an online default layer.
[[nodiscard]] std::unique_ptr<render_vsg::IEarthDisplay>
make_globe(vsg::ref_ptr<vsg::Viewer> viewer, const std::filesystem::path &imagery = {});
} // namespace orbelle::geo_rocky
