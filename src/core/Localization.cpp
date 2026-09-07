#include "core/Localization.hpp"

#include <Windows.h>

#include <array>
#include <cwchar>

namespace codextex {
namespace {

struct Translation {
    std::string_view english;
    std::string_view japanese;
    std::string_view korean;
};

constexpr std::array kTranslations{
    Translation{"3D Viewport", "3Dビューポート", "3D 뷰포트"},
    Translation{"Main Viewport", "メインビューポート", "메인 뷰포트"},
    Translation{"Projection", "プロジェクション", "프로젝션"},
    Translation{"Projection Tools", "プロジェクションツール", "프로젝션 도구"},
    Translation{"Texture Preview", "テクスチャプレビュー", "텍스처 미리보기"},
    Translation{"Session Temp", "セッション一時ファイル", "세션 임시 파일"},
    Translation{"File", "ファイル", "파일"},
    Translation{"Edit", "編集", "편집"},
    Translation{"Settings", "設定", "설정"},
    Translation{"Language", "言語", "언어"},
    Translation{"Open OBJ...", "OBJを開く...", "OBJ 열기..."},
    Translation{"Open Texture PNG...", "テクスチャPNGを開く...", "텍스처 PNG 열기..."},
    Translation{"Add Reference OBJ + PNG...", "参照OBJ + PNGを追加...", "참고 OBJ + PNG 추가..."},
    Translation{"Save Texture", "テクスチャを保存", "텍스처 저장"},
    Translation{"Save Texture As...", "テクスチャを別名で保存...", "텍스처 다른 이름으로 저장..."},
    Translation{"Exit", "終了", "종료"},
    Translation{"Undo Texture", "テクスチャを元に戻す", "텍스처 실행 취소"},
    Translation{"Redo Texture", "テクスチャをやり直す", "텍스처 다시 실행"},
    Translation{"Original texture", "元のテクスチャ", "원본 텍스처"},
    Translation{"Generating", "生成中", "생성 중"},
    Translation{"Checking Codex App Server.", "Codex App Serverを確認しています。", "Codex App Server를 확인하고 있습니다."},
    Translation{"Temporary file deletion queued.", "一時ファイルの削除を予約しました。", "임시 파일 삭제를 요청했습니다."},
    Translation{"Could not queue temporary file deletion.", "一時ファイルの削除を予約できませんでした。", "임시 파일 삭제를 요청하지 못했습니다."},
    Translation{"ImageGen was canceled.", "ImageGenをキャンセルしました。", "ImageGen을 취소했습니다."},
    Translation{"Could not start ImageGen.", "ImageGenを開始できませんでした。", "ImageGen을 시작하지 못했습니다."},
    Translation{"Could not open the Codex ImageGen diagnostic log.", "Codex ImageGenの診断ログを開けませんでした。", "Codex ImageGen 진단 로그를 열지 못했습니다."},
    Translation{"Working", "作業", "작업"},
    Translation{"Original", "元画像", "원본"},
    Translation{"Generated Image", "生成画像", "생성 이미지"},
    Translation{"ImageGen 1:1 crop", "ImageGen 1:1 クロップ", "ImageGen 1:1 크롭"},
    Translation{"Locked projection crop", "固定プロジェクションクロップ", "고정 프로젝션 크롭"},
    Translation{"Open OBJ", "OBJを開く", "OBJ 열기"},
    Translation{"Open Texture PNG", "テクスチャPNGを開く", "텍스처 PNG 열기"},
    Translation{"Load last files", "前回のファイルを読み込む", "마지막 파일 불러오기"},
    Translation{"Triangles: %zu", "三角形: %zu", "삼각형: %zu"},
    Translation{"Texture: %s (%ux%u)", "テクスチャ: %s (%ux%u)", "텍스처: %s (%ux%u)"},
    Translation{"Locked projection source", "固定プロジェクションソース", "고정 프로젝션 원본"},
    Translation{"Read-only snapshot for Projection %llu", "プロジェクション %llu の読み取り専用スナップショット", "프로젝션 %llu 읽기 전용 스냅샷"},
    Translation{"Offline capture: %u x %u", "オフラインキャプチャ: %u x %u", "오프라인 캡처: %u x %u"},
    Translation{"Frozen hidden faces: %zu", "固定された非表示面: %zu", "고정된 숨김 면: %zu"},
    Translation{"OBJ and Base Color loading is available only in Main Viewport.", "OBJとベースカラーの読み込みはメインビューポートでのみ可能です。", "OBJ와 베이스 컬러는 메인 뷰포트에서만 불러올 수 있습니다."},
    Translation{"Warning: %zu overlapping UV pair(s)", "警告: 重複UVペア %zu 件", "경고: 겹치는 UV 쌍 %zu개"},
    Translation{"Shared or mirrored UVs may let the opposite local-X side overwrite the bake.", "共有またはミラーUVでは反対側のローカルX面がベイクを上書きする場合があります。", "공유 또는 미러 UV에서는 반대쪽 로컬 X 면이 베이크를 덮어쓸 수 있습니다."},
    Translation{"ImageGen reference sets", "ImageGen参照セット", "ImageGen 참고 세트"},
    Translation{"Add reference OBJ + PNG", "参照OBJ + PNGを追加", "참고 OBJ + PNG 추가"},
    Translation{"Show in viewport", "ビューポートに表示", "뷰포트에 표시"},
    Translation{"Show in this projection tab", "このプロジェクションタブに表示", "이 프로젝션 탭에 표시"},
    Translation{"Reference sets are viewport/ImageGen context only. Toggle them off manually while projection painting if desired.", "参照セットはビューポート/ImageGenのコンテキスト専用です。必要ならプロジェクション描画中に手動で非表示にできます。", "참고 세트는 뷰포트/ImageGen 문맥 전용입니다. 필요하면 프로젝션 페인팅 중 수동으로 숨길 수 있습니다."},
    Translation{"Remove", "削除", "제거"},
    Translation{"Clear all references", "すべての参照を消去", "모든 참고 제거"},
    Translation{"Viewport display", "ビューポート表示", "뷰포트 표시"},
    Translation{"Neutral shading", "ニュートラルシェーディング", "중립 셰이딩"},
    Translation{"Fit primary view (F)", "メインビューに合わせる (F)", "메인 뷰 맞춤 (F)"},
    Translation{"Background color", "背景色", "배경색"},
    Translation{"Shading is off by default; Base Color is shown unchanged.", "シェーディングは既定でオフです。ベースカラーをそのまま表示します。", "셰이딩은 기본적으로 꺼져 있으며 베이스 컬러를 그대로 표시합니다."},
    Translation{"Main viewport mode", "メインビューポートモード", "메인 뷰포트 모드"},
    Translation{"Navigate", "ナビゲート", "탐색"},
    Translation{"Faces", "面", "면"},
    Translation{"Lasso", "投げ縄", "올가미"},
    Translation{"Selected faces: %zu", "選択した面: %zu", "선택한 면: %zu"},
    Translation{"Hide selected", "選択面を隠す", "선택 면 숨기기"},
    Translation{"Undo hide", "非表示を元に戻す", "숨김 실행 취소"},
    Translation{"Show all", "すべて表示", "모두 표시"},
    Translation{"Create projection tab", "プロジェクションタブを作成", "프로젝션 탭 만들기"},
    Translation{"Generate captures the cyan square immediately, then opens an independent locked painting tab. The main viewport remains usable.", "生成すると水色の正方形を即座にキャプチャし、独立した固定ペイントタブを開きます。メインビューポートは引き続き使用できます。", "생성을 누르면 청록색 정사각형을 즉시 캡처하고 독립된 고정 페인팅 탭을 엽니다. 메인 뷰포트는 계속 사용할 수 있습니다."},
    Translation{"ImageGen prompt", "ImageGenプロンプト", "ImageGen 프롬프트"},
    Translation{"History", "履歴", "히스토리"},
    Translation{"Prompt history", "プロンプト履歴", "프롬프트 히스토리"},
    Translation{"Delete", "削除", "삭제"},
    Translation{"Codex model", "Codexモデル", "Codex 모델"},
    Translation{"Reasoning effort", "推論の深さ", "추론 강도"},
    Translation{"Model settings", "モデル設定", "모델 설정"},
    Translation{"Pending: saved beside the executable immediately before generation.", "保留中: 生成直前に実行ファイルの隣へ保存します。", "대기 중: 생성 직전에 실행 파일 옆에 저장합니다."},
    Translation{"Loaded from CodexTex.settings.json.", "CodexTex.settings.jsonから読み込みました。", "CodexTex.settings.json에서 불러왔습니다."},
    Translation{"Generate from current view", "現在のビューから生成", "현재 뷰에서 생성"},
    Translation{"External PNG from current view", "現在のビューで外部PNGを使用", "현재 뷰에서 외부 PNG 사용"},
    Translation{"Projection workspace", "プロジェクションワークスペース", "프로젝션 작업 공간"},
    Translation{"Camera and visibility are locked for this tab", "このタブではカメラと表示状態が固定されています", "이 탭의 카메라와 표시 상태는 고정되어 있습니다"},
    Translation{"AI working", "AI処理中", "AI 작업 중"},
    Translation{"Cancel AI", "AIをキャンセル", "AI 취소"},
    Translation{"Open/replace external PNG", "外部PNGを開く/置換", "외부 PNG 열기/교체"},
    Translation{"Projection: %s", "プロジェクション: %s", "프로젝션: %s"},
    Translation{"Mask and bake", "マスクとベイク", "마스크와 베이크"},
    Translation{"Wheel zooms the locked view. Middle-drag pans it; Shift+middle-drag shifts the generated image.", "ホイールで固定ビューを拡大縮小します。中ドラッグで表示を移動し、Shift+中ドラッグで生成画像を移動します。", "휠로 고정 뷰를 확대·축소합니다. 가운데 버튼 드래그로 화면을 이동하고 Shift+가운데 버튼 드래그로 생성 이미지를 이동합니다."},
    Translation{"Brush radius", "ブラシ半径", "브러시 반경"},
    Translation{"Lasso includes area", "投げ縄の内側を含める", "올가미 내부 포함"},
    Translation{"Inward feather", "内側フェザー", "안쪽 페더"},
    Translation{"Projection shift", "プロジェクション移動", "프로젝션 이동"},
    Translation{"Reset shift", "移動をリセット", "이동 초기화"},
    Translation{"Clear mask", "マスクを消去", "마스크 지우기"},
    Translation{"Select all visible", "表示面をすべて選択", "보이는 영역 모두 선택"},
    Translation{"Max surface angle", "最大サーフェス角度", "최대 표면 각도"},
    Translation{"Mirrored UV side", "ミラーUV側", "미러 UV 방향"},
    Translation{"Paint both local-X sides", "ローカルXの両側をペイント", "로컬 X 양쪽 페인팅"},
    Translation{"Ignore local -X side", "ローカル-X側を除外", "로컬 -X 방향 제외"},
    Translation{"Ignore local +X side", "ローカル+X側を除外", "로컬 +X 방향 제외"},
    Translation{"Shared texture changed since this tab was created; bake uses the latest texture.", "このタブの作成後に共有テクスチャが変更されました。ベイクには最新版を使用します。", "이 탭을 만든 뒤 공유 텍스처가 변경되었습니다. 베이크에는 최신 텍스처를 사용합니다."},
    Translation{"Already baked", "ベイク済み", "베이크 완료"},
    Translation{"Bake into shared texture", "共有テクスチャにベイク", "공유 텍스처에 베이크"},
    Translation{"Close tab", "タブを閉じる", "탭 닫기"},
    Translation{"ImageGen log: %s", "ImageGenログ: %s", "ImageGen 로그: %s"},
    Translation{"Retry Codex detection", "Codex検出を再試行", "Codex 감지 다시 시도"},
    Translation{"Undo", "元に戻す", "실행 취소"},
    Translation{"No PNG texture loaded.", "PNGテクスチャが読み込まれていません。", "PNG 텍스처를 불러오지 않았습니다."},
    Translation{"Session folder: %s", "セッションフォルダー: %s", "세션 폴더: %s"},
    Translation{"Refresh", "更新", "새로 고침"},
    Translation{"Delete selected", "選択項目を削除", "선택 항목 삭제"},
    Translation{"Delete all temp files", "一時ファイルをすべて削除", "모든 임시 파일 삭제"},
    Translation{"No session temporary files.", "セッション一時ファイルはありません。", "세션 임시 파일이 없습니다."},
    Translation{"Select a file to inspect its contents.", "内容を確認するファイルを選択してください。", "내용을 확인할 파일을 선택하세요."},
    Translation{"Save the modified PNG texture before closing?", "終了前に変更したPNGテクスチャを保存しますか？", "종료하기 전에 변경된 PNG 텍스처를 저장할까요?"},
    Translation{"Delete temporary file", "一時ファイルを削除", "임시 파일 삭제"},
    Translation{"Delete all session temporary files", "セッション一時ファイルをすべて削除", "세션 임시 파일 모두 삭제"},
    Translation{"Delete this temporary file?", "この一時ファイルを削除しますか？", "이 임시 파일을 삭제할까요?"},
    Translation{"It is used by a projection workspace. That tab will be closed and its AI task cancelled.", "プロジェクションワークスペースで使用中です。そのタブを閉じ、AIタスクをキャンセルします。", "프로젝션 작업 공간에서 사용 중입니다. 해당 탭을 닫고 AI 작업을 취소합니다."},
    Translation{"Delete every file in this session temp folder?", "このセッション一時フォルダー内の全ファイルを削除しますか？", "이 세션 임시 폴더의 모든 파일을 삭제할까요?"},
    Translation{"All projection workspace tabs will be closed and active AI tasks cancelled.", "すべてのプロジェクションタブを閉じ、実行中のAIタスクをキャンセルします。", "모든 프로젝션 작업 탭을 닫고 진행 중인 AI 작업을 취소합니다."},
    Translation{"Open UV-mapped OBJ", "UV付きOBJを開く", "UV가 있는 OBJ 열기"},
    Translation{"Open Base Color PNG", "ベースカラーPNGを開く", "베이스 컬러 PNG 열기"},
    Translation{"Open projection PNG", "プロジェクションPNGを開く", "프로젝션 PNG 열기"},
    Translation{"Open inference reference OBJ", "推論参照OBJを開く", "추론 참고 OBJ 열기"},
    Translation{"Open texture for the reference OBJ", "参照OBJのテクスチャを開く", "참고 OBJ의 텍스처 열기"},
    Translation{"Save Base Color PNG", "ベースカラーPNGを保存", "베이스 컬러 PNG 저장"},
    Translation{"Open an OBJ and a PNG texture.", "OBJとPNGテクスチャを開いてください。", "OBJ와 PNG 텍스처를 여세요."},
    Translation{"OBJ loaded.", "OBJを読み込みました。", "OBJ를 불러왔습니다."},
    Translation{"Texture PNG loaded.", "テクスチャPNGを読み込みました。", "텍스처 PNG를 불러왔습니다."},
    Translation{"Last OBJ and texture loaded.", "前回のOBJとテクスチャを読み込みました。", "마지막 OBJ와 텍스처를 불러왔습니다."},
    Translation{"The assets loaded, but their paths could not be saved.", "アセットは読み込みましたが、そのパスを保存できませんでした。", "에셋을 불러왔지만 해당 경로를 저장하지 못했습니다."},
    Translation{"The last OBJ and texture files are no longer available.", "前回のOBJまたはテクスチャファイルは利用できません。", "마지막 OBJ 또는 텍스처 파일을 더 이상 사용할 수 없습니다."},
    Translation{"Could not save prompt history.", "プロンプト履歴を保存できませんでした。", "프롬프트 히스토리를 저장하지 못했습니다."},
    Translation{"Projection PNG must be square to match the ImageGen crop.", "ImageGenクロップに合わせるため、プロジェクションPNGは正方形である必要があります。", "ImageGen 크롭에 맞게 프로젝션 PNG는 정사각형이어야 합니다."},
    Translation{"External projection PNG loaded.", "外部プロジェクションPNGを読み込みました。", "외부 프로젝션 PNG를 불러왔습니다."},
    Translation{"All inference reference sets were removed.", "すべての推論参照セットを削除しました。", "모든 추론 참고 세트를 제거했습니다."},
    Translation{"Inference reference OBJ + PNG added. It will never be baked or saved.", "推論参照OBJ + PNGを追加しました。ベイクや保存の対象にはなりません。", "추론 참고 OBJ + PNG를 추가했습니다. 베이크하거나 저장하지 않습니다."},
    Translation{"Inference reference set removed.", "推論参照セットを削除しました。", "추론 참고 세트를 제거했습니다."},
    Translation{"Texture PNG saved. No OBJ or project file was written.", "テクスチャPNGを保存しました。OBJやプロジェクトファイルは書き込まれていません。", "텍스처 PNG를 저장했습니다. OBJ나 프로젝트 파일은 기록하지 않았습니다."},
    Translation{"Waiting for projection image.", "プロジェクション画像を待っています。", "프로젝션 이미지를 기다리는 중입니다."},
    Translation{"ImageGen request is starting.", "ImageGenリクエストを開始しています。", "ImageGen 요청을 시작하는 중입니다."},
    Translation{"Could not start ImageGen; an external PNG can still be loaded.", "ImageGenを開始できませんでした。外部PNGは引き続き読み込めます。", "ImageGen을 시작하지 못했습니다. 외부 PNG는 계속 불러올 수 있습니다."},
    Translation{"Projection workspace created; the main viewport remains available.", "プロジェクションワークスペースを作成しました。メインビューポートは引き続き使用できます。", "프로젝션 작업 공간을 만들었습니다. 메인 뷰포트는 계속 사용할 수 있습니다."},
    Translation{"Projection baked into the shared working texture.", "共有作業テクスチャにプロジェクションをベイクしました。", "공유 작업 텍스처에 프로젝션을 베이크했습니다."},
    Translation{"Projection baked into the working texture.", "作業テクスチャにプロジェクションをベイクしました。", "작업 텍스처에 프로젝션을 베이크했습니다."},
    Translation{"Texture change undone.", "テクスチャ変更を元に戻しました。", "텍스처 변경을 실행 취소했습니다."},
    Translation{"Texture change redone.", "テクスチャ変更をやり直しました。", "텍스처 변경을 다시 실행했습니다."},
    Translation{"ImageGen result must be square to match the captured crop.", "ImageGen結果はキャプチャクロップに合わせた正方形である必要があります。", "ImageGen 결과는 캡처 크롭에 맞는 정사각형이어야 합니다."},
    Translation{"ImageGen result loaded; refine the mask before baking.", "ImageGen結果を読み込みました。ベイク前にマスクを調整してください。", "ImageGen 결과를 불러왔습니다. 베이크 전에 마스크를 다듬으세요."},
    Translation{"ImageGen result archived; refine the mask before baking.", "ImageGen結果を永続保存しました。ベイク前にマスクを調整してください。", "ImageGen 결과를 영구 저장했습니다. 베이크 전에 마스크를 다듬으세요."},
    Translation{"Could not read this file.", "このファイルを読み込めませんでした。", "이 파일을 읽을 수 없습니다."},
    Translation{"Showing the first 64 KiB.", "先頭64 KiBを表示しています。", "처음 64 KiB를 표시합니다."},
    Translation{"Text/binary preview.", "テキスト/バイナリプレビュー。", "텍스트/바이너리 미리보기입니다."},
    Translation{"Selected session temporary file deleted.", "選択したセッション一時ファイルを削除しました。", "선택한 세션 임시 파일을 삭제했습니다."},
    Translation{"Some session temporary files could not be deleted.", "一部のセッション一時ファイルを削除できませんでした。", "일부 세션 임시 파일을 삭제하지 못했습니다."},
    Translation{"Codex has not been checked.", "Codexはまだ確認されていません。", "Codex 실행 여부를 아직 확인하지 않았습니다."},
    Translation{"Could not create the CodexTex session directory.", "CodexTexセッションディレクトリを作成できませんでした。", "CodexTex 세션 디렉터리를 만들 수 없습니다."},
    Translation{"No runnable Codex CLI was found on PATH, in the Codex desktop install, or in the npm user bin. AI features are disabled.", "PATH、Codexデスクトップ、npmユーザーbinに実行可能なCodex CLIが見つかりません。AI機能は無効です。", "PATH, Codex 데스크톱 설치 경로 또는 npm 사용자 bin에서 실행 가능한 Codex CLI를 찾지 못했습니다. AI 기능이 비활성화됩니다."},
    Translation{"Could not create pipes for Codex App Server.", "Codex App Server用パイプを作成できませんでした。", "Codex App Server용 파이프를 만들 수 없습니다."},
    Translation{"Codex is not signed in. AI features are disabled; external PNG projection remains available.", "Codexにサインインしていません。AI機能は無効ですが、外部PNGプロジェクションは使用できます。", "Codex에 로그인되어 있지 않습니다. AI 기능은 비활성화되지만 외부 PNG 프로젝션은 사용할 수 있습니다."},
    Translation{"The built-in imagegen skill is unavailable or disabled. External PNG projection remains available.", "組み込みimagegenスキルが利用不可または無効です。外部PNGプロジェクションは使用できます。", "내장 imagegen 스킬을 사용할 수 없거나 비활성화되었습니다. 외부 PNG 프로젝션은 사용할 수 있습니다."},
    Translation{"Codex ImageGen is ready via", "Codex ImageGenの準備ができました。実行元:", "Codex ImageGen을 사용할 수 있습니다. 실행 경로:"},
    Translation{"Codex App Server is unavailable:", "Codex App Serverを利用できません:", "Codex App Server를 사용할 수 없습니다:"},
    Translation{"Codex CLI candidates were found, but none could start App Server", "Codex CLI候補は見つかりましたが、App Serverを起動できませんでした", "Codex CLI 후보는 찾았지만 App Server를 시작하지 못했습니다"},
};

} // namespace

UiLanguage DetectSystemUiLanguage() noexcept {
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> locale{};
    if (GetUserDefaultLocaleName(locale.data(), static_cast<int>(locale.size())) == 0) {
        return UiLanguage::English;
    }
    if (_wcsnicmp(locale.data(), L"ko", 2) == 0) return UiLanguage::Korean;
    if (_wcsnicmp(locale.data(), L"ja", 2) == 0) return UiLanguage::Japanese;
    return UiLanguage::English;
}

UiLanguage ResolveUiLanguage(const std::string_view savedCode, const bool loadedFromDisk,
                             const UiLanguage detectedLanguage) noexcept {
    if (!loadedFromDisk || savedCode.empty()) return detectedLanguage;
    return ParseUiLanguage(savedCode).value_or(UiLanguage::English);
}

std::optional<UiLanguage> ParseUiLanguage(const std::string_view code) noexcept {
    if (code == "en") return UiLanguage::English;
    if (code == "ja") return UiLanguage::Japanese;
    if (code == "ko") return UiLanguage::Korean;
    return std::nullopt;
}

std::string_view UiLanguageCode(const UiLanguage language) noexcept {
    switch (language) {
    case UiLanguage::Japanese: return "ja";
    case UiLanguage::Korean: return "ko";
    default: return "en";
    }
}

std::string_view NativeLanguageName(const UiLanguage language) noexcept {
    switch (language) {
    case UiLanguage::Japanese: return "日本語";
    case UiLanguage::Korean: return "한국어";
    default: return "English";
    }
}

std::string_view Translate(const UiLanguage language, const std::string_view english) noexcept {
    if (language == UiLanguage::English) return english;
    for (const auto& translation : kTranslations) {
        if (translation.english != english) continue;
        return language == UiLanguage::Japanese ? translation.japanese : translation.korean;
    }
    return english;
}

} // namespace codextex
