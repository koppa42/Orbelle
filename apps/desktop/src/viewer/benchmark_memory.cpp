#include "benchmark_memory.hpp"
#include <QByteArray>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vsg/vk/PhysicalDevice.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

namespace orbelle::desktop {
struct BenchmarkMemory::Impl {
    QJsonObject metadata;
#ifdef _WIN32
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    UINT node = 0;
#endif
};
#ifdef _WIN32
namespace {
void check(HRESULT value, const char *operation) {
    if (FAILED(value))
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(value));
}
QJsonObject segment(IDXGIAdapter3 &adapter, UINT node, DXGI_MEMORY_SEGMENT_GROUP group) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    check(adapter.QueryVideoMemoryInfo(node, group, &info), "QueryVideoMemoryInfo failed");
    return {{"current_usage_bytes", static_cast<qint64>(info.CurrentUsage)},
            {"budget_bytes", static_cast<qint64>(info.Budget)},
            {"current_reservation_bytes", static_cast<qint64>(info.CurrentReservation)},
            {"available_for_reservation_bytes", static_cast<qint64>(info.AvailableForReservation)}};
}
} // namespace
#endif
BenchmarkMemory::BenchmarkMemory(const vsg::PhysicalDevice &physical)
    : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    const auto id = physical.getProperties<VkPhysicalDeviceIDProperties,
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES>();
    if (!id.deviceLUIDValid || !std::has_single_bit(id.deviceNodeMask))
        throw std::runtime_error("Vulkan did not provide a valid Windows adapter LUID/node");
    LUID luid{};
    static_assert(sizeof(luid) == VK_LUID_SIZE);
    std::memcpy(&luid, id.deviceLUID, sizeof(luid));
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2 failed");
    check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&impl_->adapter)),
          "Cannot match DXGI adapter to Vulkan LUID");
    DXGI_ADAPTER_DESC1 desc{};
    check(impl_->adapter->GetDesc1(&desc), "Cannot describe DXGI adapter");
    if (std::memcmp(&desc.AdapterLuid, &luid, sizeof(luid)) != 0)
        throw std::runtime_error("DXGI returned a different adapter LUID");
    impl_->node = static_cast<UINT>(std::countr_zero(id.deviceNodeMask));
    impl_->metadata = {
        {"source", "IDXGIAdapter3::QueryVideoMemoryInfo"},
        {"scope",
         "current process, matched Vulkan adapter and node; local/non-local OS memory "
         "segment usage and process budget; not device-global VRAM or Vulkan allocation totals"},
        {"adapter_match", "Vulkan deviceLUID == DXGI AdapterLuid"},
        {"adapter_luid_hex",
         QString::fromLatin1(
             QByteArray(reinterpret_cast<const char *>(id.deviceLUID), VK_LUID_SIZE).toHex())},
        {"device_uuid_hex",
         QString::fromLatin1(
             QByteArray(reinterpret_cast<const char *>(id.deviceUUID), VK_UUID_SIZE).toHex())},
        {"node_mask", static_cast<qint64>(id.deviceNodeMask)},
        {"node_index", static_cast<int>(impl_->node)},
        {"adapter_description", QString::fromWCharArray(desc.Description)},
        {"vendor_id", static_cast<qint64>(desc.VendorId)},
        {"device_id", static_cast<qint64>(desc.DeviceId)},
        {"dedicated_video_memory_bytes", static_cast<qint64>(desc.DedicatedVideoMemory)},
        {"shared_system_memory_bytes", static_cast<qint64>(desc.SharedSystemMemory)},
        {"sample_interval_seconds", 1}};
    (void)sample(); // Check both segments before declaring the run ready.
#else
    (void)physical;
    throw std::runtime_error("Process video memory timing currently requires Windows DXGI");
#endif
}
BenchmarkMemory::~BenchmarkMemory() = default;
QJsonObject BenchmarkMemory::sample() const {
#ifdef _WIN32
    return {{"local", segment(*impl_->adapter.Get(), impl_->node, DXGI_MEMORY_SEGMENT_GROUP_LOCAL)},
            {"non_local",
             segment(*impl_->adapter.Get(), impl_->node, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)}};
#else
    throw std::runtime_error("Process video memory timing currently requires Windows DXGI");
#endif
}
QJsonObject BenchmarkMemory::metadata() const { return impl_->metadata; }
} // namespace orbelle::desktop
