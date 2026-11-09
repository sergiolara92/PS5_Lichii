// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in header (rewritten 2026-10-29; missing from the shipped snapshot).
// Only main.cpp's device-cycle hooks are provided; devices_check.cpp is not built here.
#pragma once
namespace Eden::Mock {
inline void BeginGuestCycle() {}
inline void CheckGuestCycle() {}
}
