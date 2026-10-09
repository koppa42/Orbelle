#pragma once
#include <QJsonObject>
#include <memory>

namespace vsg {
class PhysicalDevice;
}
namespace orbitforge::desktop {
// Queries this process on the exact Vulkan adapter/node. Does not reserve memory
// or change the OS budget. The native adapter is independent of Vulkan lifetime.
class BenchmarkMemory {
  public:
    explicit BenchmarkMemory(const vsg::PhysicalDevice &physical);
    ~BenchmarkMemory();
    [[nodiscard]] QJsonObject sample() const;
    [[nodiscard]] QJsonObject metadata() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbitforge::desktop
