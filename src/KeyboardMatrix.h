#ifndef KEYBOARD_MATRIX_H
#define KEYBOARD_MATRIX_H

#include "BluetoothHID.h"
#include "KeyDefinitions.h"

void initKeyboardMatrix();
void scanKeyboardMatrix();
void sendKeyPress(int row, int col, bool shiftPressed, bool altPressed);
void releaseKey(int row, int col);

#endif
