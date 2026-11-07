#pragma once
// Default VSPI SCK=18 conflicts with the encoder. Set to 1 only after
// wiring E-Ink SCK to an unused pin and updating EINK_SCK below.
#ifndef ENABLE_EINK
#define ENABLE_EINK 0
#endif
#ifndef EINK_SCK
#define EINK_SCK 18
#endif
#ifndef EINK_MOSI
#define EINK_MOSI 23
#endif
#if ENABLE_EINK && (EINK_SCK == 18 || EINK_SCK == 19 || EINK_MOSI == 18 || EINK_MOSI == 19)
#error "E-Ink SPI pins conflict with the rotary encoder; choose unused GPIOs"
#endif
