cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(HZ_BUILD_TOOLS "Build the headless render tool used by tests (Linux only)" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(MSVC)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE _USE_MATH_DEFINES)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.c src/hz-util.c src/harmonizer-filter.c src/finish-filter.c
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# 動作確認用のヘッドレス描画ツール（Linux のみ。tests/headless_test.py が使う）
if(HZ_BUILD_TOOLS AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  find_package(X11 REQUIRED)
  add_executable(hz-headless-render tools/headless-render.c)
  target_link_libraries(hz-headless-render PRIVATE OBS::libobs X11::X11)
  set_target_properties(hz-headless-render PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
