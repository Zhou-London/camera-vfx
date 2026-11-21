# NVOF interpolator quality / timing test (GPU required).
add_executable(nvof_test
  tests/nvof_test.cpp
  src/interp/nvof_interp.cpp
  src/interp/blend_interp.cpp
  src/gfx/device.cpp
  src/util/common.cpp
)
target_include_directories(nvof_test PRIVATE src third_party/nvof)
target_link_libraries(nvof_test PRIVATE d3d11 dxgi d3dcompiler)
