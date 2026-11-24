# SPDX-License-Identifier: GPL-2.0-or-later
set(a32 cpu/arm64/aarch32)
add_library(kernarium_aarch32 STATIC
  ${a32}/a32_encodings.cpp
  ${a32}/a32_operand_decode.cpp
  ${a32}/decode.cpp
  ${a32}/form_operation.cpp
  ${a32}/semantics.cpp
  ${a32}/semantics_integer.cpp
  ${a32}/semantics_memory.cpp
  ${a32}/semantics_control.cpp
  ${a32}/semantics_table.cpp
  ${a32}/semantics_prepared.cpp)
target_include_directories(kernarium_aarch32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(kernarium_aarch32 PRIVATE
  DARWIN_UML_GUEST_PAGE_SIZE=4096 UML_A32_LIBRARY_EXIT_COUNTERS=0)
target_compile_options(kernarium_aarch32 PRIVATE -Wall -Wextra -Werror)
