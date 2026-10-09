#include "benchmark_presentation.hpp"
#include "benchmark_run.hpp"
#include <QJsonArray>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
constexpr std::uint32_t feedback_capacity = 512;
constexpr VkPresentStageFlagsEXT default_stages_requested =
    VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
void require_success(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
} // namespace
QJsonObject present_timing_capabilities(vsg::Window &window) {
    const auto instance = window.getOrCreateInstance();
    const auto physical = window.getOrCreatePhysicalDevice();
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical->vk(), nullptr, &count, nullptr) !=
        VK_SUCCESS)
        throw std::runtime_error("Cannot enumerate presentation timing extensions");
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical->vk(), nullptr, &count, extensions.data()) !=
        VK_SUCCESS)
        throw std::runtime_error("Cannot read presentation timing extensions");
    const auto has = [&](const char *name) {
        return std::any_of(extensions.begin(), extensions.end(), [&](const auto &extension) {
            return std::strcmp(name, extension.extensionName) == 0;
        });
    };
    QJsonObject result{{"gpu", QString::fromUtf8(physical->getProperties().deviceName)}};
    const bool supported = has(VK_EXT_PRESENT_TIMING_EXTENSION_NAME) &&
                           has(VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
                           has(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    result.insert("required_device_extensions", supported);
    if (!supported)
        return result;
    VkPhysicalDevicePresentTimingFeaturesEXT timing{};
    timing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
    VkPhysicalDevicePresentId2FeaturesKHR ids{};
    ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    ids.pNext = &timing;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &ids;
    vkGetPhysicalDeviceFeatures2(physical->vk(), &features);
    result.insert("present_id2_feature", ids.presentId2 != VK_FALSE);
    result.insert("present_timing_feature", timing.presentTiming != VK_FALSE);
    VkPresentTimingSurfaceCapabilitiesEXT stages{};
    stages.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    VkSurfaceCapabilitiesPresentId2KHR id_caps{};
    id_caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
    id_caps.pNext = &stages;
    VkSurfaceCapabilities2KHR caps{};
    caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    caps.pNext = &id_caps;
    VkPhysicalDeviceSurfaceInfo2KHR info{};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    info.surface = window.getOrCreateSurface()->vk();
    const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
        vkGetInstanceProcAddr(instance->vk(), "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    if (!query || query(physical->vk(), &info, &caps) != VK_SUCCESS)
        throw std::runtime_error("Cannot query this window's presentation timing capabilities");
    result.insert("present_id2_surface", id_caps.presentId2Supported != VK_FALSE);
    result.insert("present_timing_surface", stages.presentTimingSupported != VK_FALSE);
    result.insert("present_stage_queries", static_cast<int>(stages.presentStageQueries));
    result.insert("queue_operations_end",
                  bool(stages.presentStageQueries & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT));
    result.insert("request_dequeued",
                  bool(stages.presentStageQueries & VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT));
    result.insert("first_pixel_out", bool(stages.presentStageQueries &
                                          VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT));
    result.insert("first_pixel_visible", bool(stages.presentStageQueries &
                                              VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT));
    return result;
}

void prepare_present_timing(vsg::WindowTraits &traits) {
    traits.instanceExtensionNames.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    traits.deviceExtensionNames.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    traits.deviceExtensionNames.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    traits.deviceExtensionNames.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    traits.deviceFeatures
        ->get<VkPhysicalDevicePresentId2FeaturesKHR,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR>()
        .presentId2 = VK_TRUE;
    traits.deviceFeatures
        ->get<VkPhysicalDevicePresentTimingFeaturesEXT,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT>()
        .presentTiming = VK_TRUE;
    traits.swapchainPreferences.flags |=
        VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
}

BenchmarkPresentation::BenchmarkPresentation(std::array<vsg::ref_ptr<vsg::Window>, 2> windows,
                                             bool output_only_probe)
    : windows_(std::move(windows)),
      stages_requested_(output_only_probe ? VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
                                          : default_stages_requested),
      device_(windows_[0]->getOrCreateDevice()) {
    get_past_ = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(
        vkGetDeviceProcAddr(device_->vk(), "vkGetPastPresentationTimingEXT"));
    get_domains_ = reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
        vkGetDeviceProcAddr(device_->vk(), "vkGetSwapchainTimeDomainPropertiesEXT"));
    get_properties_ = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(
        vkGetDeviceProcAddr(device_->vk(), "vkGetSwapchainTimingPropertiesEXT"));
    const auto set_size = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(
        vkGetDeviceProcAddr(device_->vk(), "vkSetSwapchainPresentTimingQueueSizeEXT"));
    calibrate_ = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
        vkGetDeviceProcAddr(device_->vk(), "vkGetCalibratedTimestampsKHR"));
    if (!get_past_ || !get_domains_ || !get_properties_ || !set_size || !calibrate_)
        throw std::runtime_error("Required presentation timing entry points are unavailable");
    const auto clock_query = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
        vkGetInstanceProcAddr(windows_[0]->getOrCreateInstance()->vk(),
                              "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
    std::uint32_t clock_count = 0;
    if (!clock_query)
        throw std::runtime_error("Cannot enumerate calibratable clocks");
    require_success(clock_query(windows_[0]->getPhysicalDevice()->vk(), &clock_count, nullptr),
                    "Cannot enumerate calibratable clocks");
    std::vector<VkTimeDomainKHR> clocks(clock_count);
    require_success(
        clock_query(windows_[0]->getPhysicalDevice()->vk(), &clock_count, clocks.data()),
        "Cannot read calibratable clocks");
    if (std::find(clocks.begin(), clocks.end(), VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR) ==
        clocks.end())
        throw std::runtime_error("The device cannot calibrate to QPC");
    for (std::size_t view = 0; view < windows_.size(); ++view) {
        if (windows_[view]->getOrCreateDevice() != device_)
            throw std::runtime_error("Timing windows must use the same device");
        swapchains_[view] = windows_[view]->getOrCreateSwapchain()->vk();
        require_success(set_size(device_->vk(), swapchains_[view], feedback_capacity),
                        "Cannot allocate present timing feedback queue");
        VkSwapchainTimeDomainPropertiesEXT properties{};
        properties.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
        require_success(
            get_domains_(device_->vk(), swapchains_[view], &properties, &domain_counters_[view]),
            "Cannot enumerate swapchain clocks");
        if (!properties.timeDomainCount)
            throw std::runtime_error("No swapchain clock domain is available");
        std::vector<VkTimeDomainKHR> domains(properties.timeDomainCount);
        std::vector<std::uint64_t> ids(properties.timeDomainCount);
        properties.pTimeDomains = domains.data();
        properties.pTimeDomainIds = ids.data();
        require_success(
            get_domains_(device_->vk(), swapchains_[view], &properties, &domain_counters_[view]),
            "Cannot read swapchain clocks");
        bool found = false;
        for (const auto preferred :
             {VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR, VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT,
              VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT}) {
            for (std::size_t index = 0; index < properties.timeDomainCount; ++index) {
                if (domains[index] == preferred) {
                    domain_ids_[view] = ids[index];
                    domains_[view] = preferred;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        if (!found)
            throw std::runtime_error("No supported swapchain clock domain");
        if (std::find(clocks.begin(), clocks.end(), domains_[view]) == clocks.end())
            throw std::runtime_error("The selected swapchain clock cannot be calibrated");
    }
}

void BenchmarkPresentation::check_swapchains() const {
    for (std::size_t view = 0; view < windows_.size(); ++view)
        if (windows_[view]->getOrCreateSwapchain()->vk() != swapchains_[view])
            throw std::runtime_error("A timing swapchain was recreated; the run is invalid");
}

std::array<std::uint64_t, 2> BenchmarkPresentation::calibrate(std::size_t view, BenchmarkRun &run) {
    if (domains_[view] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR)
        return {};
    const bool per_stage = domains_[view] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
    const std::uint32_t qpc_index = per_stage ? 2u : 1u;
    const std::array<VkPresentStageFlagsEXT, 2> stage_names{
        VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT,
        VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT};
    std::array<VkSwapchainCalibratedTimestampInfoEXT, 2> swapchains{};
    std::array<VkCalibratedTimestampInfoKHR, 3> infos{};
    for (std::uint32_t index = 0; index < qpc_index; ++index) {
        swapchains[index].sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT;
        swapchains[index].swapchain = swapchains_[view];
        swapchains[index].presentStage = stage_names[index];
        swapchains[index].timeDomainId = domain_ids_[view];
        infos[index].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
        infos[index].timeDomain = domains_[view];
        infos[index].pNext = &swapchains[index];
    }
    infos[qpc_index].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[qpc_index].timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
    // A single call calibrates both stage clocks against the same QPC sample,
    // with one deviation bound covering all three timestamps. Stage-local
    // domains may repeat; a swapchain-local domain is queried only once.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::array<std::uint64_t, 3> times{};
        std::uint64_t deviation = 0;
        const auto before = BenchmarkRun::qpc();
        const auto result =
            calibrate_(device_->vk(), qpc_index + 1, infos.data(), times.data(), &deviation);
        const auto after = BenchmarkRun::qpc();
        require_success(result, "Cannot calibrate presentation clock to QPC");
        std::array<std::uint64_t, 2> ids{};
        for (std::size_t stage = 0; stage < 2; ++stage) {
            ids[stage] = ++calibration_id_;
            run.calibration({ids[stage], domain_ids_[view], before, after,
                             times[per_stage ? stage : 0], times[qpc_index], deviation,
                             static_cast<int>(view), 1, static_cast<int>(domains_[view]),
                             static_cast<int>(stage_names[stage])});
        }
        if (times[0] && times[1] && times[qpc_index] && deviation > 0 && deviation <= 1'000'000)
            return ids;
    }
    throw std::runtime_error("No usable presentation clock calibration after 8 attempts");
}

void BenchmarkPresentation::poll(BenchmarkRun &run) {
    check_swapchains();
    for (std::size_t view = 0; view < windows_.size(); ++view) {
        std::array<std::array<VkPresentStageTimeEXT, BenchmarkRun::feedback_stage_capacity>,
                   feedback_capacity>
            stages{};
        std::array<VkPastPresentationTimingEXT, feedback_capacity> records{};
        for (std::size_t index = 0; index < records.size(); ++index) {
            records[index].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
            records[index].presentStageCount = BenchmarkRun::feedback_stage_capacity;
            records[index].pPresentStages = stages[index].data();
        }
        VkPastPresentationTimingInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
        info.swapchain = swapchains_[view];
        VkPastPresentationTimingPropertiesEXT properties{};
        properties.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
        properties.presentationTimingCount = feedback_capacity;
        properties.pPresentationTimings = records.data();
        const auto before = BenchmarkRun::qpc();
        // Check before each view as well as each Qt tick. A preceding query or
        // calibration may finish after the deadline; never start another poll.
        if (run.drain_deadline_qpc() && before >= run.drain_deadline_qpc())
            return;
        const auto query_id = ++query_id_;
        const auto result = get_past_(device_->vk(), &info, &properties);
        const auto queried = BenchmarkRun::qpc();
        const bool output_valid = result == VK_SUCCESS || result == VK_INCOMPLETE;
        run.feedback_query(
            {query_id, before, queried, output_valid ? properties.timeDomainsCounter : 0,
             output_valid ? properties.timingPropertiesCounter : 0, feedback_capacity,
             output_valid ? properties.presentationTimingCount : 0, info.flags,
             static_cast<int>(view), 1, result, output_valid});
        if (!output_valid)
            require_success(result, "Cannot read presentation feedback");
        if (properties.presentationTimingCount > records.size())
            throw std::runtime_error("Presentation feedback exceeded its record buffer capacity");
        // Complete reports have already been consumed by the driver. Save the entire
        // batch before any calibration/property query or validation can throw.
        for (std::size_t index = 0; index < properties.presentationTimingCount; ++index) {
            const auto &record = records[index];
            BenchmarkRun::Feedback value{record.presentId,
                                         queried,
                                         0,
                                         0,
                                         record.timeDomainId,
                                         static_cast<int>(view),
                                         1,
                                         static_cast<int>(record.timeDomain),
                                         record.reportComplete};
            value.query_id = query_id;
            value.record_index = static_cast<std::uint32_t>(index);
            value.stage_count = record.presentStageCount;
            value.target_time = record.targetTime;
            for (std::size_t stage = 0;
                 stage < std::min(record.presentStageCount, BenchmarkRun::feedback_stage_capacity);
                 ++stage) {
                const auto &time = stages[index][stage];
                value.stages[stage] = {time.stage, time.time};
                if (time.stage == VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT)
                    value.queue_end_ns = time.time;
                if (time.stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)
                    value.first_pixel_out_ns = time.time;
            }
            run.feedback(value);
        }
        bool valid_records = true;
        for (std::size_t index = 0; index < properties.presentationTimingCount; ++index) {
            const auto &record = records[index];
            const bool known = pending_[view].contains(record.presentId);
            if (record.reportComplete)
                pending_[view].erase(record.presentId); // Includes complete reports with time=0.
            VkPresentStageFlagsEXT seen = 0;
            for (std::size_t stage = 0;
                 stage < std::min(record.presentStageCount, BenchmarkRun::feedback_stage_capacity);
                 ++stage) {
                const auto name = stages[index][stage].stage;
                valid_records &= (name == VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT ||
                                  name == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) &&
                                 !(seen & name);
                seen |= name;
            }
            const auto count = stages_requested_ == default_stages_requested ? 2u : 1u;
            valid_records &= known && record.reportComplete == VK_TRUE && record.targetTime == 0 &&
                             record.presentStageCount == count && seen == stages_requested_ &&
                             record.timeDomain == domains_[view] &&
                             record.timeDomainId == domain_ids_[view];
        }
        if (!valid_records)
            throw std::runtime_error(
                "Invalid presentation identity, stages, completeness, or clock domain");
        if (properties.timeDomainsCounter != domain_counters_[view])
            throw std::runtime_error("Swapchain clock domains changed; run invalid");
        if (properties.timingPropertiesCounter < last_properties_observed_[view])
            throw std::runtime_error("Swapchain timing properties counter moved backwards");
        last_properties_observed_[view] = properties.timingPropertiesCounter;
        if (!properties_known_[view] ||
            properties.timingPropertiesCounter != properties_counters_[view]) {
            VkSwapchainTimingPropertiesEXT refresh{};
            refresh.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
            std::uint64_t counter = 0;
            const auto begin = BenchmarkRun::qpc();
            const auto code = get_properties_(device_->vk(), swapchains_[view], &refresh, &counter);
            const auto end = BenchmarkRun::qpc();
            const bool available = code == VK_SUCCESS;
            run.timing_properties({query_id, begin, end, available ? counter : 0,
                                   available ? refresh.refreshDuration : 0,
                                   available ? refresh.refreshInterval : 0, static_cast<int>(view),
                                   1, code, available});
            if (code != VK_NOT_READY)
                require_success(code, "Cannot read swapchain timing properties");
            if (available) {
                if (counter < last_properties_observed_[view])
                    throw std::runtime_error("Swapchain timing properties counter moved backwards");
                last_properties_observed_[view] = counter;
                properties_counters_[view] = counter;
                properties_known_[view] = true;
            }
        }
        if (properties.presentationTimingCount)
            run.associate_calibrations(query_id, calibrate(view, run));
    }
}

std::size_t BenchmarkPresentation::pending_count() const {
    return pending_[0].size() + pending_[1].size();
}

VkResult BenchmarkPresentation::present(vsg::Presentation &presentation, std::uint64_t frame,
                                        BenchmarkRun &run) {
    check_swapchains();
    if (presentation.windows.size() != windows_.size())
        throw std::runtime_error("Timing requires the original two-window presentation batch");
    std::vector<VkSemaphore> semaphores;
    for (const auto &semaphore : presentation.waitSemaphores)
        semaphores.push_back(semaphore->vk());
    std::array<std::uint32_t, 2> indices{};
    std::array<std::uint64_t, 2> ids{frame + 1, frame + 1};
    std::array<VkResult, 2> results{};
    std::array<VkPresentTimingInfoEXT, 2> timing{};
    // Our explicit view order is used consistently for every per-swapchain array.
    for (std::size_t view = 0; view < windows_.size(); ++view) {
        const auto &window = windows_[view];
        if (!window->visible() || window->imageIndex() >= window->numFrames() ||
            std::find(presentation.windows.begin(), presentation.windows.end(), window) ==
                presentation.windows.end())
            throw std::runtime_error("A benchmark view has no acquired presentation image");
        indices[view] = static_cast<std::uint32_t>(window->imageIndex());
        semaphores.push_back(window->frame(indices[view]).renderFinishedSemaphore->vk());
        timing[view].sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
        timing[view].timeDomainId = domain_ids_[view];
        timing[view].presentStageQueries = stages_requested_;
        // targetTime remains zero: observe presentation without scheduling it.
    }
    VkPresentTimingsInfoEXT timings{};
    timings.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
    timings.swapchainCount = 2;
    timings.pTimingInfos = timing.data();
    VkPresentId2KHR identification{};
    identification.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
    identification.pNext = &timings;
    identification.swapchainCount = 2;
    identification.pPresentIds = ids.data();
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.pNext = &identification;
    info.waitSemaphoreCount = static_cast<std::uint32_t>(semaphores.size());
    info.pWaitSemaphores = semaphores.data();
    info.swapchainCount = 2;
    info.pSwapchains = swapchains_.data();
    info.pImageIndices = indices.data();
    info.pResults = results.data();
    const auto begin = BenchmarkRun::qpc();
    const auto result = presentation.queue->present(info);
    const auto end = BenchmarkRun::qpc();
    for (int view = 0; view < 2; ++view) {
        run.present({frame, begin, end, frame + 1, view, 1, results[view]});
        if (results[view] == VK_SUCCESS)
            pending_[view].insert(frame + 1);
    }
    // Even QUEUE_FULL consumes the semaphore wait and acquired image. Never retry.
    require_success(result, "Timed presentation failed (not retried)");
    for (const auto code : results)
        require_success(code, "A timed swapchain presentation failed");
    return result;
}

QJsonObject BenchmarkPresentation::metadata() const {
    QJsonArray views;
    for (int view = 0; view < 2; ++view) {
        QJsonArray pending;
        for (const auto id : pending_[view])
            pending.append(static_cast<qint64>(id));
        views.append(QJsonObject{
            {"view", view},
            {"generation", 1},
            {"present_mode",
             static_cast<int>(windows_[view]->traits()->swapchainPreferences.presentMode)},
            {"image_count",
             static_cast<int>(windows_[view]->traits()->swapchainPreferences.imageCount)},
            {"time_domain", static_cast<int>(domains_[view])},
            {"time_domain_id", static_cast<qint64>(domain_ids_[view])},
            {"time_domains_counter", static_cast<qint64>(domain_counters_[view])},
            {"pending_present_ids", pending}});
    }
    return {{"source", "VK_EXT_present_timing revision 3"},
            {"stage_mask", static_cast<int>(stages_requested_)},
            {"output_only_probe", stages_requested_ != default_stages_requested},
            {"feedback_capacity_per_view", static_cast<int>(feedback_capacity)},
            {"feedback_stage_capacity", static_cast<int>(BenchmarkRun::feedback_stage_capacity)},
            {"feedback_query_flags", 0},
            {"stage_time_unit", "nanoseconds"},
            {"views", views}};
}
} // namespace orbitforge::desktop
