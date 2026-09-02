#include "core/Localization.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("UI language codes accept only supported languages") {
    using codextex::UiLanguage;
    CHECK(codextex::ParseUiLanguage("en") == UiLanguage::English);
    CHECK(codextex::ParseUiLanguage("ja") == UiLanguage::Japanese);
    CHECK(codextex::ParseUiLanguage("ko") == UiLanguage::Korean);
    CHECK_FALSE(codextex::ParseUiLanguage("fr").has_value());
    CHECK_FALSE(codextex::ParseUiLanguage("").has_value());
}

TEST_CASE("Language menu names remain native and generated image is localized") {
    using codextex::UiLanguage;
    CHECK(codextex::NativeLanguageName(UiLanguage::English) == "English");
    CHECK(codextex::NativeLanguageName(UiLanguage::Japanese) == "日本語");
    CHECK(codextex::NativeLanguageName(UiLanguage::Korean) == "한국어");
    CHECK(codextex::Translate(UiLanguage::English, "Generated Image") == "Generated Image");
    CHECK(codextex::Translate(UiLanguage::Japanese, "Generated Image") == "生成画像");
    CHECK(codextex::Translate(UiLanguage::Korean, "Generated Image") == "생성 이미지");
}

TEST_CASE("Saved language wins while unsupported saved values fall back to English") {
    using codextex::UiLanguage;
    CHECK(codextex::ResolveUiLanguage({}, false, UiLanguage::Korean) == UiLanguage::Korean);
    CHECK(codextex::ResolveUiLanguage({}, true, UiLanguage::Japanese) == UiLanguage::Japanese);
    CHECK(codextex::ResolveUiLanguage("ko", true, UiLanguage::Japanese) == UiLanguage::Korean);
    CHECK(codextex::ResolveUiLanguage("fr", true, UiLanguage::Korean) == UiLanguage::English);
}
