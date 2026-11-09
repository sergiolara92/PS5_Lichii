// SPDX-License-Identifier: GPL-3.0-or-later
// The JIT switches main.cpp sets from dev settings, for builds without reg_alloc.cmake's register-allocator work
// (EDEN_REG_ALLOC_SCOPE off: the Lichii base until it is re-targeted). Same names and defaults as there.
extern "C" {
bool eden_free_moves = true;
bool eden_alloc_r13 = true;
bool eden_plain_ordered_reads = true;  // headless/CMakeLists.txt, ordered reads
bool eden_fcmp_adc = true, eden_light_dmb = true, eden_checkbit_flags = true;  // fast_paths.cmake
bool eden_jit_frame_once = true;  // fast_paths.cmake: set before any JIT is created, never after
unsigned eden_jit_pin_count = 0;  // reg_pin.cmake: set before any JIT is created, never after
unsigned char eden_jit_pins[2] = {};
unsigned long long eden_dmb_dropped = 0, eden_checkbit_fused = 0;  // fast_paths.cmake
unsigned long long eden_free_moves_done = 0, eden_free_moves_spilled = 0;
}
