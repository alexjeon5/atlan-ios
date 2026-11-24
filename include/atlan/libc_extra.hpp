// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"

namespace atlan {
// Further C library services over guest memory, C locale only:
// memchr, strstr, strpbrk, strcspn, strncat, strtok, strtol/strtoul/atol/strtod, toupper/tolower and the
// isw*/tow* classes (ASCII; other code points are unclassified), the libm functions in float and double form
// (soft-float ABI: values travel in core registers; computed by the host libm), getpid, and the terminating
// calls abort/exit/__assert2, which stop the guest with an explicit service error rather than returning.
void install_libc_extra(ImportDispatcher& dispatcher);
}
