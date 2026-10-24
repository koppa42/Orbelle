#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <orbelle/render_vsg/sensor_layer.hpp>
#include <stdexcept>
#include <vsg/all.h>
namespace orbelle::render_vsg {
namespace {
vsg::dvec3 surface_direction(const core::SensorVector &point) {
    return vsg::normalize(
        vsg::dvec3(point[0] / 6378137., point[1] / 6378137., point[2] / 6356752.314245179));
}
std::size_t surface_segments(const std::array<core::SensorVector, 2> &edge) {
    const auto cosine = vsg::dot(surface_direction(edge[0]), surface_direction(edge[1]));
    // Half-degree chords sag by at most 61 m, below the 190 m display lift.
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(std::acos(std::clamp(cosine, -1., 1.)) / (std::numbers::pi / 360))));
}
constexpr const char *vertex = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(location=0) in vec3 position;
layout(location=1) in vec4 color;
layout(location=0) out vec4 tint;
layout(location=1) out vec3 earth_position;
void main(){gl_Position=pc.projection*pc.modelview*vec4(position,1);tint=color;earth_position=position;}
)glsl";
constexpr const char *fragment = R"glsl(#version 450
layout(location=0) in vec4 tint;
layout(location=1) in vec3 earth_position;
layout(location=0) out vec4 color;
void main(){
    vec3 p=earth_position/vec3(6378137.0,6378137.0,6356752.314245179);
    float height=dot(p,p)-1;
    float coverage=smoothstep(0,max(fwidth(height),1e-7),height);
    if(coverage<=0) discard;
    color=vec4(tint.rgb,tint.a*coverage);
}
)glsl";
// Instanced screen-space quads give every view antialiased outlines without
// optional Vulkan wideLines or a multisampled framebuffer.
constexpr const char *line_vertex = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(set=0,binding=1) readonly buffer Viewports { vec4 viewport[]; } view;
layout(location=0) in vec3 start;
layout(location=1) in vec3 end;
layout(location=2) in vec4 appearance;
layout(location=3) in float width;
layout(location=0) flat out vec4 tint;
layout(location=1) noperspective out vec2 coordinate;
layout(location=2) flat out float segment_length;
layout(location=3) flat out float line_width;
void main(){
    tint=appearance;
    line_width=width;
    coordinate=vec2(0);
    segment_length=0;
    mat4 transform=pc.projection*pc.modelview;
    vec4 a=transform*vec4(start,1),b=transform*vec4(end,1);
    float da=a.w-a.z,db=b.w-b.z;
    if((da<0&&db<0)||(a.w<=0&&b.w<=0)){
        gl_Position=vec4(2,2,2,1);return;
    }
    if(da<0) a=mix(a,b,da/(da-db));
    else if(db<0) b=mix(a,b,da/(da-db));
    vec2 delta=(b.xy/b.w-a.xy/a.w)*view.viewport[0].zw*.5;
    segment_length=length(delta);
    vec2 direction=segment_length>1e-4?delta/segment_length:vec2(1,0);
    const int endpoint[6]=int[6](0,0,1,1,0,1);
    const float side[6]=float[6](-1,1,-1,-1,1,1);
    bool at_end=endpoint[gl_VertexIndex]==1;
    float extent=width*.5+.75;
    float extension=at_end?extent:-extent;
    coordinate=vec2((at_end?segment_length:0)+extension,side[gl_VertexIndex]*extent);
    vec4 current=at_end?b:a;
    current.xy+=(direction*extension+vec2(-direction.y,direction.x)*coordinate.y)*2/view.viewport[0].zw*current.w;
    gl_Position=current;
}
)glsl";
constexpr const char *line_fragment = R"glsl(#version 450
layout(location=0) flat in vec4 tint;
layout(location=1) noperspective in vec2 coordinate;
layout(location=2) flat in float segment_length;
layout(location=3) flat in float line_width;
layout(location=0) out vec4 color;
void main(){
    float cap=max(max(-coordinate.x,coordinate.x-segment_length),0);
    float distance=length(vec2(cap,coordinate.y));
    float alpha=1-smoothstep(line_width*.5-.5,line_width*.5+.75,distance);
    if(alpha<=0) discard;
    color=vec4(tint.rgb,tint.a*alpha);
}
)glsl";
vsg::ref_ptr<vsg::BindGraphicsPipeline> pipeline(bool lines) {
    auto input =
        lines ? vsg::VertexInputState::create(
                    vsg::VertexInputState::Bindings{
                        {0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_INSTANCE},
                        {1, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_INSTANCE},
                        {2, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_INSTANCE},
                        {3, sizeof(float), VK_VERTEX_INPUT_RATE_INSTANCE}},
                    vsg::VertexInputState::Attributes{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                      {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                      {2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                      {3, 3, VK_FORMAT_R32_SFLOAT, 0}})
              : vsg::VertexInputState::create(
                    vsg::VertexInputState::Bindings{
                        {0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX},
                        {1, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_VERTEX}},
                    vsg::VertexInputState::Attributes{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                      {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}});
    auto assembly = vsg::InputAssemblyState::create();
    assembly->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    auto depth = vsg::DepthStencilState::create();
    depth->depthWriteEnable = false;
    depth->depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    auto raster = vsg::RasterizationState::create();
    raster->cullMode = VK_CULL_MODE_NONE;
    // Expanded screen-space strokes cover pixels beside the geometric line.
    // Bias their depth by the local slope so Earth does not cut the AA fringe.
    raster->depthBiasEnable = lines;
    raster->depthBiasConstantFactor = 1;
    raster->depthBiasSlopeFactor = 1;
    auto blend = vsg::ColorBlendState::create();
    auto &attachment = blend->attachments.front();
    attachment.blendEnable = true;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    auto layouts = lines ? vsg::DescriptorSetLayouts{vsg::ViewDescriptorSetLayout::create()}
                         : vsg::DescriptorSetLayouts{};
    auto layout = vsg::PipelineLayout::create(
        layouts, vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}});
    return vsg::BindGraphicsPipeline::create(vsg::GraphicsPipeline::create(
        layout,
        vsg::ShaderStages{vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main",
                                                   lines ? line_vertex : vertex),
                          vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main",
                                                   lines ? line_fragment : fragment)},
        vsg::GraphicsPipelineStates{input, assembly, raster, vsg::MultisampleState::create(), blend,
                                    depth}));
}
} // namespace
struct SensorLayer::Impl {
    std::size_t capacity;
    vsg::ref_ptr<vsg::vec3Array> positions;
    vsg::ref_ptr<vsg::vec4Array> colors;
    vsg::ref_ptr<vsg::vec3Array> line_starts, line_ends;
    vsg::ref_ptr<vsg::vec4Array> line_colors;
    vsg::ref_ptr<vsg::floatArray> line_widths;
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
    const auto edge_capacity = static_cast<std::uint32_t>(std::max<std::size_t>(1, capacity / 2));
    impl_->line_starts = vsg::vec3Array::create(edge_capacity);
    impl_->line_ends = vsg::vec3Array::create(edge_capacity);
    impl_->line_colors = vsg::vec4Array::create(edge_capacity);
    impl_->line_widths = vsg::floatArray::create(edge_capacity);
    impl_->line_starts->properties.dataVariance = vsg::DYNAMIC_DATA;
    impl_->line_ends->properties.dataVariance = vsg::DYNAMIC_DATA;
    impl_->line_colors->properties.dataVariance = vsg::DYNAMIC_DATA;
    impl_->line_widths->properties.dataVariance = vsg::DYNAMIC_DATA;
    for (bool lines : {false, true}) {
        auto group = vsg::StateGroup::create();
        auto bind = pipeline(lines);
        group->add(bind);
        if (lines)
            group->add(vsg::BindViewDescriptorSets::create(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                           bind->pipeline->layout, 0));
        auto draw = lines ? impl_->lines : impl_->triangles;
        draw->assignArrays(lines ? vsg::DataList{impl_->line_starts, impl_->line_ends,
                                                 impl_->line_colors, impl_->line_widths}
                                 : vsg::DataList{impl_->positions, impl_->colors});
        draw->instanceCount = lines ? 0 : 1;
        draw->vertexCount = lines ? 6 : 0;
        group->addChild(draw);
        impl_->root->addChild(group);
    }
}
SensorLayer::~SensorLayer() = default;
vsg::ref_ptr<vsg::Node> SensorLayer::node() const { return impl_->root; }
std::size_t SensorLayer::capacity() const { return impl_->capacity; }
std::size_t SensorLayer::required(std::span<const visualization::SensorMesh> meshes) {
    std::size_t n = 0;
    for (const auto &m : meshes) {
        n += (m.fill ? m.sides.size() * 3 : 0) + m.edges.size() * 2;
        if (m.show_space_extension)
            n += (m.fill ? m.space_sides.size() * 3 : 0) + m.space_edges.size() * 2;
        else if (m.fill)
            n += m.limb_sides.size() * 3;
        for (const auto &edge : m.ground_edges)
            n += surface_segments(edge) * 2;
    }
    return n;
}
void SensorLayer::update(std::span<const visualization::SensorMesh> meshes, bool srgb,
                         double ratio) {
    if (!std::isfinite(ratio) || ratio <= 0 || ratio > 16)
        throw std::invalid_argument("Invalid sensor display pixel ratio");
    if (required(meshes) > capacity())
        throw std::length_error("Sensor vertex capacity exceeded");
    std::size_t i = 0;
    auto position = [](const core::SensorVector &p, bool surface) {
        const double lift = surface ? 1.00003 : 1;
        return vsg::vec3(static_cast<float>(p[0] * lift), static_cast<float>(p[1] * lift),
                         static_cast<float>(p[2] * lift));
    };
    auto tint = [&](const visualization::SensorMesh &m, float alpha) {
        vsg::vec4 color(static_cast<float>((m.color >> 16) & 255) / 255,
                        static_cast<float>((m.color >> 8) & 255) / 255,
                        static_cast<float>(m.color & 255) / 255, alpha);
        if (srgb)
            for (std::size_t k = 0; k < 3; ++k)
                color[k] = color[k] <= .04045f ? color[k] / 12.92f
                                               : std::pow((color[k] + .055f) / 1.055f, 2.4f);
        return color;
    };
    for (const auto &m : meshes) {
        const auto append = [&](const auto &triangles, double opacity, bool lift_rim = false) {
            if (m.fill)
                for (const auto &t : triangles)
                    for (std::size_t corner = 0; corner < t.size(); ++corner) {
                        impl_->positions->at(i) = position(t[corner], lift_rim && corner > 0);
                        impl_->colors->at(i++) = tint(m, static_cast<float>(opacity));
                    }
        };
        append(m.sides, m.opacity);
        if (m.show_space_extension)
            append(m.space_sides, m.opacity * .25);
        else
            append(m.limb_sides, m.opacity, true);
    }
    impl_->triangles->vertexCount = static_cast<std::uint32_t>(i);
    i = 0;
    for (const auto &m : meshes) {
        const auto append_edge = [&](const auto &edge, bool surface, float opacity = .85f) {
            impl_->line_starts->at(i) = position(edge[0], surface);
            impl_->line_ends->at(i) = position(edge[1], surface);
            impl_->line_colors->at(i) = tint(m, opacity);
            impl_->line_widths->at(i++) = static_cast<float>(1.2 * ratio);
        };
        for (const auto &edge : m.edges)
            append_edge(edge, false);
        if (m.show_space_extension)
            for (const auto &edge : m.space_edges)
                append_edge(edge, false, .3f);
        for (const auto &edge : m.ground_edges) {
            const auto segments = surface_segments(edge);
            const auto first = surface_direction(edge[0]), last = surface_direction(edge[1]);
            auto previous = edge[0];
            for (std::size_t j = 1; j <= segments; ++j) {
                const auto t = static_cast<double>(j) / static_cast<double>(segments);
                const auto direction = vsg::normalize(first * (1 - t) + last * t);
                const core::SensorVector next{direction.x * 6378137., direction.y * 6378137.,
                                              direction.z * 6356752.314245179};
                append_edge(std::array{previous, next}, true);
                previous = next;
            }
        }
    }
    impl_->lines->instanceCount = static_cast<std::uint32_t>(i);
    impl_->positions->dirty();
    impl_->colors->dirty();
    impl_->line_starts->dirty();
    impl_->line_ends->dirty();
    impl_->line_colors->dirty();
    impl_->line_widths->dirty();
}
} // namespace orbelle::render_vsg
