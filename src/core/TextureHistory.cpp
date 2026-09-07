#include "core/TextureHistory.hpp"

#include <new>
#include <optional>
#include <utility>

namespace codextex {

TextureHistory::TextureHistory(const std::size_t maxEntries, const std::size_t maxBytes)
    : maxEntries_(maxEntries), maxBytes_(maxBytes) {}

void TextureHistory::Clear() noexcept {
    undo_.clear();
    redo_.clear();
    memoryBytes_ = 0;
    currentRevision_ = 0;
    savedRevision_ = 0;
    nextRevision_ = 1;
}

void TextureHistory::MarkSaved() noexcept {
    savedRevision_ = currentRevision_;
}

bool TextureHistory::IsDirty() const noexcept {
    return currentRevision_ != savedRevision_;
}

std::size_t TextureHistory::Bytes(const Entry& entry) noexcept {
    return entry.image.Pixels().capacity();
}

bool TextureHistory::ApplyChange(TextureImage before, const ApplyOperation& apply,
                                 std::string& error) {
    std::optional<Entries> pending;
    try {
        pending.emplace();
        pending->push_back({std::move(before), currentRevision_});
    } catch (const std::bad_alloc&) {
        error = "Could not allocate texture undo history.";
        return false;
    }
    if (!apply(error)) return false;
    CommitChange(*pending);
    error.clear();
    return true;
}

void TextureHistory::RecordChange(TextureImage before) {
    Entries pending;
    pending.push_back({std::move(before), currentRevision_});
    CommitChange(pending);
}

void TextureHistory::CommitChange(Entries& pending) noexcept {
    for (const auto& entry : redo_) memoryBytes_ -= Bytes(entry);
    redo_.clear();
    memoryBytes_ += Bytes(pending.back());
    undo_.splice(undo_.end(), pending);
    currentRevision_ = nextRevision_++;
    Trim();
}

bool TextureHistory::Undo(TextureImage current, const ApplyTexture& upload, std::string& error) {
    if (undo_.empty()) {
        error = "There is no texture change to undo.";
        return false;
    }
    return Restore(undo_, redo_, std::move(current), upload, error);
}

bool TextureHistory::Redo(TextureImage current, const ApplyTexture& upload, std::string& error) {
    if (redo_.empty()) {
        error = "There is no texture change to redo.";
        return false;
    }
    return Restore(redo_, undo_, std::move(current), upload, error);
}

bool TextureHistory::Restore(Entries& from, Entries& to, TextureImage current,
                             const ApplyTexture& upload, std::string& error) {
    std::optional<Entries> pending;
    try {
        pending.emplace();
        pending->push_back({std::move(current), currentRevision_});
    } catch (const std::bad_alloc&) {
        error = "Could not allocate texture undo history.";
        return false;
    }

    const Entry& candidate = from.back();
    if (!upload(candidate.image, error)) return false;

    // Splicing an allocated node cannot allocate after the GPU upload has succeeded.
    currentRevision_ = candidate.revision;
    memoryBytes_ -= Bytes(candidate);
    from.pop_back();
    memoryBytes_ += Bytes(pending->back());
    to.splice(to.end(), *pending);
    Trim();
    error.clear();
    return true;
}

void TextureHistory::Trim() noexcept {
    while (undo_.size() + redo_.size() > maxEntries_ || memoryBytes_ > maxBytes_) {
        // Each front is the furthest state on its side of the current revision.
        Entries& oldest = undo_.size() >= redo_.size() ? undo_ : redo_;
        memoryBytes_ -= Bytes(oldest.front());
        oldest.pop_front();
    }
}

} // namespace codextex
