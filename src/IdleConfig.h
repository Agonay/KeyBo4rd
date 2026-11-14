#pragma once
// How long the keyboard must go without key or knob input before it counts as
// idle. Shared by everything that waits for idle: the E-Ink animation start and
// the automatic update check. Override with -DKEYBOARD_IDLE_MS=<ms>.
#ifndef KEYBOARD_IDLE_MS
#define KEYBOARD_IDLE_MS (10UL * 60UL * 1000UL)   // 10 min
#endif
