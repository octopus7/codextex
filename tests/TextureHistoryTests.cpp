#include "core/TextureHistory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

codextex::TextureImage Image(const std::uint8_t value, const std::uint32_t width = 1) {
    const std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * 4, value);
    codextex::TextureImage image;
    image.Assign(width, 1, pixels);
    return image;
}

auto UploadTo(codextex::TextureImage& working) {
    return [&working](const codextex::TextureImage& image, std::string&) {
        working = image;
        return true;
    };
}

bool Change(codextex::TextureHistory& history, codextex::TextureImage& working,
            const std::uint8_t value, std::string& error) {
    return history.ApplyChange(working, [&](std::string&) {
        working = Image(value);
        return true;
    }, error);
}

} // namespace

TEST_CASE("Texture undo upload failure preserves both histories and saved state") {
    codextex::TextureHistory history;
    auto working = Image(10);
    std::string error;
    REQUIRE(Change(history, working, 20, error));
    history.MarkSaved();
    const auto bytes = history.MemoryBytes();
    int calls = 0;
    CHECK_FALSE(history.Undo(working, [&](const auto& candidate, std::string& failure) {
        ++calls;
        CHECK(candidate.Pixels()[0] == 10);
        CHECK(history.UndoCount() == 1);
        CHECK(history.RedoCount() == 0);
        CHECK_FALSE(history.IsDirty());
        failure = "GPU upload failed";
        return false;
    }, error));
    CHECK(calls == 1);
    CHECK(error == "GPU upload failed");
    CHECK(working.Pixels()[0] == 20);
    CHECK(history.UndoCount() == 1);
    CHECK(history.RedoCount() == 0);
    CHECK(history.MemoryBytes() == bytes);
    CHECK_FALSE(history.IsDirty());
    REQUIRE(history.Undo(working, UploadTo(working), error));
    CHECK(working.Pixels()[0] == 10);
    CHECK(history.IsDirty());
}

TEST_CASE("Texture redo upload failure preserves the candidate for retry") {
    codextex::TextureHistory history;
    auto working = Image(10);
    std::string error;
    REQUIRE(Change(history, working, 20, error));
    REQUIRE(history.Undo(working, UploadTo(working), error));
    const auto bytes = history.MemoryBytes();
    CHECK_FALSE(history.Redo(working, [&](const auto& candidate, std::string& failure) {
        CHECK(candidate.Pixels()[0] == 20);
        CHECK(history.UndoCount() == 0);
        CHECK(history.RedoCount() == 1);
        CHECK_FALSE(history.IsDirty());
        failure = "GPU upload failed";
        return false;
    }, error));
    CHECK(error == "GPU upload failed");
    CHECK(working.Pixels()[0] == 10);
    CHECK(history.UndoCount() == 0);
    CHECK(history.RedoCount() == 1);
    CHECK(history.MemoryBytes() == bytes);
    CHECK_FALSE(history.IsDirty());
    REQUIRE(history.Redo(working, UploadTo(working), error));
    CHECK(working.Pixels()[0] == 20);
    CHECK(history.IsDirty());
}

TEST_CASE("Failed texture changes keep redo history and dirty state unchanged") {
    codextex::TextureHistory history;
    auto working = Image(10);
    std::string error;
    REQUIRE(Change(history, working, 20, error));
    REQUIRE(history.Undo(working, UploadTo(working), error));
    const auto bytes = history.MemoryBytes();
    CHECK_FALSE(history.ApplyChange(working, [&](std::string& failure) {
        CHECK(history.CanRedo());
        CHECK_FALSE(history.CanUndo());
        failure = "Bake failed";
        return false;
    }, error));
    CHECK(error == "Bake failed");
    CHECK(history.CanRedo());
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.IsDirty());
    CHECK(history.MemoryBytes() == bytes);
    REQUIRE(history.Redo(working, UploadTo(working), error));
    CHECK(working.Pixels()[0] == 20);
}

TEST_CASE("Texture history tracks savepoints across undo redo and divergent edits") {
    codextex::TextureHistory history;
    auto working = Image(10);
    std::string error;
    REQUIRE(Change(history, working, 20, error));
    history.MarkSaved();
    REQUIRE(Change(history, working, 30, error));
    CHECK(history.IsDirty());
    REQUIRE(history.Undo(working, UploadTo(working), error));
    CHECK_FALSE(history.IsDirty());
    REQUIRE(history.Undo(working, UploadTo(working), error));
    CHECK(history.IsDirty());
    REQUIRE(history.Redo(working, UploadTo(working), error));
    CHECK_FALSE(history.IsDirty());
    REQUIRE(history.Redo(working, UploadTo(working), error));
    CHECK(history.IsDirty());
    REQUIRE(history.Undo(working, UploadTo(working), error));
    REQUIRE(history.Undo(working, UploadTo(working), error));
    REQUIRE(Change(history, working, 40, error));
    CHECK(history.IsDirty());
    CHECK_FALSE(history.CanRedo());
    REQUIRE(history.Undo(working, UploadTo(working), error));
    REQUIRE(history.Redo(working, UploadTo(working), error));
    CHECK(history.IsDirty());
    history.Clear();
    CHECK_FALSE(history.IsDirty());
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.CanRedo());
    CHECK(history.MemoryBytes() == 0);
}

TEST_CASE("Texture history retains the nearest eight changes by default") {
    codextex::TextureHistory history;
    auto working = Image(0);
    std::string error;
    for (std::uint8_t value = 1; value <= 10; ++value) {
        REQUIRE(Change(history, working, value, error));
    }
    CHECK(history.UndoCount() == 8);
    for (std::uint8_t expected = 9; expected >= 2; --expected) {
        REQUIRE(history.Undo(working, UploadTo(working), error));
        CHECK(working.Pixels()[0] == expected);
    }
    CHECK_FALSE(history.CanUndo());
    CHECK(history.RedoCount() == 8);
    CHECK(history.IsDirty());
}

TEST_CASE("Texture history bounds combined undo and redo pixel storage") {
    const auto bytesPerImage = Image(0).Pixels().capacity();
    codextex::TextureHistory history(8, 2 * bytesPerImage);
    auto working = Image(0);
    std::string error;
    for (std::uint8_t value = 1; value <= 3; ++value) {
        REQUIRE(Change(history, working, value, error));
    }
    CHECK(history.UndoCount() == 2);
    CHECK(history.MemoryBytes() == 2 * bytesPerImage);
    REQUIRE(history.Undo(working, UploadTo(working), error));
    CHECK(working.Pixels()[0] == 2);
    CHECK(history.UndoCount() == 1);
    CHECK(history.RedoCount() == 1);
    CHECK(history.MemoryBytes() == 2 * bytesPerImage);
    REQUIRE(history.Undo(working, UploadTo(working), error));
    CHECK(working.Pixels()[0] == 1);
    CHECK_FALSE(history.CanUndo());
    REQUIRE(history.Redo(working, UploadTo(working), error));
    REQUIRE(history.Redo(working, UploadTo(working), error));
    CHECK(working.Pixels()[0] == 3);
    CHECK(history.MemoryBytes() == 2 * bytesPerImage);
}

TEST_CASE("An oversized texture snapshot is discarded while the edit remains dirty") {
    const auto smallBytes = Image(0).Pixels().capacity();
    codextex::TextureHistory history(8, smallBytes);
    history.RecordChange(Image(0));
    REQUIRE(history.CanUndo());
    auto oversized = Image(20, 4);
    std::string error;
    REQUIRE(history.ApplyChange(oversized, [](std::string&) { return true; }, error));
    CHECK(history.IsDirty());
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.CanRedo());
    CHECK(history.MemoryBytes() == 0);
    history.MarkSaved();
    CHECK_FALSE(history.IsDirty());
}

TEST_CASE("Texture edits can commit with history retention disabled") {
    codextex::TextureHistory history(0, 0);
    auto working = Image(10);
    std::string error;
    REQUIRE(Change(history, working, 20, error));
    CHECK(working.Pixels()[0] == 20);
    CHECK(history.IsDirty());
    CHECK_FALSE(history.CanUndo());
    CHECK(history.MemoryBytes() == 0);
    int calls = 0;
    const auto upload = [&](const auto&, std::string&) { ++calls; return true; };
    CHECK_FALSE(history.Undo(working, upload, error));
    CHECK_FALSE(history.Redo(working, upload, error));
    CHECK(calls == 0);
}
