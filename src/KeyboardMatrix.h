#ifndef KEYBOARD_MATRIX_H
#define KEYBOARD_MATRIX_H

#include "BluetoothHID.h"
#include "KeyDefinitions.h"

void initKeyboardMatrix();
void scanKeyboardMatrix();
unsigned long getLastKeyActivityTime();

#endif
