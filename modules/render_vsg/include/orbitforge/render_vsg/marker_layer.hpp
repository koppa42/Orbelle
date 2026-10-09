#pragma once
#include <memory>
#include <orbitforge/visualization/snapshot.hpp>
#include <span>
#include <vsg/nodes/Node.h>

namespace orbitforge::render_vsg {
enum class MarkerStyle { satellite, selection };
// Shared geometry for all views. update() is called on the GUI/update thread,
// before record/submit; it copies display values and never writes into results.
class MarkerLayer {
  public:
    explicit MarkerLayer(std::size_t capacity, MarkerStyle style = MarkerStyle::satellite);
    ~MarkerLayer();
    MarkerLayer(const MarkerLayer &) = delete;
    MarkerLayer &operator=(const MarkerLayer &) = delete;
    [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const;
    void update(const visualization::RenderSnapshot &snapshot);
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] vsg::ref_ptr<vsg::Node>
make_track(std::span<const visualization::RenderSnapshot> samples);
} // namespace orbitforge::render_vsg
