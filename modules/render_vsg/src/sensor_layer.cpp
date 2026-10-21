#include <cmath>
#include <limits>
#include <orbitforge/render_vsg/sensor_layer.hpp>
#include <stdexcept>
#include <vsg/all.h>
namespace orbitforge::render_vsg {
namespace {
constexpr const char *vertex = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(location=0) in vec3 position;
layout(location=1) in vec4 color;
layout(location=0) out vec4 tint;
void main(){gl_Position=pc.projection*pc.modelview*vec4(position,1);tint=color;}
)glsl";
constexpr const char *fragment = R"glsl(#version 450
layout(location=0) in vec4 tint;
layout(location=0) out vec4 color;
void main(){color=tint;}
)glsl";
vsg::ref_ptr<vsg::BindGraphicsPipeline> pipeline(bool lines) {
    auto input = vsg::VertexInputState::create(
        vsg::VertexInputState::Bindings{{0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX},
                                        {1, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_VERTEX}},
        vsg::VertexInputState::Attributes{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                          {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}});
    auto assembly = vsg::InputAssemblyState::create();
    assembly->topology =
        lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    auto depth = vsg::DepthStencilState::create();
    depth->depthWriteEnable = false;
    depth->depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    auto raster = vsg::RasterizationState::create();
    raster->cullMode = VK_CULL_MODE_NONE;
    auto blend = vsg::ColorBlendState::create();
    auto &attachment = blend->attachments.front();
    attachment.blendEnable = true;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    auto layout = vsg::PipelineLayout::create(
        vsg::DescriptorSetLayouts{}, vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}});
    return vsg::BindGraphicsPipeline::create(vsg::GraphicsPipeline::create(
        layout,
        vsg::ShaderStages{vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertex),
                          vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", fragment)},
        vsg::GraphicsPipelineStates{input, assembly, raster, vsg::MultisampleState::create(), blend,
                                    depth}));
}
} // namespace
struct SensorLayer::Impl {
    std::size_t capacity;
    vsg::ref_ptr<vsg::vec3Array> positions;
    vsg::ref_ptr<vsg::vec4Array> colors;
    vsg::ref_ptr<vsg::VertexDraw> triangles = vsg::VertexDraw::create(),
                                  lines = vsg::VertexDraw::create();
    vsg::ref_ptr<vsg::Group> root = vsg::Group::create();
};
SensorLayer::SensorLayer(std::size_t capacity) : impl_(std::make_unique<Impl>()) {
    if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Sensor vertex capacity invalid");
    impl_->capacity = capacity;
    impl_->positions = vsg::vec3Array::create(static_cast<std::uint32_t>(capacity));
    impl_->colors = vsg::vec4Array::create(static_cast<std::uint32_t>(capacity));
    impl_->positions->properties.dataVariance = vsg::DYNAMIC_DATA;
    impl_->colors->properties.dataVariance = vsg::DYNAMIC_DATA;
    for (bool lines : {false, true}) {
        auto group = vsg::StateGroup::create();
        group->add(pipeline(lines));
        auto draw = lines ? impl_->lines : impl_->triangles;
        draw->assignArrays(vsg::DataList{impl_->positions, impl_->colors});
        draw->instanceCount = 1;
        draw->vertexCount = 0;
        group->addChild(draw);
        impl_->root->addChild(group);
    }
}
SensorLayer::~SensorLayer() = default;
vsg::ref_ptr<vsg::Node> SensorLayer::node() const { return impl_->root; }
std::size_t SensorLayer::capacity() const { return impl_->capacity; }
std::size_t SensorLayer::required(std::span<const visualization::SensorMesh> meshes) {
    std::size_t n = 0;
    for (const auto &m : meshes)
        n += (m.fill ? m.sides.size() * 3 : 0) + (m.edges.size() + m.ground_edges.size()) * 2;
    return n;
}
void SensorLayer::update(std::span<const visualization::SensorMesh> meshes, bool srgb) {
    if (required(meshes) > capacity())
        throw std::length_error("Sensor vertex capacity exceeded");
    std::size_t i = 0;
    auto append = [&](const core::SensorVector &p, const visualization::SensorMesh &m, bool line,
                      bool surface) {
        const double lift = surface ? 1.00003 : 1;
        impl_->positions->at(i) = {static_cast<float>(p[0] * lift), static_cast<float>(p[1] * lift),
                                   static_cast<float>(p[2] * lift)};
        vsg::vec4 color(static_cast<float>((m.color >> 16) & 255) / 255,
                        static_cast<float>((m.color >> 8) & 255) / 255,
                        static_cast<float>(m.color & 255) / 255,
                        static_cast<float>(line ? .85 : m.opacity));
        if (srgb)
            for (std::size_t k = 0; k < 3; ++k)
                color[k] = color[k] <= .04045f ? color[k] / 12.92f
                                               : std::pow((color[k] + .055f) / 1.055f, 2.4f);
        impl_->colors->at(i++) = color;
    };
    for (const auto &m : meshes)
        if (m.fill)
            for (const auto &t : m.sides)
                for (const auto &p : t)
                    append(p, m, false, false);
    impl_->triangles->vertexCount = static_cast<std::uint32_t>(i);
    impl_->lines->firstVertex = static_cast<std::uint32_t>(i);
    for (const auto &m : meshes) {
        for (const auto &edge : m.edges)
            for (const auto &p : edge)
                append(p, m, true, false);
        for (const auto &edge : m.ground_edges)
            for (const auto &p : edge)
                append(p, m, true, true);
    }
    impl_->lines->vertexCount = static_cast<std::uint32_t>(i) - impl_->lines->firstVertex;
    impl_->positions->dirty();
    impl_->colors->dirty();
}
} // namespace orbitforge::render_vsg
