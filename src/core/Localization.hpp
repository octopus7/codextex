#pragma once

#include <optional>
#include <string_view>

namespace codextex {

enum class UiLanguage {
    English,
    Japanese,
    Korean,
};

UiLanguage DetectSystemUiLanguage() noexcept;
UiLanguage ResolveUiLanguage(std::string_view savedCode, bool loadedFromDisk,
                             UiLanguage detectedLanguage) noexcept;
std::optional<UiLanguage> ParseUiLanguage(std::string_view code) noexcept;
std::string_view UiLanguageCode(UiLanguage language) noexcept;
std::string_view NativeLanguageName(UiLanguage language) noexcept;
std::string_view Translate(UiLanguage language, std::string_view english) noexcept;

} // namespace codextex
