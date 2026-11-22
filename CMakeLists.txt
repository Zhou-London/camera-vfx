cmake_minimum_required(VERSION 3.26)
project(CameraVFX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(cameravfxBinDir "${PROJECT_BINARY_DIR}/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${cameravfxBinDir}")
if(MSVC)
  add_compile_options(/W3 /permissive- /utf-8 /MP)
endif()

set(CAMERAVFX_VFX_RUNTIME_DIR "${PROJECT_SOURCE_DIR}/third_party/nvvfx_runtime" CACHE PATH
    "NVIDIA Video Effects 1.3 runtime (NVVideoEffects.dll, nvVFX*.dll, nvngx_*.dll), copied next to CameraVFX.exe as nvvfx_runtime")

set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ftxui EXCLUDE_FROM_ALL)

add_library(cameravfx_core STATIC
  src/app.cpp
  src/app_settings.cpp
  src/app_status.cpp
  src/config.cpp
  src/selftest.cpp
  src/audio/audio_monitor.cpp
  src/capture/frame_diff.cpp
  src/capture/frame_source.cpp
  src/capture/mf_capture.cpp
  src/capture/synthetic_source.cpp
  src/capture/timecode.cpp
  src/gfx/blit.cpp
  src/gfx/color_convert.cpp
  src/gfx/device.cpp
  src/gfx/gpu_profiler.cpp
  src/pacing/cadence.cpp
  src/pacing/clocks.cpp
  src/pacing/lead.cpp
  src/pacing/multiplier.cpp
  src/pacing/pacer.cpp
  src/present/present_monitor.cpp
  src/present/presenter.cpp
  src/ui/dashboard.cpp
  src/util/common.cpp
  src/vfx/vfx_chain.cpp
  src/vfx/vfx_quality.cpp
  third_party/nvvfx/src/nvVideoEffectsProxy.cpp
  third_party/nvvfx/src/nvCVImageProxy.cpp
)
target_include_directories(cameravfx_core PUBLIC src third_party/nvvfx/include)
target_compile_definitions(cameravfx_core PUBLIC _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(cameravfx_core PUBLIC
  ftxui::component ftxui::dom ftxui::screen
  d3d11 dxgi d3dcompiler mfplat mf mfreadwrite mfuuid ole32 shlwapi winmm user32 avrt cfgmgr32)

add_executable(CameraVFX src/main.cpp)
target_link_libraries(CameraVFX PRIVATE cameravfx_core)

get_property(multiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(builtConfigs ${CMAKE_BUILD_TYPE})
if(multiConfig)
  set(builtConfigs ${CMAKE_CONFIGURATION_TYPES})
endif()
foreach(config IN LISTS builtConfigs)
  if(NOT EXISTS "${PROJECT_BINARY_DIR}/${config}/CameraVFX.ini")
    file(COPY "${PROJECT_SOURCE_DIR}/CameraVFX.ini" DESTINATION "${PROJECT_BINARY_DIR}/${config}")
  endif()
endforeach()

file(GLOB_RECURSE vfxRuntimeFiles CONFIGURE_DEPENDS "${CAMERAVFX_VFX_RUNTIME_DIR}/*")
if(EXISTS "${CAMERAVFX_VFX_RUNTIME_DIR}/NVVideoEffects.dll")
  set(vfxRuntimeCopy "${cameravfxBinDir}/nvvfx_runtime")
  add_custom_command(
    OUTPUT "${vfxRuntimeCopy}/copied.stamp"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CAMERAVFX_VFX_RUNTIME_DIR}" "${vfxRuntimeCopy}"
    COMMAND ${CMAKE_COMMAND} -E touch "${vfxRuntimeCopy}/copied.stamp"
    DEPENDS ${vfxRuntimeFiles}
    COMMENT "Copying the VFX runtime next to CameraVFX.exe"
    VERBATIM)
  add_custom_target(vfx_runtime ALL DEPENDS "${vfxRuntimeCopy}/copied.stamp")
else()
  message(WARNING
    "NVIDIA VFX runtime not found in ${CAMERAVFX_VFX_RUNTIME_DIR}.\n"
    "CameraVFX builds, but frame generation, super resolution and TrueHDR cannot start and install fails.\n"
    "Fetch it with:  powershell -ExecutionPolicy Bypass -File tools/get-vfx-runtime.ps1\n"
    "or point -DCAMERAVFX_VFX_RUNTIME_DIR=<dir> at an existing copy; the next build picks it up.")
endif()

install(TARGETS CameraVFX RUNTIME DESTINATION .)
install(FILES CameraVFX.ini DESTINATION .)
install(DIRECTORY "${CAMERAVFX_VFX_RUNTIME_DIR}/" DESTINATION nvvfx_runtime)

enable_testing()
foreach(test pacing_test startup_test repeat_test log_test)
  add_executable(${test} tests/${test}.cpp)
  target_link_libraries(${test} PRIVATE cameravfx_core)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

foreach(tool tui_demo audio_test)
  add_executable(${tool} tests/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE cameravfx_core)
endforeach()
