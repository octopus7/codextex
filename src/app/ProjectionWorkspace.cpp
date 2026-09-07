#include "app/ProjectionWorkspace.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace codextex {

ProjectionWorkspace& ProjectionWorkspaces::Add(ProjectionWorkspace workspace) {
    if (Find(workspace.id)) {
        throw std::invalid_argument("Projection workspace IDs must be unique.");
    }
    workspaces_.push_back(std::move(workspace));
    return workspaces_.back();
}

ProjectionWorkspace* ProjectionWorkspaces::Find(const std::uint64_t id) noexcept {
    const auto found = std::ranges::find(workspaces_, id, &ProjectionWorkspace::id);
    return found == workspaces_.end() ? nullptr : &*found;
}

const ProjectionWorkspace* ProjectionWorkspaces::Find(const std::uint64_t id) const noexcept {
    const auto found = std::ranges::find(workspaces_, id, &ProjectionWorkspace::id);
    return found == workspaces_.end() ? nullptr : &*found;
}

bool ProjectionWorkspaces::Erase(const std::uint64_t id, const BeforeRemove& beforeRemove) {
    const auto found = std::ranges::find(workspaces_, id, &ProjectionWorkspace::id);
    if (found == workspaces_.end()) return false;
    beforeRemove(*found);
    RememberRecoveryFiles(*found);
    workspaces_.erase(found);
    return true;
}

void ProjectionWorkspaces::EraseAll(const BeforeRemove& beforeRemove) {
    for (auto& workspace : workspaces_) {
        beforeRemove(workspace);
        RememberRecoveryFiles(workspace);
    }
    workspaces_.clear();
}

void ProjectionWorkspaces::RememberRecoveryFiles(const ProjectionWorkspace& workspace) {
    if (workspace.temporaryCleanupBlocked && !workspace.temporaryDirectory.empty() &&
        std::ranges::find(recoveryDirectories_, workspace.temporaryDirectory) == recoveryDirectories_.end()) {
        recoveryDirectories_.push_back(workspace.temporaryDirectory);
    }
}

bool ProjectionWorkspaces::HasRecoveryFiles() const noexcept {
    return !recoveryDirectories_.empty() || std::ranges::any_of(workspaces_, [](const auto& workspace) {
        return workspace.temporaryCleanupBlocked && !workspace.temporaryDirectory.empty();
    });
}

} // namespace codextex
