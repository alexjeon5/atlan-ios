// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"

namespace atlan {
// Small system calls the guest reaches from ordinary screens: sysinfo and statfs (plausible device figures),
// settimeofday (refused, EPERM: the host clock is not the guest's to set), readlink (EINVAL: no /proc),
// pthread_getschedparam (default policy and priority) and fgetc (one byte through the fread service bound before).
// `errno_address` is the guest errno word.
void install_system_services(ImportDispatcher& dispatcher, std::uint32_t errno_address);
}
