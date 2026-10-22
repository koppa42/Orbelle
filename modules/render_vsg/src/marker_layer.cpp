#include <cmath>
#include <limits>
#include <mutex>
#include <orbelle/render_vsg/marker_layer.hpp>
#include <stdexcept>
#include <vsg/all.h>

namespace orbelle::render_vsg {
namespace {
constexpr const char *vertex_source = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(location=0) in vec3 position;
layout(location=1) in vec3 input_color;
layout(location=0) out vec3 marker_color;
void main() {
    gl_Position = pc.projection * pc.modelview * vec4(position, 1.0);
    gl_PointSize = 8.0;
    marker_color = input_color;
}
)glsl";
constexpr const char *point_source = R"glsl(#version 450
layout(location=0) in vec3 marker_color;
layout(location=0) out vec4 color;
void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    if (dot(p,p) > 1.0) discard;
    color = vec4(marker_color, 1.0);
}
)glsl";
constexpr const char *station_source = R"glsl(#version 450
layout(location=0) in vec3 marker_color;
layout(location=0) out vec4 color;
void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    if (abs(p.x) + abs(p.y) > 1.0) discard;
    color = vec4(marker_color, 1.0);
}
)glsl";
constexpr const char *selection_vertex_source = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(location=0) in vec3 position;
layout(location=1) in vec3 input_color;
layout(location=0) out vec3 marker_color;
void main() {
    gl_Position = pc.projection * pc.modelview * vec4(position, 1.0);
    gl_PointSize = 18.0;
    marker_color = input_color;
}
)glsl";
constexpr const char *selection_source = R"glsl(#version 450
layout(location=0) in vec3 marker_color;
layout(location=0) out vec4 color;
void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    float r = dot(p,p);
    if (r > 1.0 || r < 0.30) discard;
    color = vec4(marker_color, 1.0);
}
)glsl";

vsg::ref_ptr<vsg::BindGraphicsPipeline> shared_pipeline(MarkerStyle style) {
    // Pipelines belong to the live layers, not to a process-lifetime GPU cache.
    // Their per-view/device implementations are managed by VSG.
    static std::mutex mutex;
    static std::array<vsg::observer_ptr<vsg::BindGraphicsPipeline>, 4> shared;
    const std::lock_guard lock(mutex);
    const auto index = static_cast<std::size_t>(style);
    if (auto pipeline = shared[index].ref_ptr())
        return pipeline;
    auto input = vsg::VertexInputState::create(
        vsg::VertexInputState::Bindings{{0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX},
                                        {1, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX}},
        vsg::VertexInputState::Attributes{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                          {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0}});
    auto assembly = vsg::InputAssemblyState::create();
    assembly->topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    auto layout = vsg::PipelineLayout::create(
        vsg::DescriptorSetLayouts{}, vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}});
    const bool selected = style == MarkerStyle::selection || style == MarkerStyle::ground_selection;
    auto depth = vsg::DepthStencilState::create();
    if (style == MarkerStyle::ground_station || style == MarkerStyle::ground_selection) {
        depth->depthTestEnable = false;
        depth->depthWriteEnable = false;
    }
    if (selected)
        depth->depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    auto pipeline = vsg::GraphicsPipeline::create(
        layout,
        vsg::ShaderStages{
            vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main",
                                     selected ? selection_vertex_source : vertex_source),
            vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main",
                                     selected                               ? selection_source
                                     : style == MarkerStyle::ground_station ? station_source
                                                                            : point_source)},
        vsg::GraphicsPipelineStates{input, assembly, vsg::RasterizationState::create(),
                                    vsg::MultisampleState::create(), vsg::ColorBlendState::create(),
                                    depth});
    auto bind = vsg::BindGraphicsPipeline::create(pipeline);
    shared[index] = bind;
    return bind;
}
vsg::ref_ptr<vsg::StateGroup> geometry(vsg::ref_ptr<vsg::vec3Array> positions,
                                       vsg::ref_ptr<vsg::vec3Array> colors,
                                       vsg::ref_ptr<vsg::VertexDraw> draw,
                                       MarkerStyle style = MarkerStyle::satellite) {
    auto group = vsg::StateGroup::create();
    group->add(shared_pipeline(style));
    draw->assignArrays(vsg::DataList{positions, colors});
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
    vsg::ref_ptr<vsg::vec3Array> colors;
    vsg::ref_ptr<vsg::VertexDraw> draw;
    vsg::ref_ptr<vsg::Node> root;
    std::vector<vsg::vec3> converted;
    std::uint32_t default_color = 0xffb81f;
};
MarkerLayer::MarkerLayer(std::size_t capacity, MarkerStyle style)
    : impl_(std::make_unique<Impl>()) {
    if (static_cast<unsigned>(style) > static_cast<unsigned>(MarkerStyle::ground_selection))
        throw std::invalid_argument("Invalid marker style");
    if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Marker capacity must fit a nonzero Vulkan vertex count");
    }
    impl_->positions = vsg::vec3Array::create(static_cast<std::uint32_t>(capacity));
    impl_->positions->properties.dataVariance = vsg::DYNAMIC_DATA;
    impl_->colors = vsg::vec3Array::create(static_cast<std::uint32_t>(capacity));
    impl_->colors->properties.dataVariance = vsg::DYNAMIC_DATA;
    impl_->draw = vsg::VertexDraw::create();
    impl_->draw->vertexCount = 0;
    impl_->root = geometry(impl_->positions, impl_->colors, impl_->draw, style);
    if (style == MarkerStyle::ground_station)
        impl_->default_color = 0x38d9ad;
    else if (style == MarkerStyle::selection || style == MarkerStyle::ground_selection)
        impl_->default_color = 0xff1abf;
    impl_->converted.reserve(capacity);
}
MarkerLayer::~MarkerLayer() = default;
vsg::ref_ptr<vsg::Node> MarkerLayer::node() const { return impl_->root; }
std::size_t MarkerLayer::size() const noexcept { return impl_->draw->vertexCount; }
void MarkerLayer::update(const visualization::RenderSnapshot &snapshot,
                         std::span<const std::uint32_t> colors, bool srgb_framebuffer) {
    check_frame(snapshot);
    if (snapshot.markers.size() > impl_->positions->size()) {
        throw std::length_error("Snapshot exceeds the allocated marker capacity");
    }
    if (!colors.empty() &&
        (colors.size() != snapshot.markers.size() ||
         std::any_of(colors.begin(), colors.end(), [](auto rgb) { return rgb > 0xffffff; })))
        throw std::invalid_argument("Marker colors must match the snapshot and use 24-bit RGB");
    auto &converted = impl_->converted;
    converted.clear();
    for (const auto &marker : snapshot.markers)
        converted.push_back(display_position(marker));
    bool color_changed = false;
    for (std::size_t i = 0; i < snapshot.markers.size(); ++i) {
        const auto rgb = colors.empty() ? impl_->default_color : colors[i];
        vsg::vec3 color(static_cast<float>((rgb >> 16) & 255) / 255,
                        static_cast<float>((rgb >> 8) & 255) / 255,
                        static_cast<float>(rgb & 255) / 255);
        // Explicit user colors are sRGB; legacy selection/station palettes are linear.
        if (!colors.empty() && srgb_framebuffer)
            for (std::size_t axis = 0; axis < 3; ++axis) {
                auto &component = color[axis];
                component = component <= .04045f ? component / 12.92f
                                                 : std::pow((component + .055f) / 1.055f, 2.4f);
            }
        if (impl_->colors->at(i) != color) {
            impl_->colors->at(i) = color;
            color_changed = true;
        }
    }
    if (color_changed)
        impl_->colors->dirty();
    if (impl_->draw->vertexCount == converted.size() &&
        std::equal(converted.begin(), converted.end(), impl_->positions->begin()))
        return;
    std::copy(converted.begin(), converted.end(), impl_->positions->begin());
    impl_->positions->dirty();
    impl_->draw->vertexCount = static_cast<std::uint32_t>(converted.size());
}
} // namespace orbelle::render_vsg
