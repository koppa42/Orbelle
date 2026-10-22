#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <orbelle/render_vsg/marker_layer.hpp>
#include <stdexcept>
#include <vsg/all.h>

namespace orbelle::render_vsg {
namespace {
// Screen-space triangles avoid optional Vulkan wideLines and line-stipple features.
// Each instance is one segment, with adjacent positions for bounded miter joins.
constexpr const char *vertex_source = R"glsl(#version 450
layout(push_constant) uniform Matrices { mat4 projection; mat4 modelview; } pc;
layout(set=0, binding=0) uniform Appearance { vec4 color; vec4 parameters; } style;
layout(set=1, binding=1) readonly buffer Viewports { vec4 viewport[]; } view;
layout(location=0) in vec3 previous;
layout(location=1) in vec3 start;
layout(location=2) in vec3 end;
layout(location=3) in vec3 next;
layout(location=0) noperspective out float lateral;
layout(location=1) flat out vec2 dash_direction;
vec2 pixel(vec4 clip) { return clip.xy / clip.w * view.viewport[0].zw * .5; }
vec2 unit(vec2 v, vec2 fallback) { return dot(v,v) > 1e-8 ? normalize(v) : fallback; }
vec2 join(vec2 before, vec2 after, vec2 fallback) {
    vec2 tangent = unit(before + after, fallback);
    vec2 normal = vec2(-tangent.y, tangent.x);
    float scale = dot(normal, vec2(-fallback.y, fallback.x));
    return scale > .5 ? normal / scale : vec2(-fallback.y, fallback.x);
}
void main() {
    mat4 transform = pc.projection * pc.modelview;
    vec4 a = transform * vec4(start, 1);
    vec4 b = transform * vec4(end, 1);
    // Clip the segment against the reverse-depth near plane before dividing by w.
    float da = a.w - a.z, db = b.w - b.z;
    if ((da < 0 && db < 0) || (a.w <= 0 && b.w <= 0)) {
        gl_Position = vec4(2, 2, 2, 1);
        lateral = 0;
        dash_direction = vec2(1, 0);
        return;
    }
    bool clipped_a = da < 0, clipped_b = db < 0;
    if (clipped_a) a = mix(a, b, da / (da - db));
    else if (clipped_b) b = mix(a, b, da / (da - db));
    vec2 dir = unit(pixel(b) - pixel(a), vec2(1, 0));
    const int endpoint[6] = int[6](0, 0, 1, 1, 0, 1);
    const float side[6] = float[6](-1, 1, -1, -1, 1, 1);
    bool at_end = endpoint[gl_VertexIndex] == 1;
    vec4 current = at_end ? b : a;
    vec2 offset = vec2(-dir.y, dir.x);
    vec4 neighbor = transform * vec4(at_end ? next : previous, 1);
    if (neighbor.w > 0 && neighbor.w >= neighbor.z &&
        !(at_end ? clipped_b : clipped_a)) {
        vec2 other = unit(at_end ? pixel(neighbor) - pixel(b) : pixel(a) - pixel(neighbor), dir);
        offset = join(dir, other, dir);
    }
    float half_width = style.parameters.x * style.parameters.z * .5;
    lateral = side[gl_VertexIndex] * (half_width + .75);
    current.xy += offset * lateral * 2 / view.viewport[0].zw * current.w;
    gl_Position = current;
    // Quantized screen-space phase keeps dense samples from restarting every dash.
    float angle = round(atan(dir.y, dir.x) / .7853981634) * .7853981634;
    dash_direction = vec2(cos(angle), sin(angle));
}
)glsl";
constexpr const char *fragment_source = R"glsl(#version 450
layout(set=0, binding=0) uniform Appearance { vec4 color; vec4 parameters; } style;
layout(location=0) noperspective in float lateral;
layout(location=1) flat in vec2 dash_direction;
layout(location=0) out vec4 color;
void main() {
    float width = style.parameters.x * style.parameters.z;
    int pattern = int(style.parameters.y);
    float distance = dot(gl_FragCoord.xy, dash_direction) / width;
    if (pattern == 1 && mod(distance, 6) >= 4) discard;
    if (pattern == 2 && mod(distance, 3) >= 1) discard;
    float phase = mod(distance, 9);
    if (pattern == 3 && ((phase >= 4 && phase < 6) || phase >= 7)) discard;
    float alpha = 1 - smoothstep(width * .5 - .5, width * .5 + .75, abs(lateral));
    if (alpha <= 0) discard;
    vec3 rgb = style.color.rgb;
    if (style.parameters.w > .5)
        rgb = mix(rgb / 12.92, pow((rgb + .055) / 1.055, vec3(2.4)), greaterThan(rgb, vec3(.04045)));
    color = vec4(rgb, alpha);
}
)glsl";
vsg::ref_ptr<vsg::BindGraphicsPipeline> shared_pipeline() {
    static std::mutex mutex;
    static vsg::observer_ptr<vsg::BindGraphicsPipeline> shared;
    const std::lock_guard lock(mutex);
    if (auto existing = shared.ref_ptr())
        return existing;
    vsg::VertexInputState::Bindings bindings;
    vsg::VertexInputState::Attributes attributes;
    for (std::uint32_t i = 0; i < 4; ++i) {
        bindings.push_back({i, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_INSTANCE});
        attributes.push_back({i, i, VK_FORMAT_R32G32B32_SFLOAT, 0});
    }
    auto input = vsg::VertexInputState::create(bindings, attributes);
    auto appearance = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    auto layout = vsg::PipelineLayout::create(
        vsg::DescriptorSetLayouts{appearance, vsg::ViewDescriptorSetLayout::create()},
        vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}});
    auto raster = vsg::RasterizationState::create();
    raster->cullMode = VK_CULL_MODE_NONE;
    auto blend = vsg::ColorBlendState::create();
    blend->configureAttachments(true);
    auto depth = vsg::DepthStencilState::create();
    depth->depthWriteEnable = false;
    auto pipeline = vsg::GraphicsPipeline::create(
        layout,
        vsg::ShaderStages{
            vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertex_source),
            vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", fragment_source)},
        vsg::GraphicsPipelineStates{input, vsg::InputAssemblyState::create(), raster,
                                    vsg::MultisampleState::create(), blend, depth});
    auto bind = vsg::BindGraphicsPipeline::create(pipeline);
    shared = bind;
    return bind;
}
} // namespace
struct TrackLayer::Impl {
    vsg::ref_ptr<vsg::Node> root;
    vsg::ref_ptr<vsg::VertexDraw> draw;
    vsg::ref_ptr<vsg::vec4Array> appearance;
    std::vector<core::Epoch> epochs;
};
TrackLayer::TrackLayer(std::span<const visualization::RenderSnapshot> samples)
    : impl_(std::make_unique<Impl>()) {
    if (samples.size() < 2 || samples.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("A track requires at least two samples within Vulkan limits");
    std::vector<vsg::vec3> positions;
    positions.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto &sample = samples[i];
        if ((sample.frame != core::FrameId::itrf && sample.frame != core::FrameId::gcrf) ||
            sample.origin != core::OriginId::earth || sample.markers.size() != 1 ||
            sample.frame != samples.front().frame ||
            (i && sample.epoch.tai_nanoseconds() <= samples[i - 1].epoch.tai_nanoseconds()) ||
            sample.markers.front().object_id != samples.front().markers.front().object_id)
            throw std::invalid_argument("A track requires one consistent Earth object and frame");
        const auto &point = sample.markers.front().position_m;
        for (const auto value : point)
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                throw std::invalid_argument("Position cannot be represented in the display buffer");
        positions.emplace_back(static_cast<float>(point[0]), static_cast<float>(point[1]),
                               static_cast<float>(point[2]));
        impl_->epochs.push_back(sample.epoch);
    }
    const auto count = static_cast<std::uint32_t>(samples.size() - 1);
    auto previous = vsg::vec3Array::create(count), start = vsg::vec3Array::create(count),
         end = vsg::vec3Array::create(count), next = vsg::vec3Array::create(count);
    for (std::size_t i = 0; i < count; ++i) {
        previous->at(i) = positions[i == 0 ? 0 : i - 1];
        start->at(i) = positions[i];
        end->at(i) = positions[i + 1];
        next->at(i) = positions[std::min(i + 2, positions.size() - 1)];
    }
    impl_->draw = vsg::VertexDraw::create();
    impl_->draw->assignArrays(vsg::DataList{previous, start, end, next});
    impl_->draw->vertexCount = 6;
    impl_->draw->instanceCount = count;
    impl_->appearance = vsg::vec4Array::create(2);
    impl_->appearance->properties.dataVariance = vsg::DYNAMIC_DATA;
    set_appearance(0x73cdee, 1.5, 0);
    auto pipeline = shared_pipeline();
    auto layout = pipeline->pipeline->layout;
    auto descriptor =
        vsg::DescriptorBuffer::create(impl_->appearance, 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    auto set = vsg::DescriptorSet::create(layout->setLayouts.front(), vsg::Descriptors{descriptor});
    auto group = vsg::StateGroup::create();
    group->add(pipeline);
    group->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, set));
    group->add(vsg::BindViewDescriptorSets::create(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1));
    group->addChild(impl_->draw);
    impl_->root = group;
}
TrackLayer::~TrackLayer() = default;
vsg::ref_ptr<vsg::Node> TrackLayer::node() const { return impl_->root; }
std::size_t TrackLayer::size() const {
    return impl_->draw->instanceCount ? impl_->draw->instanceCount + 1 : 0;
}
void TrackLayer::set_appearance(std::uint32_t rgb, double width, unsigned pattern, double ratio,
                                bool srgb_framebuffer) {
    if (rgb > 0xffffff || !std::isfinite(width) || width < .5 || width > 8 || pattern > 3 ||
        !std::isfinite(ratio) || ratio <= 0 || ratio > 16)
        throw std::invalid_argument("Invalid track appearance");
    const vsg::vec4 color(static_cast<float>((rgb >> 16) & 255) / 255,
                          static_cast<float>((rgb >> 8) & 255) / 255,
                          static_cast<float>(rgb & 255) / 255, 1);
    const vsg::vec4 parameters(static_cast<float>(width), static_cast<float>(pattern),
                               static_cast<float>(ratio), srgb_framebuffer ? 1.f : 0.f);
    if (impl_->appearance->at(0) == color && impl_->appearance->at(1) == parameters)
        return;
    impl_->appearance->at(0) = color;
    impl_->appearance->at(1) = parameters;
    impl_->appearance->dirty();
}
void TrackLayer::set_interval(core::Epoch first, core::Epoch last) {
    if (last.tai_nanoseconds() < first.tai_nanoseconds())
        throw std::invalid_argument("Reversed display interval");
    const auto &epochs = impl_->epochs;
    if (first == last || first.tai_nanoseconds() > epochs.back().tai_nanoseconds() ||
        last.tai_nanoseconds() < epochs.front().tai_nanoseconds()) {
        impl_->draw->instanceCount = 0;
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
    impl_->draw->firstInstance = static_cast<std::uint32_t>(std::distance(epochs.begin(), begin));
    impl_->draw->instanceCount = static_cast<std::uint32_t>(std::distance(begin, end) - 1);
}
vsg::ref_ptr<vsg::Node> make_track(std::span<const visualization::RenderSnapshot> samples) {
    return TrackLayer(samples).node();
}
} // namespace orbelle::render_vsg
