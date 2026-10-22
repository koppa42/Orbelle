#include <orbelle/render_vsg/compile_manager.hpp>
#include <stdexcept>
#include <vsg/app/Viewer.h>

namespace orbelle::render_vsg {
CompileManager::CompileManager(vsg::Viewer &viewer, vsg::ref_ptr<vsg::ResourceHints> hints)
    : Inherit(viewer, hints), hints_(std::move(hints)) {}

void CompileManager::borrow_traversals() {
    if (!borrowed_.empty())
        return;
    borrowed_ = takeCompileTraversals(numCompileTraversals);
    if (borrowed_.size() != numCompileTraversals) {
        for (auto &traversal : borrowed_)
            compileTraversals->add(traversal);
        borrowed_.clear();
        throw std::runtime_error("Cannot pause an inactive VSG compiler");
    }
}

void CompileManager::return_traversals_if_ready() {
    if (paused_ || exclusive_depth_ != 0)
        return;
    for (auto &traversal : borrowed_)
        compileTraversals->add(traversal);
    borrowed_.clear();
}

void CompileManager::run_exclusive(const std::function<void()> &operation) {
    borrow_traversals();
    ++exclusive_depth_;
    try {
        operation();
    } catch (...) {
        --exclusive_depth_;
        return_traversals_if_ready();
        throw;
    }
    --exclusive_depth_;
    return_traversals_if_ready();
}

void CompileManager::begin_change(vsg::Viewer &viewer) {
    if (paused_)
        return;
    borrow_traversals();
    paused_ = true;
    viewer.deviceWaitIdle();
    // Destroy contexts while their View and ViewDependentState still exist.
    for (auto &traversal : borrowed_)
        traversal->contexts.clear();
}

void CompileManager::end_change(vsg::Viewer &viewer) {
    if (!paused_ || viewer.recordAndSubmitTasks.empty())
        return;
    const vsg::ResourceRequirements requirements(hints_);
    for (auto &traversal : borrowed_)
        traversal->add(viewer, requirements);
    paused_ = false;
    return_traversals_if_ready();
}
} // namespace orbelle::render_vsg
