#include "satellite_model.hpp"
#include <cmath>
#include <stdexcept>
#include <vsg/all.h>
#include <vsgXchange/all.h>

namespace orbelle::desktop {
vsg::ref_ptr<vsg::MatrixTransform> make_satellite_model(const std::filesystem::path &path) {
    vsg::ref_ptr<vsg::Node> model;
    if (path.empty()) {
        auto group = vsg::Group::create();
        auto builder = vsg::Builder::create();
        vsg::StateInfo style;
        style.lighting = false;
        const auto box = [&](vsg::vec3 centre, vsg::vec3 size, vsg::vec4 colour) {
            vsg::GeometryInfo info;
            info.position = centre;
            info.dx = {size.x, 0, 0};
            info.dy = {0, size.y, 0};
            info.dz = {0, 0, size.z};
            info.color = colour;
            group->addChild(builder->createBox(info, style));
        };
        box({0, 0, 0}, {1.2f, 0.9f, 1.1f}, {0.91f, 0.72f, 0.30f, 1});
        box({0, 0, 0.6f}, {0.9f, 0.7f, 0.1f}, {0.9f, 0.95f, 1, 1});
        box({0, 0, 0}, {4.3f, 0.08f, 0.08f}, {0.65f, 0.7f, 0.8f, 1});
        for (const float side : {-1.0f, 1.0f}) {
            box({side * 1.6f, 0, 0}, {1.8f, 1.1f, 0.05f}, {0.08f, 0.27f, 0.58f, 1});
            for (const float offset : {-0.6f, 0.0f, 0.6f})
                box({side * 1.6f + offset, 0, 0.04f}, {0.025f, 1.1f, 0.018f},
                    {0.49f, 0.71f, 0.94f, 1});
            box({side * 1.6f, 0, 0.04f}, {1.8f, 0.025f, 0.018f}, {0.49f, 0.71f, 0.94f, 1});
        }
        vsg::GeometryInfo dish;
        dish.position = {0, 0, 0.9f};
        dish.dx = {0.8f, 0, 0};
        dish.dy = {0, 0.8f, 0};
        dish.dz = {0, 0, 0.18f};
        dish.color = vsg::vec4{0.85f, 0.9f, 0.95f, 1};
        group->addChild(builder->createCone(dish, style));
        model = group;
    } else {
        if (!std::filesystem::is_regular_file(path))
            throw std::invalid_argument("The selected model file does not exist");
        auto options = vsg::Options::create();
        options->add(vsgXchange::all::create());
        options->paths.push_back(vsg::Path(path.parent_path().wstring()));
        model = vsg::read_cast<vsg::Node>(vsg::Path(path.wstring()), options);
        if (!model)
            throw std::runtime_error("Cannot load model; use glTF/GLB or VSG format");
    }
    vsg::ComputeBounds bounds;
    model->accept(bounds);
    const auto diameter = vsg::length(bounds.bounds.max - bounds.bounds.min);
    if (!bounds.bounds.valid() || !std::isfinite(diameter) || diameter <= 0)
        throw std::runtime_error("Model has no finite non-empty geometry");
    const auto centre = (bounds.bounds.min + bounds.bounds.max) * 0.5;
    auto normalized =
        vsg::MatrixTransform::create(vsg::scale(1 / diameter) * vsg::translate(-centre));
    normalized->addChild(model);
    return normalized;
}
} // namespace orbelle::desktop
