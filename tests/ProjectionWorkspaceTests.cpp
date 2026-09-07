#include "app/ProjectionWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <utility>
#include <vector>

namespace {

codextex::ProjectionWorkspace Workspace(const std::uint64_t id) {
    codextex::ProjectionWorkspace workspace;
    workspace.id = id;
    workspace.prompt = "Prompt " + std::to_string(id);
    workspace.temporaryDirectory = "projection-" + std::to_string(id);
    workspace.capturePath = workspace.temporaryDirectory / "capture.png";
    workspace.hiddenFaces = {0, 1};
    workspace.camera.yaw = static_cast<float>(id);
    return workspace;
}

} // namespace

TEST_CASE("Projection workspace lookup keeps concurrent jobs independent") {
    codextex::ProjectionWorkspaces workspaces;
    workspaces.Add(Workspace(11));
    workspaces.Add(Workspace(22));
    REQUIRE(workspaces.Find(11));
    workspaces.Find(11)->projectionLoaded = true;
    workspaces.Find(11)->status = "Generated";
    const auto& readOnly = workspaces;
    REQUIRE(readOnly.Find(22));
    CHECK_FALSE(readOnly.Find(22)->projectionLoaded);
    CHECK(readOnly.Find(22)->status == "Waiting for projection image.");
    CHECK(readOnly.Find(22)->camera.yaw == 22.0f);
    CHECK(readOnly.Find(22)->capturePath == std::filesystem::path("projection-22/capture.png"));
    CHECK(readOnly.Find(99) == nullptr);
}

TEST_CASE("Closing a projection runs cancellation and cleanup before removing its state") {
    codextex::ProjectionWorkspaces workspaces;
    workspaces.Add(Workspace(11));
    workspaces.Add(Workspace(22));
    std::vector<std::string> actions;
    REQUIRE(workspaces.Erase(11, [&](auto& workspace) {
        REQUIRE(workspaces.Find(11) == &workspace);
        CHECK(workspaces.size() == 2);
        CHECK(workspace.capturePath == std::filesystem::path("projection-11/capture.png"));
        actions.push_back("cancel " + std::to_string(workspace.id));
        actions.push_back("cleanup " + workspace.temporaryDirectory.string());
    }));
    CHECK(actions == std::vector<std::string>{"cancel 11", "cleanup projection-11"});
    CHECK(workspaces.Find(11) == nullptr);
    REQUIRE(workspaces.Find(22));
    CHECK(workspaces.Find(22)->prompt == "Prompt 22");
    CHECK(workspaces.size() == 1);
    CHECK_FALSE(workspaces.Erase(11, [&](auto&) { FAIL("Removed jobs must not be canceled twice"); }));
}

TEST_CASE("Clearing projections calls every lifecycle hook exactly once before releasing state") {
    codextex::ProjectionWorkspaces workspaces;
    workspaces.Add(Workspace(11));
    workspaces.Add(Workspace(22));
    std::vector<std::uint64_t> canceled;
    workspaces.EraseAll([&](auto& workspace) {
        CHECK(workspaces.Find(workspace.id) == &workspace);
        canceled.push_back(workspace.id);
    });
    CHECK(canceled == std::vector<std::uint64_t>{11, 22});
    CHECK(workspaces.empty());
    CHECK(workspaces.Find(11) == nullptr);
    CHECK(workspaces.Find(22) == nullptr);
    workspaces.EraseAll([&](auto&) { FAIL("An empty collection has no remaining jobs"); });
}

TEST_CASE("Projection lifecycle hooks retain recovery information until removal") {
    codextex::ProjectionWorkspaces workspaces;
    auto failed = Workspace(11);
    failed.temporaryCleanupBlocked = true;
    failed.statusIsError = true;
    workspaces.Add(std::move(failed));
    std::filesystem::path recoveredFrom;
    REQUIRE(workspaces.Erase(11, [&](auto& workspace) {
        REQUIRE(workspace.temporaryCleanupBlocked);
        REQUIRE(workspace.statusIsError);
        recoveredFrom = workspace.temporaryDirectory;
    }));
    CHECK(recoveredFrom == std::filesystem::path("projection-11"));
    CHECK(workspaces.empty());
}

TEST_CASE("A failed projection removal hook leaves the workspace available for retry") {
    codextex::ProjectionWorkspaces workspaces;
    workspaces.Add(Workspace(11));
    CHECK_THROWS_AS(workspaces.Erase(11, [](auto&) {
        throw std::runtime_error("Cancellation failed");
    }), std::runtime_error);
    REQUIRE(workspaces.Find(11));
    CHECK(workspaces.Find(11)->capturePath == std::filesystem::path("projection-11/capture.png"));
    REQUIRE(workspaces.Erase(11, [](auto&) {}));
    CHECK(workspaces.empty());
}

TEST_CASE("Projection workspace IDs cannot route multiple tabs to the same job") {
    codextex::ProjectionWorkspaces workspaces;
    workspaces.Add(Workspace(11));
    auto duplicate = Workspace(11);
    duplicate.prompt = "Duplicate";
    CHECK_THROWS_AS(workspaces.Add(std::move(duplicate)), std::invalid_argument);
    CHECK(workspaces.size() == 1);
    REQUIRE(workspaces.Find(11));
    CHECK(workspaces.Find(11)->prompt == "Prompt 11");
}

TEST_CASE("Archive recovery survives closing tabs until explicit session cleanup") {
    codextex::ProjectionWorkspaces workspaces;
    workspaces.Add(Workspace(11));
    workspaces.Add(Workspace(22));
    CHECK_FALSE(workspaces.HasRecoveryFiles());
    workspaces.Find(11)->temporaryCleanupBlocked = true;
    CHECK(workspaces.HasRecoveryFiles());
    REQUIRE(workspaces.Erase(11, [](auto&) {}));
    CHECK(workspaces.HasRecoveryFiles());
    workspaces.EraseAll([](auto&) {});
    CHECK(workspaces.empty());
    CHECK(workspaces.HasRecoveryFiles());
    workspaces.ForgetRecoveryFiles();
    CHECK_FALSE(workspaces.HasRecoveryFiles());
}

TEST_CASE("Clearing all projection tabs retains failed archive recovery directories") {
    codextex::ProjectionWorkspaces workspaces;
    workspaces.Add(Workspace(11));
    workspaces.Add(Workspace(22));
    workspaces.Find(22)->temporaryCleanupBlocked = true;
    std::vector<std::uint64_t> closed;
    workspaces.EraseAll([&](auto& workspace) {
        closed.push_back(workspace.id);
    });
    CHECK(closed == std::vector<std::uint64_t>{11, 22});
    REQUIRE(workspaces.empty());
    CHECK(workspaces.HasRecoveryFiles());

    // Repeating tab cleanup must not discard recovery from already closed tabs.
    workspaces.EraseAll([](auto&) {});
    CHECK(workspaces.HasRecoveryFiles());
    workspaces.ForgetRecoveryFiles();
    CHECK_FALSE(workspaces.HasRecoveryFiles());
}
