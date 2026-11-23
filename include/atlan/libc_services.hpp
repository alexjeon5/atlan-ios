// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"

namespace atlan {
// Typed host implementations of C library services, operating only on guest
// memory through Memory::read/write. Services are added when an observed guest
// import needs them; each reports guest memory faults as service errors.
void install_libc_services(ImportDispatcher& dispatcher);
}
