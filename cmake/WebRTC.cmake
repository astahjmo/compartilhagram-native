# Standalone Google libwebrtc, exposed through webrtc-sdk's C++ ABI wrapper.
# Pin release and checksum to keep the C++ ABI reproducible.
set(WEBRTC_ROOT "" CACHE PATH "Extracted libwebrtc SDK (contains include/ and lib/)")
option(DOWNLOAD_WEBRTC "Download the pinned Linux or Windows x64 SDK" ON)
if(WIN32)
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR
     CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]")
    message(FATAL_ERROR "Windows requires an MSVC-compatible x64 compiler and the x64 SDK.")
  endif()
  set(_webrtc_library libwebrtc.dll)
  set(_webrtc_archive libwebrtc-win-x64-release.zip)
  set(_webrtc_sha256 55bde16897e83f3bcaf001ba72c2189e96197193c3160a5eeb154f769fd6551d)
else()
  set(_webrtc_library libwebrtc.so)
  set(_webrtc_archive libwebrtc-linux-x64-release.zip)
  set(_webrtc_sha256 796bd7655e6bea76f851f0420721f7f017fe35c5acbef46b49c908a0801949af)
endif()
if(NOT WEBRTC_ROOT)
  if(NOT DOWNLOAD_WEBRTC OR (NOT WIN32 AND
      (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")))
    message(FATAL_ERROR "Set WEBRTC_ROOT to a compatible standalone libwebrtc SDK. Automatic SDKs support Linux/Windows x64.")
  endif()
  include(FetchContent)
  FetchContent_Declare(webrtc_sdk
    URL https://github.com/webrtc-sdk/libwebrtc/releases/download/libwebrtc.m144.7559.09/${_webrtc_archive}
    URL_HASH SHA256=${_webrtc_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(webrtc_sdk)
  set(WEBRTC_ROOT "${webrtc_sdk_SOURCE_DIR}")
endif()
if(NOT EXISTS "${WEBRTC_ROOT}/include/libwebrtc.h" OR
   NOT EXISTS "${WEBRTC_ROOT}/lib/${_webrtc_library}")
  message(FATAL_ERROR "WEBRTC_ROOT must contain include/libwebrtc.h and lib/${_webrtc_library}")
endif()
add_library(WebRTC SHARED IMPORTED GLOBAL)
set_target_properties(WebRTC PROPERTIES
  IMPORTED_LOCATION "${WEBRTC_ROOT}/lib/${_webrtc_library}"
  INTERFACE_INCLUDE_DIRECTORIES "${WEBRTC_ROOT}/include"
  INTERFACE_COMPILE_DEFINITIONS RTC_DESKTOP_DEVICE)
if(WIN32)
  if(NOT EXISTS "${WEBRTC_ROOT}/lib/libwebrtc.dll.lib")
    message(FATAL_ERROR "Windows SDK is missing lib/libwebrtc.dll.lib")
  endif()
  set_target_properties(WebRTC PROPERTIES
    IMPORTED_IMPLIB "${WEBRTC_ROOT}/lib/libwebrtc.dll.lib")
  target_compile_definitions(WebRTC INTERFACE LIB_WEBRTC_API_DLL)
endif()
