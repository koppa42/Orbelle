#pragma once
#include <filesystem>
#include <vsg/nodes/MatrixTransform.h>

namespace orbitforge::desktop {
// Empty path creates the bundled schematic model. Returns a centered unit-diameter model.
[[nodiscard]] vsg::ref_ptr<vsg::MatrixTransform>
make_satellite_model(const std::filesystem::path &path = {});
} // namespace orbitforge::desktop
