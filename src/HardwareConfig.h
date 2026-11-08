#pragma once
#ifndef ENABLE_EINK
#define ENABLE_EINK 1
#endif
// The E-Ink SCK is wired to GPIO18, the same pin as the encoder CLK. The pin is
// time-shared: the encoder is paused while the panel is being driven
// (see einkAcquireBus/einkReleaseBus in DisplayManager.cpp). Move the wire to a
// free GPIO (e.g. 32) and set EINK_SCK to drop the sharing.
#ifndef EINK_SCK
#define EINK_SCK 18
#endif
#ifndef EINK_MOSI
#define EINK_MOSI 23
#endif
#define EINK_SHARES_ENCODER_PIN (EINK_SCK == 18)
#if ENABLE_EINK && (EINK_SCK == 19 || EINK_MOSI == 18 || EINK_MOSI == 19)
#error "E-Ink SPI pins conflict with the rotary encoder; choose unused GPIOs"
#endif
