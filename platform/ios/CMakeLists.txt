# SPDX-License-Identifier: GPL-2.0-or-later
enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
# The launcher screens are the Atlan app's own UI (adapter side, docs/engine-adapter.md).
set(ATLAN_LAUNCHER_SOURCES
  ../../adapters/atlan/ios/launcher_kit.mm
  ../../adapters/atlan/ios/launcher_settings.mm
  ../../adapters/atlan/ios/launcher_style.mm
  ../../adapters/atlan/ios/launcher_verified.mm
  ../../adapters/atlan/ios/launcher_view_controller.mm)
# The design's fonts (OFL-1.1, adapters/atlan/ios/fonts/README.md), copied into the app and listed in UIAppFonts.
set(ATLAN_LAUNCHER_FONTS
  ../../adapters/atlan/ios/fonts/Barlow-Regular.ttf
  ../../adapters/atlan/ios/fonts/Barlow-Medium.ttf
  ../../adapters/atlan/ios/fonts/Barlow-SemiBold.ttf
  ../../adapters/atlan/ios/fonts/NotoSansKR-Hangul.ttf)
add_executable(AtlanRuntime MACOSX_BUNDLE main.mm ${ATLAN_LAUNCHER_SOURCES} ${ATLAN_LAUNCHER_FONTS} ../apple/metal_gl_backend.mm ../apple/apple_network.mm)
target_include_directories(AtlanRuntime PRIVATE ../apple)
target_link_libraries(AtlanRuntime PRIVATE atlan_adapter atlan_apple_audio "-framework UIKit" "-framework CoreGraphics" "-framework CoreText" "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework UniformTypeIdentifiers" "-framework CoreLocation" "-framework Network")
target_compile_options(AtlanRuntime PRIVATE -fobjc-arc -Wall -Wextra -Werror)
set_target_properties(AtlanRuntime PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.hanggok.atlan"
  MACOSX_BUNDLE_BUNDLE_NAME "Atlan"
  MACOSX_BUNDLE_BUNDLE_VERSION "1"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  RESOURCE "${ATLAN_LAUNCHER_FONTS}")
