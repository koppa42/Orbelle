#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <orbelle/render_vsg/compile_manager.hpp>
#include <stdexcept>
#include <vsg/app/Viewer.h>
#include <vsg/nodes/Group.h>

namespace {
using namespace std::chrono_literals;

// An empty scene uses the real VSG worker queue without requiring a GPU/window.
TEST(CompileManager, NestedExclusionBlocksWorkerAndReleasesAfterException) {
    auto viewer = vsg::Viewer::create();
    auto manager =
        orbelle::render_vsg::CompileManager::create(*viewer, vsg::ResourceHints::create());
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::future<vsg::CompileResult> worker;
    EXPECT_THROW(manager->run_exclusive([&] {
        worker = std::async(std::launch::async, [&] {
            entered.set_value();
            return manager->compile(vsg::Group::create());
        });
        entered_future.wait();
        manager->run_exclusive(
            [&] { EXPECT_EQ(worker.wait_for(30ms), std::future_status::timeout); });
        // The inner scope must not release the outer scope's exclusion.
        EXPECT_EQ(worker.wait_for(30ms), std::future_status::timeout);
        throw std::runtime_error("exercise exception cleanup");
    }),
                 std::runtime_error);
    const auto completed = worker.wait_for(2s);
    viewer->status->set(false);
    EXPECT_EQ(completed, std::future_status::ready);
    EXPECT_EQ(worker.get().result, VK_SUCCESS);
}

TEST(CompileManager, ClosingAllViewsKeepsWorkersPausedUntilShutdown) {
    auto viewer = vsg::Viewer::create();
    auto manager =
        orbelle::render_vsg::CompileManager::create(*viewer, vsg::ResourceHints::create());
    manager->run_exclusive([&] {
        manager->begin_change(*viewer);
        // The last window has gone: no contexts may be returned to workers.
        manager->end_change(*viewer);
        manager->run_exclusive([] {});
    });
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto worker = std::async(std::launch::async, [&] {
        entered.set_value();
        return manager->compile(vsg::Group::create());
    });
    entered_future.wait();
    EXPECT_EQ(worker.wait_for(30ms), std::future_status::timeout);
    viewer->status->set(false);
    EXPECT_EQ(worker.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(worker.get().result, VK_INCOMPLETE);
}
} // namespace
