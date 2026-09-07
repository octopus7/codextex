#pragma once

#include "core/TextureImage.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <string>

namespace codextex {

class TextureHistory {
public:
    using ApplyTexture = std::function<bool(const TextureImage&, std::string&)>;
    using ApplyOperation = std::function<bool(std::string&)>;

    explicit TextureHistory(std::size_t maxEntries = 8,
                            std::size_t maxBytes = 256u * 1024u * 1024u);

    void Clear() noexcept;
    void MarkSaved() noexcept;
    [[nodiscard]] bool IsDirty() const noexcept;
    [[nodiscard]] bool CanUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool CanRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t UndoCount() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t RedoCount() const noexcept { return redo_.size(); }
    // Combined allocated RGBA storage in both histories, excluding the live texture.
    [[nodiscard]] std::size_t MemoryBytes() const noexcept { return memoryBytes_; }

    // The operation must return true only after the working texture is committed.
    // All history allocation happens first; failure leaves history and savepoint intact.
    bool ApplyChange(TextureImage before, const ApplyOperation& apply, std::string& error);
    // For an already committed operation; prefer ApplyChange when allocation can fail.
    void RecordChange(TextureImage before);
    bool Undo(TextureImage current, const ApplyTexture& upload, std::string& error);
    bool Redo(TextureImage current, const ApplyTexture& upload, std::string& error);

private:
    struct Entry {
        TextureImage image;
        std::uint64_t revision{};
    };
    using Entries = std::list<Entry>;

    bool Restore(Entries& from, Entries& to, TextureImage current,
                 const ApplyTexture& upload, std::string& error);
    void CommitChange(Entries& pending) noexcept;
    void Trim() noexcept;
    static std::size_t Bytes(const Entry& entry) noexcept;

    Entries undo_;
    Entries redo_;
    std::size_t maxEntries_;
    std::size_t maxBytes_;
    std::size_t memoryBytes_{};
    std::uint64_t currentRevision_{};
    std::uint64_t savedRevision_{};
    std::uint64_t nextRevision_{1};
};

} // namespace codextex
