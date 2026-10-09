#include <cmath>
#include <limits>
#include <orbitforge/render_vsg/marker_layer.hpp>
#include <stdexcept>
#include <vsg/all.h>

namespace orbitforge::render_vsg {
namespace {
constexpr const char *vertex_source = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(location=0) in vec3 position;
void main() {
    gl_Position = pc.projection * pc.modelview * vec4(position, 1.0);
    gl_PointSize = 8.0;
}
)glsl";
constexpr const char *point_source = R"glsl(#version 450
layout(location=0) out vec4 color;
void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    if (dot(p,p) > 1.0) discard;
    color = vec4(1.0, 0.72, 0.12, 1.0);
}
)glsl";
constexpr const char *line_source = R"glsl(#version 450
layout(location=0) out vec4 color;
void main() { color = vec4(0.35, 0.82, 1.0, 1.0); }
)glsl";
constexpr const char *selection_vertex_source = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(location=0) in vec3 position;
void main() {
    gl_Position = pc.projection * pc.modelview * vec4(position, 1.0);
    gl_PointSize = 18.0;
}
)glsl";
constexpr const char *selection_source = R"glsl(#version 450
layout(location=0) out vec4 color;
void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    float r = dot(p,p);
    if (r > 1.0 || r < 0.30) discard;
    color = vec4(1.0, 0.10, 0.75, 1.0);
}
)glsl";

vsg::ref_ptr<vsg::StateGroup> geometry(vsg::ref_ptr<vsg::vec3Array> positions,
                                       vsg::ref_ptr<vsg::VertexDraw> draw, bool points,
                                       MarkerStyle style = MarkerStyle::satellite) {
    auto input = vsg::VertexInputState::create(
        vsg::VertexInputState::Bindings{{0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX}},
        vsg::VertexInputState::Attributes{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}});
    auto assembly = vsg::InputAssemblyState::create();
    assembly->topology =
        points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    auto layout = vsg::PipelineLayout::create(
        vsg::DescriptorSetLayouts{}, vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}});
    const bool selected = style == MarkerStyle::selection;
    auto depth = vsg::DepthStencilState::create();
    if (selected)
        depth->depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    auto pipeline = vsg::GraphicsPipeline::create(
        layout,
        vsg::ShaderStages{
            vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main",
                                     selected ? selection_vertex_source : vertex_source),
            vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main",
                                     selected ? selection_source
                                              : (points ? point_source : line_source))},
        vsg::GraphicsPipelineStates{input, assembly, vsg::RasterizationState::create(),
                                    vsg::MultisampleState::create(), vsg::ColorBlendState::create(),
                                    depth});
    auto group = vsg::StateGroup::create();
    group->add(vsg::BindGraphicsPipeline::create(pipeline));
    draw->assignArrays(vsg::DataList{positions});
    draw->instanceCount = 1;
    group->addChild(draw);
    return group;
}
void check_frame(const visualization::RenderSnapshot &snapshot) {
    if ((snapshot.frame != core::FrameId::itrf && snapshot.frame != core::FrameId::gcrf) ||
        snapshot.origin != core::OriginId::earth) {
        throw std::invalid_argument(
            "The globe renderer requires explicit Earth/ITRF or Earth/GCRF positions");
    }
}
vsg::vec3 display_position(const visualization::Marker &marker) {
    for (const auto value : marker.position_m) {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
            throw std::invalid_argument("Position cannot be represented in the display buffer");
        }
    }
    return {static_cast<float>(marker.position_m[0]), static_cast<float>(marker.position_m[1]),
            static_cast<float>(marker.position_m[2])};
}
} // namespace

struct MarkerLayer::Impl {
    vsg::ref_ptr<vsg::vec3Array> positions;
    vsg::ref_ptr<vsg::VertexDraw> draw;
    vsg::ref_ptr<vsg::Node> root;
    std::vector<vsg::vec3> converted;
};
MarkerLayer::MarkerLayer(std::size_t capacity, MarkerStyle style)
    : impl_(std::make_unique<Impl>()) {
    if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Marker capacity must fit a nonzero Vulkan vertex count");
    }
    impl_->positions = vsg::vec3Array::create(static_cast<std::uint32_t>(capacity));
    impl_->positions->properties.dataVariance = vsg::DYNAMIC_DATA;
    impl_->draw = vsg::VertexDraw::create();
    impl_->draw->vertexCount = 0;
    impl_->root = geometry(impl_->positions, impl_->draw, true, style);
    impl_->converted.reserve(capacity);
}
MarkerLayer::~MarkerLayer() = default;
vsg::ref_ptr<vsg::Node> MarkerLayer::node() const { return impl_->root; }
std::size_t MarkerLayer::size() const noexcept { return impl_->draw->vertexCount; }
void MarkerLayer::update(const visualization::RenderSnapshot &snapshot) {
    check_frame(snapshot);
    if (snapshot.markers.size() > impl_->positions->size()) {
        throw std::length_error("Snapshot exceeds the allocated marker capacity");
    }
    auto &converted = impl_->converted;
    converted.clear();
    for (const auto &marker : snapshot.markers)
        converted.push_back(display_position(marker));
    std::copy(converted.begin(), converted.end(), impl_->positions->begin());
    impl_->positions->dirty();
    impl_->draw->vertexCount = static_cast<std::uint32_t>(converted.size());
}
struct TrackLayer::Impl {
    vsg::ref_ptr<vsg::Node> root;
    vsg::ref_ptr<vsg::VertexDraw> draw;
    std::vector<core::Epoch> epochs;
};
TrackLayer::TrackLayer(std::span<const visualization::RenderSnapshot> samples)
    : impl_(std::make_unique<Impl>()) {
    if (samples.size() < 2 || samples.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("A track requires at least two samples within Vulkan limits");
    }
    auto positions = vsg::vec3Array::create(static_cast<std::uint32_t>(samples.size()));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        check_frame(samples[i]);
        if (samples[i].markers.size() != 1 || samples[i].frame != samples.front().frame ||
            (i && samples[i].epoch.tai_nanoseconds() <= samples[i - 1].epoch.tai_nanoseconds()) ||
            samples[i].markers.front().object_id != samples.front().markers.front().object_id) {
            throw std::invalid_argument("A track requires one consistent object ID");
        }
        positions->at(i) = display_position(samples[i].markers.front());
        impl_->epochs.push_back(samples[i].epoch);
    }
    auto draw = vsg::VertexDraw::create();
    draw->vertexCount = static_cast<std::uint32_t>(samples.size());
    impl_->draw = draw;
    impl_->root = geometry(positions, draw, false);
}
TrackLayer::~TrackLayer() = default;
vsg::ref_ptr<vsg::Node> TrackLayer::node() const { return impl_->root; }
std::size_t TrackLayer::size() const { return impl_->draw->vertexCount; }
void TrackLayer::set_interval(core::Epoch first, core::Epoch last) {
    if (last.tai_nanoseconds() < first.tai_nanoseconds())
        throw std::invalid_argument("Reversed display interval");
    const auto &epochs = impl_->epochs;
    if (first == last || first.tai_nanoseconds() > epochs.back().tai_nanoseconds() ||
        last.tai_nanoseconds() < epochs.front().tai_nanoseconds()) {
        impl_->draw->vertexCount = 0;
        return;
    }
    const auto compare = [](core::Epoch a, core::Epoch b) {
        return a.tai_nanoseconds() < b.tai_nanoseconds();
    };
    auto begin = std::upper_bound(epochs.begin(), epochs.end(), first, compare);
    if (begin != epochs.begin())
        --begin;
    auto end = std::lower_bound(epochs.begin(), epochs.end(), last, compare);
    if (end != epochs.end())
        ++end;
    impl_->draw->firstVertex = static_cast<std::uint32_t>(std::distance(epochs.begin(), begin));
    impl_->draw->vertexCount = static_cast<std::uint32_t>(std::distance(begin, end));
}
vsg::ref_ptr<vsg::Node> make_track(std::span<const visualization::RenderSnapshot> samples) {
    return TrackLayer(samples).node();
}
} // namespace orbitforge::render_vsg
