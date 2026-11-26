// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"

namespace atlan {
// Typed host implementations of C library services, operating only on guest
// memory through Memory::read/write. Services are added when an observed guest
// import needs them; each reports guest memory faults as service errors.
void install_libc_services(ImportDispatcher& dispatcher);
// localtime/gmtime(+_r) and mktime using the host C library and time zone. The non-reentrant forms return a
// pointer to a 44-byte bionic `struct tm` at `static_tm`, a writable guest address the embedder reserves.
void install_time_services(ImportDispatcher& dispatcher, std::uint32_t static_tm);
}
