# Standalone Google libwebrtc, exposed through webrtc-sdk's C++ ABI wrapper.
# Pin both release and checksum; never silently consume a moving SDK ABI.
set(WEBRTC_ROOT "" CACHE PATH "Extracted libwebrtc SDK (contains include/ and lib/)")
option(DOWNLOAD_WEBRTC "Download the pinned Linux x86_64 SDK when WEBRTC_ROOT is empty" ON)
if(NOT WEBRTC_ROOT)
  if(NOT DOWNLOAD_WEBRTC OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "Set WEBRTC_ROOT to a compatible standalone libwebrtc SDK. The automatic SDK is Linux x86_64 only.")
  endif()
  include(FetchContent)
  FetchContent_Declare(webrtc_sdk
    URL https://github.com/webrtc-sdk/libwebrtc/releases/download/libwebrtc.m144.7559.09/libwebrtc-linux-x64-release.zip
    URL_HASH SHA256=796bd7655e6bea76f851f0420721f7f017fe35c5acbef46b49c908a0801949af
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(webrtc_sdk)
  set(WEBRTC_ROOT "${webrtc_sdk_SOURCE_DIR}")
endif()
if(NOT EXISTS "${WEBRTC_ROOT}/include/libwebrtc.h" OR NOT EXISTS "${WEBRTC_ROOT}/lib/libwebrtc.so")
  message(FATAL_ERROR "WEBRTC_ROOT must contain include/libwebrtc.h and lib/libwebrtc.so")
endif()
add_library(WebRTC SHARED IMPORTED GLOBAL)
set_target_properties(WebRTC PROPERTIES
  IMPORTED_LOCATION "${WEBRTC_ROOT}/lib/libwebrtc.so"
  INTERFACE_INCLUDE_DIRECTORIES "${WEBRTC_ROOT}/include"
  INTERFACE_COMPILE_DEFINITIONS RTC_DESKTOP_DEVICE)
