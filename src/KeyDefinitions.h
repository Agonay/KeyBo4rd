#ifndef KEY_DEFINITIONS_H
#define KEY_DEFINITIONS_H

#include <Arduino.h>
#include <BleKeyboard.h>

struct Key {
  uint8_t normalKey;
  uint8_t shiftKey;
  uint8_t altKey;
  bool isModifier;
};

struct KeyState {
  bool isPressed;
  bool lastRawState;
  unsigned long stateChangeTime;
  unsigned long firstPressTime;
  unsigned long lastRepeatTime;
  bool wasSent;
};

// ---------- Alphabet Keys ----------
const Key Key_A = {'a', 'A'};
const Key Key_B = {'b', 'B'};
const Key Key_C = {'c', 'C'};
const Key Key_D = {'d', 'D'};
const Key Key_E = {'e', 'E'};
const Key Key_F = {'f', 'F'};
const Key Key_G = {'g', 'G'};
const Key Key_H = {'h', 'H'};
const Key Key_I = {'i', 'I'};
const Key Key_J = {'j', 'J'};
const Key Key_K = {'k', 'K'};
const Key Key_L = {'l', 'L'};
const Key Key_M = {'m', 'M'};
const Key Key_N = {'n', 'N'};
const Key Key_O = {'o', 'O'};
const Key Key_P = {'p', 'P'};
const Key Key_Q = {'q', 'Q'};
const Key Key_R = {'r', 'R'};
const Key Key_S = {'s', 'S'};
const Key Key_T = {'t', 'T'};
const Key Key_U = {'u', 'U'};
const Key Key_V = {'v', 'V'};
const Key Key_W = {'w', 'W'};
const Key Key_X = {'x', 'X'};
const Key Key_Y = {'y', 'Y'};
const Key Key_Z = {'z', 'Z'};
//  /  — physically at the US semicolon position
const Key Key_Nie = {';', ':'};

// ---------- Number Keys ----------
// Modifier is handled separately; OS produces the Spanish char
const Key Key_0 = {'0', '0', '0'};
const Key Key_1 = {'1', '1', '1'};
const Key Key_2 = {'2', '2', '2'};
const Key Key_3 = {'3', '3', '3'};
const Key Key_4 = {'4', '4', '4'};
const Key Key_5 = {'5', '5', '5'};
const Key Key_6 = {'6', '6', '6'};
const Key Key_7 = {'7', '7', '7'};
const Key Key_8 = {'8', '8', '8'};
const Key Key_9 = {'9', '9', '9'};

// ---------- Punctuation & Symbols ----------
//  /  — key left of 1 (US backtick position)
const Key Key_Grado = {'`', '~'};
// ' / ? — at US minus position
const Key Key_Apostrophe_ES = {'\'', '_'};
//  /  — at US equals position
const Key Key_ExclInv = {'=', '+'};
// dead  / dead  — at US [ position
const Key Key_Acento = {'[', '{'};
// + / * — at US ] position
const Key Key_Mas = {']', '}'};
// , / ; — at US comma position
const Key Key_Comma_ES = {',', ';'};
// . / : — at US period position
const Key Key_Period_ES = {'.', ':'};
// - / _ — at US slash position
const Key Key_Guion = {'/', '?'};
// [ / { — literal brackets (if physically present)
const Key Key_LeftBracket  = {'[', '{'};
const Key Key_RightBracket = {']', '}'};
// \ / |
const Key Key_Backslash = {'\\', '|'};

// ---------- Modifier Keys ----------
const Key Key_Shift      = {KEY_LEFT_SHIFT,  0, 0, true};
const Key Key_CapsLock   = {KEY_CAPS_LOCK,    0, 0, true};
const Key Key_Ctrl       = {KEY_LEFT_CTRL,    0, 0, true};
const Key Key_Left_Alt   = {KEY_LEFT_ALT,     0, 0, true};
const Key Key_Right_Alt  = {KEY_RIGHT_ALT,    0, 0, true};  // AltGr
const Key Key_Cmd        = {KEY_LEFT_GUI,     0, 0, true};

// ---------- Function Keys ----------
const Key Key_F1  = {KEY_F1,  0};
const Key Key_F2  = {KEY_F2,  0};
const Key Key_F3  = {KEY_F3,  0};
const Key Key_F4  = {KEY_F4,  0};
const Key Key_F5  = {KEY_F5,  0};
const Key Key_F6  = {KEY_F6,  0};
const Key Key_F7  = {KEY_F7,  0};
const Key Key_F8  = {KEY_F8,  0};
const Key Key_F9  = {KEY_F9,  0};
const Key Key_F10 = {KEY_F10, 0};
const Key Key_F11 = {KEY_F11, 0};
const Key Key_F12 = {KEY_F12, 0};

// ---------- Navigation / Action Keys ----------
const Key Key_Enter      = {KEY_RETURN,       0};
const Key Key_Backspace  = {KEY_BACKSPACE,    0};
const Key Key_Tab        = {KEY_TAB,          0};
const Key Key_Space      = {' ', ' ', ' '};
const Key Key_Escape     = {KEY_ESC,          0};
const Key Key_Delete     = {KEY_DELETE,       0};
const Key Key_Home       = {KEY_HOME,         0};
const Key Key_End        = {KEY_END,          0};
const Key Key_PageUp     = {KEY_PAGE_UP,      0};
const Key Key_PageDown   = {KEY_PAGE_DOWN,    0};
const Key Key_ArrowUp    = {KEY_UP_ARROW,     0};
const Key Key_ArrowDown  = {KEY_DOWN_ARROW,   0};
const Key Key_ArrowLeft  = {KEY_LEFT_ARROW,   0};
const Key Key_ArrowRight = {KEY_RIGHT_ARROW,  0};

#endif // KEY_DEFINITIONS_H
