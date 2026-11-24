# SPDX-License-Identifier: GPL-2.0-or-later
enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
add_executable(AtlanRuntime MACOSX_BUNDLE main.mm)
target_link_libraries(AtlanRuntime PRIVATE atlan_shell atlan_apple_audio "-framework UIKit" "-framework Foundation" "-framework UniformTypeIdentifiers")
target_compile_options(AtlanRuntime PRIVATE -fobjc-arc -Wall -Wextra -Werror)
set_target_properties(AtlanRuntime PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "org.example.atlan-runtime"
  MACOSX_BUNDLE_BUNDLE_NAME "ARM32 Runtime"
  MACOSX_BUNDLE_BUNDLE_VERSION "1"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
