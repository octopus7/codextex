cmake_minimum_required(VERSION 3.28)
project(CodexTex VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "CodexTex is Windows-only")
endif()

option(CODEXTEX_BUILD_TESTS "Build CodexTex tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(directxtex CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)

add_library(codextex_core
  src/core/AppSettings.cpp
  src/core/GenerationArchive.cpp
  src/core/Localization.cpp
  src/core/Mesh.cpp
  src/core/TextureImage.cpp
  src/core/TextureHistory.cpp
  src/core/Mask.cpp
  src/core/PromptBuilder.cpp
)
target_include_directories(codextex_core PUBLIC src)
target_compile_definitions(codextex_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(codextex_core
  PUBLIC
    Microsoft::DirectXTex
    nlohmann_json::nlohmann_json
    tinyobjloader::tinyobjloader
)

add_executable(CodexTex WIN32
  src/main.cpp
  src/app/Application.cpp
  src/graphics/Renderer.cpp
  src/codex/CodexBridge.cpp
  src/codex/AsyncCodexClient.cpp
)
target_include_directories(CodexTex PRIVATE src)
target_compile_definitions(CodexTex PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(CodexTex PRIVATE
  codextex_core
  imgui::imgui
  d3d11 dxgi d3dcompiler windowscodecs comdlg32 shell32 ole32
)
add_custom_command(TARGET CodexTex POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:Microsoft::DirectXTex>"
    "$<TARGET_FILE_DIR:CodexTex>"
)
set_property(TARGET CodexTex PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(MSVC)
  target_compile_options(codextex_core PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(CodexTex PRIVATE /W4 /permissive- /utf-8)
endif()

if(CODEXTEX_BUILD_TESTS)
  include(CTest)
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(codextex_mock_codex tests/MockCodex.cpp)
  target_link_libraries(codextex_mock_codex PRIVATE nlohmann_json::nlohmann_json)
  set_target_properties(codextex_mock_codex PROPERTIES
    OUTPUT_NAME codex
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mock-codex"
  )
  add_executable(codextex_tests
    src/codex/CodexBridge.cpp
    src/codex/AsyncCodexClient.cpp
    src/graphics/Renderer.cpp
    tests/AppSettingsTests.cpp
    tests/GenerationArchiveTests.cpp
    tests/CodexBridgeTests.cpp
    tests/AsyncCodexClientTests.cpp
    tests/MeshTests.cpp
    tests/MaskTests.cpp
    tests/LocalizationTests.cpp
    tests/PromptBuilderTests.cpp
    tests/ProjectionViewTransformTests.cpp
    tests/RendererTests.cpp
    tests/TextureImageTests.cpp
    tests/TextureHistoryTests.cpp
  )
  target_link_libraries(codextex_tests PRIVATE
    codextex_core Catch2::Catch2WithMain d3d11 dxgi d3dcompiler
  )
  target_compile_definitions(codextex_tests PRIVATE
    CODEXTEX_MOCK_CODEX_PATH="$<TARGET_FILE:codextex_mock_codex>"
  )
  add_dependencies(codextex_tests codextex_mock_codex)
  add_custom_command(TARGET codextex_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:Microsoft::DirectXTex>"
      "$<TARGET_FILE_DIR:codextex_tests>"
  )
  if(MSVC)
    target_compile_options(codextex_tests PRIVATE /W4 /permissive- /utf-8)
  endif()
  include(Catch)
  catch_discover_tests(codextex_tests)
endif()
