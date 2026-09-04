#ifndef ROTARY_ENCODER_H
#define ROTARY_ENCODER_H

#include "BluetoothHID.h"
#include "MenuManager.h"

void initRotaryEncoder();
void handleButtonPress();
void handleRotaryEncoder();

enum ButtonEvent { BTN_NONE, BTN_SHORT, BTN_LONG, BTN_VERYLONG };
ButtonEvent pollButtonEvent();

#endif
