#ifndef ROTARY_ENCODER_H
#define ROTARY_ENCODER_H

#include "BluetoothHID.h"
#include "MenuManager.h"

void initRotaryEncoder();
void handleButtonPress();
void handleRotaryEncoder();

enum ButtonEvent { BTN_NONE, BTN_SHORT, BTN_DOUBLE, BTN_LONG, BTN_VERYLONG };
ButtonEvent pollButtonEvent();
unsigned long getLastKnobActivityTime();

// GPIO18 (CLK) is shared with the E-Ink SPI clock: suspend before driving the panel,
// resume after SPI is released. Both are no-ops before initRotaryEncoder().
void encoderSuspend();
void encoderResume();

#endif
