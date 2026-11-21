#if defined(linux) || defined(unix) || defined(__linux)
#warning nvVideoEffectsProxy.cpp not ported
#else // _WIN32_
/*###############################################################################
#
# Copyright (c) 2020 NVIDIA Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
###############################################################################*/
#include <string>

#include "nvVideoEffects.h"

#ifdef _WIN32
  #define _WINSOCKAPI_
  #include <windows.h>
#else // !_WIN32
  #include <dlfcn.h>
  typedef void* HMODULE;
  typedef void* HANDLE;
  typedef void* HINSTANCE;
#endif // _WIN32


inline void* nvGetProcAddress(HINSTANCE handle, const char* proc) {
  if (nullptr == handle) return nullptr;
#ifdef _WIN32
  return GetProcAddress(handle, proc);
#else // !_WIN32
  return dlsym(handle, proc);
#endif // _WIN32
}

inline int nvFreeLibrary(HINSTANCE handle) {
#ifdef _WIN32
  return FreeLibrary(handle);
#else // !_WIN32
  return dlclose(handle);
#endif // _WIN32
}

static std::wstring Utf8ToWide(const char* utf8_str) {
  if (!utf8_str || !utf8_str[0]) return std::wstring();
  int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_str, -1, nullptr, 0);
  if (wide_len <= 0) return std::wstring();
  std::wstring wide(wide_len, L'\0');
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_str, -1, &wide[0], wide_len);
  if (n <= 0 || n != wide_len) return std::wstring();
  wide.resize(static_cast<size_t>(wide_len - 1));
  return wide;
}

HINSTANCE getNvVfxLib() {
  bool sdk_path_set = false;

  extern char* g_nvVFXSDKPath;
  if (g_nvVFXSDKPath && g_nvVFXSDKPath[0]) {
    std::wstring wide_path = Utf8ToWide(g_nvVFXSDKPath);
    if (!wide_path.empty()) {
      SetDllDirectoryW(wide_path.c_str());
      sdk_path_set = true;
    }
  }

  if (!sdk_path_set) {
    // There can be multiple apps on the system,
    // some might include the SDK in the app package and
    // others might expect the SDK to be installed in Program Files
    DWORD len = GetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", nullptr, 0);
    std::wstring env_path;
    if (len > 0) {
      env_path.resize(len);
      GetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", &env_path[0], len);
      env_path.resize(len - 1);
    }
    if (env_path != L"USE_APP_PATH") {
      len = GetEnvironmentVariableW(L"ProgramFiles", nullptr, 0);
      std::wstring prog_files;
      if (len > 0) {
        prog_files.resize(len);
        GetEnvironmentVariableW(L"ProgramFiles", &prog_files[0], len);
        prog_files.resize(len - 1);
      }
      std::wstring full_path = prog_files + L"\\NVIDIA Corporation\\NVIDIA Video Effects\\";
      SetDllDirectoryW(full_path.c_str());
    }
  }

  static const HINSTANCE NvVfxLib = LoadLibraryW(L"NVVideoEffects.dll");
  return NvVfxLib;
}

NvCV_Status NvVFX_API NvVFX_GetVersion(unsigned int* version) {
  static const auto funcPtr = (decltype(NvVFX_GetVersion)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetVersion");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(version);
}

NvCV_Status NvVFX_API NvVFX_CreateEffect(NvVFX_EffectSelector code, NvVFX_Handle* obj) {
  static const auto funcPtr = (decltype(NvVFX_CreateEffect)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_CreateEffect");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(code, obj);
}

void NvVFX_API NvVFX_DestroyEffect(NvVFX_Handle obj) {
  static const auto funcPtr = (decltype(NvVFX_DestroyEffect)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_DestroyEffect");

  if (nullptr != funcPtr) funcPtr(obj);
}

NvCV_Status NvVFX_API NvVFX_SetU32(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, unsigned int val) {
  static const auto funcPtr = (decltype(NvVFX_SetU32)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetU32");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_SetS32(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, int val) {
  static const auto funcPtr = (decltype(NvVFX_SetS32)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetS32");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_SetF32(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, float val) {
  static const auto funcPtr = (decltype(NvVFX_SetF32)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetF32");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_SetF64(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, double val) {
  static const auto funcPtr = (decltype(NvVFX_SetF64)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetF64");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_SetU64(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, unsigned long long val) {
  static const auto funcPtr = (decltype(NvVFX_SetU64)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetU64");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_SetImage(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, NvCVImage* im) {
  static const auto funcPtr = (decltype(NvVFX_SetImage)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetImage");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, im);
}

NvCV_Status NvVFX_API NvVFX_SetObject(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, void* ptr) {
  static const auto funcPtr = (decltype(NvVFX_SetObject)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetObject");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, ptr);
}

NvCV_Status NvVFX_API NvVFX_SetStateObjectHandleArray(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, NvVFX_StateObjectHandle* handle) {
  static const auto funcPtr = (decltype(NvVFX_SetStateObjectHandleArray)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetStateObjectHandleArray");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, handle);
}

NvCV_Status NvVFX_API NvVFX_SetString(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, const char* str) {
  static const auto funcPtr = (decltype(NvVFX_SetString)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetString");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, str);
}

NvCV_Status NvVFX_API NvVFX_SetCudaStream(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, CUstream stream) {
  static const auto funcPtr = (decltype(NvVFX_SetCudaStream)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_SetCudaStream");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, stream);
}

NvCV_Status NvVFX_API NvVFX_GetU32(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, unsigned int* val) {
  static const auto funcPtr = (decltype(NvVFX_GetU32)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetU32");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_GetS32(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, int* val) {
  static const auto funcPtr = (decltype(NvVFX_GetS32)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetS32");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_GetF32(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, float* val) {
  static const auto funcPtr = (decltype(NvVFX_GetF32)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetF32");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_GetF64(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, double* val) {
  static const auto funcPtr = (decltype(NvVFX_GetF64)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetF64");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_GetU64(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, unsigned long long* val) {
  static const auto funcPtr = (decltype(NvVFX_GetU64)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetU64");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, val);
}

NvCV_Status NvVFX_API NvVFX_GetImage(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, NvCVImage* im) {
  static const auto funcPtr = (decltype(NvVFX_GetImage)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetImage");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, im);
}

NvCV_Status NvVFX_API NvVFX_GetObject(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, void** ptr) {
  static const auto funcPtr = (decltype(NvVFX_GetObject)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetObject");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, ptr);
}

NvCV_Status NvVFX_API NvVFX_GetString(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, const char** str) {
  static const auto funcPtr = (decltype(NvVFX_GetString)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetString");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, str);
}

NvCV_Status NvVFX_API NvVFX_GetCudaStream(NvVFX_Handle obj, NvVFX_ParameterSelector paramName, CUstream* stream) {
  static const auto funcPtr = (decltype(NvVFX_GetCudaStream)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_GetCudaStream");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, paramName, stream);
}

NvCV_Status NvVFX_API NvVFX_Run(NvVFX_Handle obj, int async) {
  static const auto funcPtr = (decltype(NvVFX_Run)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_Run");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, async);
}

NvCV_Status NvVFX_API NvVFX_Load(NvVFX_Handle obj) {
  static const auto funcPtr = (decltype(NvVFX_Load)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_Load");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj);
}

NvCV_Status NvVFX_API NvVFX_CudaStreamCreate(CUstream* stream) {
  static const auto funcPtr =
      (decltype(NvVFX_CudaStreamCreate)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_CudaStreamCreate");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(stream);
}

NvCV_Status NvVFX_API NvVFX_CudaStreamDestroy(CUstream stream) {
  static const auto funcPtr =
      (decltype(NvVFX_CudaStreamDestroy)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_CudaStreamDestroy");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(stream);
}

NvCV_Status NvVFX_API NvVFX_CudaStreamSynchronize(CUstream stream) {
  static const auto funcPtr =
      (decltype(NvVFX_CudaStreamSynchronize)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_CudaStreamSynchronize");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(stream);
}

NvCV_Status NvVFX_API NvVFX_ConfigureLogger(int verbosity, const char* file, void (*cb)(void*, const char*), void* cb_data) {
  static const auto funcPtr =
      (decltype(NvVFX_ConfigureLogger)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_ConfigureLogger");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(verbosity, file, cb, cb_data);
}

NvCV_Status NvVFX_API NvVFX_AllocateState(NvVFX_Handle obj, NvVFX_StateObjectHandle* handle) {
  static const auto funcPtr = (decltype(NvVFX_AllocateState)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_AllocateState");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, handle);
}

NvCV_Status NvVFX_API NvVFX_DeallocateState(NvVFX_Handle obj, NvVFX_StateObjectHandle handle) {
  static const auto funcPtr = (decltype(NvVFX_DeallocateState)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_DeallocateState");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, handle);
}

NvCV_Status NvVFX_API NvVFX_ResetState(NvVFX_Handle obj, NvVFX_StateObjectHandle handle) {
  static const auto funcPtr = (decltype(NvVFX_ResetState)*)nvGetProcAddress(getNvVfxLib(), "NvVFX_ResetState");

  if (nullptr == funcPtr) return NVCV_ERR_LIBRARY;
  return funcPtr(obj, handle);
}

#endif // enabling for this file
