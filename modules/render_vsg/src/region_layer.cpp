#include <cmath>
#include <limits>
#include <orbelle/render_vsg/marker_layer.hpp>
#include <orbelle/render_vsg/region_layer.hpp>
#include <stdexcept>
#include <vsg/all.h>

namespace orbelle::render_vsg {
namespace {
constexpr const char *vertex = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(location=0) in vec3 position;
void main(){gl_Position=pc.projection*pc.modelview*vec4(position,1);}
)glsl";
constexpr const char *fragment = R"glsl(#version 450
layout(set=0,binding=0) uniform Appearance { vec4 tint; } style;
layout(location=0) out vec4 color;
void main(){color=style.tint;}
)glsl";
vsg::ref_ptr<vsg::BindGraphicsPipeline> fill_pipeline() {
    auto input = vsg::VertexInputState::create(
        vsg::VertexInputState::Bindings{{0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX}},
        vsg::VertexInputState::Attributes{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}});
    auto assembly = vsg::InputAssemblyState::create();
    assembly->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
    auto descriptor_layout = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    auto layout =
        vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{descriptor_layout},
                                    vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}});
    return vsg::BindGraphicsPipeline::create(vsg::GraphicsPipeline::create(
        layout,
        vsg::ShaderStages{vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertex),
                          vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", fragment)},
        vsg::GraphicsPipelineStates{input, assembly, raster, vsg::MultisampleState::create(), blend,
                                    depth}));
}
} // namespace
struct RegionLayer::Impl {
    vsg::ref_ptr<vsg::Group> root = vsg::Group::create();
    std::unique_ptr<TrackLayer> outline;
    std::uint32_t color = 0;
    double ratio = 1;
    bool srgb = false;
};
RegionLayer::RegionLayer(const visualization::RegionMesh &mesh, std::uint32_t color, double opacity,
                         double ratio, bool srgb)
    : impl_(std::make_unique<Impl>()) {
    if (color > 0xffffff || !std::isfinite(opacity) || opacity < 0 || opacity > 1 ||
        mesh.triangles.size() > std::numeric_limits<std::uint32_t>::max() / 3)
        throw std::invalid_argument("Invalid region appearance or mesh size");
    impl_->color = color;
    impl_->ratio = ratio;
    impl_->srgb = srgb;
    if (!mesh.triangles.empty() && opacity > 0) {
        auto positions =
            vsg::vec3Array::create(static_cast<std::uint32_t>(mesh.triangles.size() * 3));
        std::size_t i = 0;
        for (const auto &triangle : mesh.triangles)
            for (const auto &p : triangle)
                positions->at(i++) = vsg::vec3(static_cast<float>(p[0]), static_cast<float>(p[1]),
                                               static_cast<float>(p[2]));
        auto tint = vsg::vec4Array::create(1);
        tint->at(0) = vsg::vec4(float((color >> 16) & 255) / 255, float((color >> 8) & 255) / 255,
                                float(color & 255) / 255, static_cast<float>(opacity));
        if (srgb)
            for (std::size_t channel = 0; channel < 3; ++channel) {
                auto &v = tint->at(0)[channel];
                v = v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
            }
        auto bind = fill_pipeline();
        auto group = vsg::StateGroup::create();
        group->add(bind);
        auto descriptor =
            vsg::DescriptorBuffer::create(tint, 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        auto set = vsg::DescriptorSet::create(bind->pipeline->layout->setLayouts.front(),
                                              vsg::Descriptors{descriptor});
        group->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  bind->pipeline->layout, 0, set));
        auto draw = vsg::VertexDraw::create();
        draw->assignArrays(vsg::DataList{positions});
        draw->vertexCount = static_cast<std::uint32_t>(positions->size());
        draw->instanceCount = 1;
        group->addChild(draw);
        impl_->root->addChild(group);
    }
    std::vector<visualization::RenderSnapshot> samples;
    for (const auto &p : mesh.outline)
        samples.push_back({core::Epoch{static_cast<std::int64_t>(samples.size())},
                           core::FrameId::itrf,
                           core::OriginId::earth,
                           {{"region-outline", p}}});
    impl_->outline = std::make_unique<TrackLayer>(samples);
    set_selected(false);
    impl_->root->addChild(impl_->outline->node());
}
RegionLayer::~RegionLayer() = default;
vsg::ref_ptr<vsg::Node> RegionLayer::node() const { return impl_->root; }
void RegionLayer::set_selected(bool selected) {
    impl_->outline->set_appearance(impl_->color, selected ? 2.5 : 1.5, 0, impl_->ratio,
                                   impl_->srgb);
}
} // namespace orbelle::render_vsg
