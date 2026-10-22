#include <algorithm>
#include <cmath>
#include <limits>
#include <orbelle/render_vsg/labels.hpp>
#include <stdexcept>
#include <vsg/all.h>

namespace orbelle::render_vsg {
namespace {
bool earth_occludes(vsg::dvec3 eye, vsg::dvec3 position) {
    constexpr vsg::dvec3 axes(6378137.0, 6378137.0, 6356752.314245179);
    for (std::size_t k = 0; k < 3; ++k) {
        eye[k] /= axes[k];
        position[k] /= axes[k];
    }
    const auto direction = position - eye;
    const double a = vsg::dot(direction, direction);
    const double b = vsg::dot(eye, direction);
    const double c = vsg::dot(eye, eye) - 1.0;
    const double discriminant = b * b - a * c;
    if (a == 0.0 || discriminant < 0.0)
        return false;
    const double root = std::sqrt(discriminant);
    const double entry = (-b - root) / a, exit = (-b + root) / a;
    return exit >= 0.0 && entry < 1.0 - 1e-10;
}
void require_itrf(const visualization::RenderSnapshot &snapshot) {
    if (snapshot.frame != core::FrameId::itrf || snapshot.origin != core::OriginId::earth)
        throw std::invalid_argument("Display labels require Earth/ITRF positions");
}
} // namespace
std::vector<ProjectedMarker> visible_markers(const visualization::RenderSnapshot &snapshot,
                                             const vsg::Camera &camera, std::uint32_t width,
                                             std::uint32_t height, double margin,
                                             const core::PositionRotation *to_itrf) {
    if (snapshot.frame == core::FrameId::gcrf) {
        if (snapshot.origin != core::OriginId::earth || !to_itrf ||
            to_itrf->epoch != snapshot.epoch || to_itrf->from != core::FrameId::gcrf ||
            to_itrf->to != core::FrameId::itrf || to_itrf->origin != snapshot.origin)
            throw std::invalid_argument(
                "GCRF visibility requires an epoch-matched Earth-fixed rotation");
    } else
        require_itrf(snapshot);
    if (!camera.viewMatrix || !camera.projectionMatrix || !width || !height ||
        !std::isfinite(margin) || margin < 0)
        throw std::invalid_argument("Invalid display viewport");
    const auto view = camera.viewMatrix->transform();
    const auto projection = camera.projectionMatrix->transform() * view;
    const auto eye = vsg::inverse(view) * vsg::dvec3(0, 0, 0);
    const auto fixed = [&](vsg::dvec3 value) {
        if (snapshot.frame == core::FrameId::itrf)
            return value;
        const auto p = to_itrf->apply_position({value.x, value.y, value.z});
        return vsg::dvec3(p[0], p[1], p[2]);
    };
    const auto fixed_eye = fixed(eye);
    std::vector<ProjectedMarker> result;
    for (std::size_t i = 0; i < snapshot.markers.size(); ++i) {
        const auto &p = snapshot.markers[i].position_m;
        const vsg::dvec3 position(p[0], p[1], p[2]);
        const auto clip = projection * vsg::dvec4(position, 1.0);
        if (!std::isfinite(clip.w) || !std::isfinite(clip.z) || clip.w <= 0 || clip.z < 0 ||
            clip.z > clip.w)
            continue;
        const double x = (clip.x / clip.w + 1) * 0.5 * width;
        const double y = (clip.y / clip.w + 1) * 0.5 * height;
        if (!std::isfinite(x) || !std::isfinite(y) || x < margin || x >= width - margin ||
            y < margin || y >= height - margin || earth_occludes(fixed_eye, fixed(position)))
            continue;
        result.push_back({i, x, y});
    }
    return result;
}
std::optional<std::size_t> pick_marker(std::span<const ProjectedMarker> markers, double x, double y,
                                       double radius) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius) || radius <= 0)
        throw std::invalid_argument("Pick coordinates and radius must be finite and valid");
    std::optional<std::size_t> picked;
    double best = radius * radius;
    for (const auto &marker : markers) {
        const double dx = marker.x_pixels - x, dy = marker.y_pixels - y;
        const double squared = dx * dx + dy * dy;
        if (squared < best || (squared == best && (!picked || marker.index < *picked))) {
            best = squared;
            picked = marker.index;
        }
    }
    return picked;
}
struct LabelLayer::Impl {
    vsg::ref_ptr<vsg::Switch> root = vsg::Switch::create();
    std::vector<vsg::ref_ptr<vsg::Text>> labels;
    std::size_t count = 0;
};
LabelLayer::LabelLayer(std::size_t capacity, vsg::ref_ptr<vsg::Font> font)
    : impl_(std::make_unique<Impl>()) {
    if (!capacity || capacity > 10000 || !font || !font->atlas || !font->glyphMetrics ||
        !font->charmap)
        throw std::invalid_argument("Invalid display label capacity/font");
    // The upstream font defaults require anisotropy=16. This fixture uses linear
    // sampling and does not add a device feature requirement just for labels.
    if (!font->atlasImageInfo)
        font->createFontImages();
    font->atlasImageInfo->sampler->anisotropyEnable = VK_FALSE;
    font->atlasImageInfo->sampler->maxAnisotropy = 1.0f;
    auto shader = vsg::createTextShaderSet();
    for (std::size_t i = 0; i < capacity; ++i) {
        auto text = vsg::Text::create();
        text->font = font;
        text->shaderSet = shader;
        text->technique = vsg::GpuLayoutTechnique::create();
        auto layout = vsg::StandardLayout::create();
        layout->billboard = true;
        layout->billboardAutoScaleDistance = 1e8f;
        layout->color = vsg::vec4(0.95f, 0.96f, 1.0f, 1.0f);
        layout->outlineColor = vsg::vec4(0.005f, 0.005f, 0.005f, 1.0f);
        layout->outlineWidth = 0.08f;
        text->layout = layout;
        text->text = vsg::stringValue::create("SAT-00000");
        // VSG 1.1.15 reallocates when length == capacity and doesn't rebind the
        // existing descriptor. Enforce length < 64 for every subsequent update.
        text->setup(64);
        impl_->root->addChild(vsg::MASK_OFF, text);
        impl_->labels.push_back(text);
    }
}
LabelLayer::~LabelLayer() = default;
vsg::ref_ptr<vsg::Node> LabelLayer::node() const { return impl_->root; }
std::size_t LabelLayer::size() const noexcept { return impl_->count; }
void LabelLayer::update(const visualization::RenderSnapshot &snapshot,
                        std::span<const ProjectedMarker> visible, const vsg::Camera &camera,
                        std::uint32_t height) {
    require_itrf(snapshot);
    if (!camera.projectionMatrix)
        throw std::invalid_argument("Invalid label camera");
    const double projection_scale = std::abs(camera.projectionMatrix->transform()[1][1]);
    if (!height || !std::isfinite(projection_scale) || projection_scale <= 0)
        throw std::invalid_argument("Invalid label projection");
    const auto count = std::min(visible.size(), impl_->labels.size());
    // Validate before mutating any published GPU layout.
    for (std::size_t i = 0; i < count; ++i) {
        const auto &marker = snapshot.markers.at(visible[i].index);
        if (marker.object_id.empty() || marker.object_id.size() >= 64 ||
            !std::all_of(marker.object_id.begin(), marker.object_id.end(),
                         [](unsigned char c) { return c >= 32 && c < 127; }))
            throw std::invalid_argument("Labels currently support 1..63 printable ASCII bytes");
        for (const unsigned char c : marker.object_id)
            if (impl_->labels[i]->font->glyphIndexForCharcode(c) == 0)
                throw std::invalid_argument("The fixed font does not contain a requested glyph");
        for (double value : marker.position_m)
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                throw std::invalid_argument("Invalid label display position");
    }
    const float glyph_size = static_cast<float>(12.0 * 1e8 * 2.0 / (height * projection_scale));
    for (std::size_t i = 0; i < impl_->labels.size(); ++i) {
        impl_->root->children[i].mask = i < count ? vsg::MASK_ALL : vsg::MASK_OFF;
        if (i >= count)
            continue;
        const auto &marker = snapshot.markers[visible[i].index];
        const auto &text = impl_->labels[i];
        auto layout = text->layout.cast<vsg::StandardLayout>();
        layout->position = {static_cast<float>(marker.position_m[0]),
                            static_cast<float>(marker.position_m[1]),
                            static_cast<float>(marker.position_m[2])};
        layout->horizontal = {glyph_size, 0, 0};
        layout->vertical = {0, glyph_size, 0};
        text->text.cast<vsg::stringValue>()->value() = marker.object_id;
        text->setup(64);
    }
    impl_->count = count;
}
} // namespace orbelle::render_vsg
