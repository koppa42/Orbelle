#include <algorithm>
#include <orbitforge/geo_rocky/globe.hpp>
#include <rocky/GDALImageLayer.h>
#include <rocky/vsg/DisplayManager.h>
#include <rocky/vsg/MapNode.h>
#include <rocky/vsg/ViewDependentState.h>
#include <stdexcept>

namespace orbitforge::geo_rocky {
namespace {
class TraitsHelper : public rocky::DisplayManager {
  public:
    using rocky::DisplayManager::configureTraits;
};
class Globe final : public render_vsg::IEarthDisplay {
  public:
    Globe(vsg::ref_ptr<vsg::Viewer> viewer, const std::filesystem::path &imagery)
        : context_(rocky::VSGContextFactory::create(viewer)) {
        auto *context = context_.get();
        if (!context->status.ok())
            throw std::runtime_error(context->status.error().message);
        context->activeViewIDs.clear();
        traits_.initialize(context);
        map_ = rocky::MapNode::create(context);
        if (!map_->terrainNode->status.ok())
            throw std::runtime_error(map_->terrainNode->status.error().message);
        map_->terrainSettings().maxLevel = 5u;
        map_->terrainSettings().pixelError = 32.0f;
        map_->terrainSettings().lighting = false;
        map_->terrainSettings().supportMultiThreadedRecord = false;
        if (!imagery.empty()) {
            if (!std::filesystem::is_regular_file(imagery)) {
                throw std::invalid_argument("Explicit local globe imagery does not exist");
            }
            auto layer = rocky::GDALImageLayer::create();
            const auto utf8_path = std::filesystem::absolute(imagery).generic_u8string();
            layer->uri =
                std::string(reinterpret_cast<const char *>(utf8_path.data()), utf8_path.size());
            map_->map->add(layer);
            const auto result = map_->map->openAllLayers(context->io);
            if (result.failed())
                throw std::runtime_error(result.error().message);
        }
    }
    ~Globe() override {
        stop_background_work();
        map_ = {};
    }
    vsg::ref_ptr<vsg::Node> node() const override { return map_; }
    void prepare_window(vsg::WindowTraits &traits, bool validation) override {
        traits_.configureTraits(&traits);
        traits.debugLayer = validation;
        // VSG's synchronizationLayer requests a legacy synchronization2 shim,
        // not synchronization validation. Validation settings are supplied by the test runner.
        traits.synchronizationLayer = false;
        traits.deviceFeatures->get().largePoints = VK_TRUE;
        traits.queueFlags |= VK_QUEUE_COMPUTE_BIT;
    }
    void prepare_view(vsg::View &view) override {
        if (!view.viewDependentState.cast<rocky::ViewDependentStateEx>())
            view.viewDependentState =
                rocky::ViewDependentStateEx::create(vsg::ref_ptr<vsg::View>(&view));
        context_.get()->activeViewIDs.push_back(view.viewID);
    }
    void release_view(const vsg::View &view) override {
        std::erase(context_.get()->activeViewIDs, view.viewID);
    }
    void stop_background_work() override { context_.get()->io.services().jobs.shutdown(); }

  private:
    rocky::VSGContextSingleton context_;
    TraitsHelper traits_;
    vsg::ref_ptr<rocky::MapNode> map_;
};
} // namespace
std::unique_ptr<render_vsg::IEarthDisplay> make_globe(vsg::ref_ptr<vsg::Viewer> viewer,
                                                      const std::filesystem::path &imagery) {
    if (!viewer)
        throw std::invalid_argument("Rocky needs a live rendering viewer");
    return std::make_unique<Globe>(std::move(viewer), imagery);
}
} // namespace orbitforge::geo_rocky
