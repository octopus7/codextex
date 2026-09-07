#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace codextex {

class TextureImage {
public:
    bool LoadPng(const std::filesystem::path& path, std::string& error);
    bool LoadEncoded(std::span<const std::uint8_t> bytes, std::string& error,
                      std::size_t maxDecodedBytes = 256ull * 1024 * 1024);
    bool SavePng(const std::filesystem::path& path, std::string& error) const;
    void Assign(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);
    [[nodiscard]] TextureImage CenterCroppedSquare() const;

    [[nodiscard]] std::uint32_t Width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t Height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<std::uint8_t>& Pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::vector<std::uint8_t>& Pixels() noexcept { return pixels_; }
    [[nodiscard]] bool Empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] const std::filesystem::path& SourcePath() const noexcept { return sourcePath_; }

private:
    std::filesystem::path sourcePath_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<std::uint8_t> pixels_;
};

} // namespace codextex
