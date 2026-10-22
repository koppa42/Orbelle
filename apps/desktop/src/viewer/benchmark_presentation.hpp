#pragma once
#include <QJsonObject>
#include <array>
#include <set>
#include <vsg/app/Presentation.h>
#include <vsg/app/Window.h>

namespace orbelle::desktop {
// Query the actual Qt window surface; an advertised device extension alone
// does not promise display-stage timestamps for this presentation path.
QJsonObject present_timing_capabilities(vsg::Window &window);
void prepare_present_timing(vsg::WindowTraits &traits);
class BenchmarkRun;
class BenchmarkPresentation {
  public:
    explicit BenchmarkPresentation(std::array<vsg::ref_ptr<vsg::Window>, 2> windows,
                                   bool output_only_probe = false);
    VkResult present(vsg::Presentation &presentation, std::uint64_t frame, BenchmarkRun &run);
    void poll(BenchmarkRun &run);
    [[nodiscard]] std::size_t pending_count() const;
    [[nodiscard]] QJsonObject metadata() const;

  private:
    std::array<vsg::ref_ptr<vsg::Window>, 2> windows_;
    const VkPresentStageFlagsEXT stages_requested_;
    std::array<VkSwapchainKHR, 2> swapchains_{};
    std::array<std::uint64_t, 2> domain_ids_{}, domain_counters_{};
    std::array<VkTimeDomainKHR, 2> domains_{};
    std::array<std::set<std::uint64_t>, 2> pending_;
    std::array<std::uint64_t, 2> properties_counters_{};
    std::array<std::uint64_t, 2> last_properties_observed_{};
    std::array<bool, 2> properties_known_{};
    std::uint64_t calibration_id_ = 0, query_id_ = 0;
    vsg::ref_ptr<vsg::Device> device_;
    PFN_vkGetPastPresentationTimingEXT get_past_ = nullptr;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT get_domains_ = nullptr;
    PFN_vkGetSwapchainTimingPropertiesEXT get_properties_ = nullptr;
    PFN_vkGetCalibratedTimestampsKHR calibrate_ = nullptr;
    std::array<std::uint64_t, 2> calibrate(std::size_t view, BenchmarkRun &run);
    void check_swapchains() const;
};
} // namespace orbelle::desktop
