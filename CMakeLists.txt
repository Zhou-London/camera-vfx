cmake_minimum_required(VERSION 3.24)
project(camss LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W3 /permissive- /utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(CAMSS_VFX_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/third_party/nvvfx_runtime" CACHE PATH
    "Directory holding the NVIDIA Video Effects 1.3 runtime DLLs (NVVideoEffects.dll, nvVFX*.dll, nvngx_*.dll)")

set(CAMSS_SOURCES
  src/app.cpp
  src/util/common.cpp
  src/gfx/device.cpp
  src/gfx/color_convert.cpp
  src/gfx/blit.cpp
  src/capture/mf_capture.cpp
  src/present/presenter.cpp
  src/pacing/clocks.cpp
  src/pacing/pacer.cpp
  src/interp/blend_interp.cpp
  src/interp/nvof_interp.cpp
  src/scale/rtx_video.cpp
  src/vfx/vfx_chain.cpp
  third_party/nvvfx/src/NVVideoEffectsProxy.cpp
  third_party/nvvfx/src/nvCVImageProxy.cpp
)

add_executable(camss ${CAMSS_SOURCES})
target_include_directories(camss PRIVATE src third_party/nvvfx/include third_party/nvof)
file(TO_CMAKE_PATH "${CAMSS_VFX_RUNTIME_DIR}" _vfxdir)
target_compile_definitions(camss PRIVATE CAMSS_VFX_RUNTIME_DIR=L"${_vfxdir}")
target_link_libraries(camss PRIVATE d3d11 dxgi d3dcompiler mfplat mf mfreadwrite mfuuid ole32 shlwapi winmm dwmapi user32)

# Small helper tools.
add_executable(mfenum tools/mfenum.cpp)

# Unit tests for the pacing core (no GPU required).
add_executable(pacing_test tests/pacing_test.cpp src/pacing/clocks.cpp src/pacing/pacer.cpp src/util/common.cpp)
target_include_directories(pacing_test PRIVATE src)

# Optional extra targets (e.g. NVOF interpolator test)
include(tests/nvof_test.cmake OPTIONAL)
