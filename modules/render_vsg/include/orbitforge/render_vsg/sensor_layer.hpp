#pragma once
#include <memory>
#include <orbitforge/visualization/sensor_view.hpp>
#include <span>
#include <vsg/nodes/Node.h>
namespace orbitforge::render_vsg {
class SensorLayer {
  public:
    explicit SensorLayer(std::size_t capacity);
    ~SensorLayer();
    [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const;
    [[nodiscard]] std::size_t capacity() const;
    static std::size_t required(std::span<const visualization::SensorMesh> meshes);
    void update(std::span<const visualization::SensorMesh> meshes, bool srgb_framebuffer);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbitforge::render_vsg
