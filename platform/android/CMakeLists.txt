# SPDX-License-Identifier: GPL-2.0-or-later
# Android host: activity glue (JNI), OpenSL ES audio, GL through platform/gles. An object library so its JNI_OnLoad
# always reaches the app's shared library, which an app adds with atlan_android_app().
add_library(atlan_android_host OBJECT guest_host.cpp opensles_audio_sink.cpp)
target_include_directories(atlan_android_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_link_libraries(atlan_android_host PUBLIC atlan_shell atlan_gles OpenSLES log android)
target_compile_options(atlan_android_host PRIVATE -Wall -Wextra -Werror)

# atlan_android_app(SOURCES...): libatlanhost.so, the one native library of the APK, from the host and the app's
# sources (which define atlan::android::make_app_adapter). Only JNI_OnLoad is exported.
function(atlan_android_app)
  add_library(atlanhost SHARED ${ARGN})
  target_link_libraries(atlanhost PRIVATE atlan_android_host)
  target_link_options(atlanhost PRIVATE
    -Wl,--version-script=${PROJECT_SOURCE_DIR}/platform/android/exports.map
    -Wl,--gc-sections -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1)
  target_compile_options(atlanhost PRIVATE -Wall -Wextra -Werror)
endfunction()
