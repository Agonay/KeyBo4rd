"""Verify the firmware HID positions against Windows Spanish (Spain).
Run on Windows: python tests/windows_layout.py
No keyboard input is injected; ToUnicodeEx uses a separate keyboard state.
"""
import ctypes as c
from pathlib import Path
import re

u = c.WinDLL('user32', use_last_error=True)
u.LoadKeyboardLayoutW.argtypes = [c.c_wchar_p, c.c_uint]
u.LoadKeyboardLayoutW.restype = c.c_void_p
u.MapVirtualKeyExW.argtypes = [c.c_uint, c.c_uint, c.c_void_p]
u.MapVirtualKeyExW.restype = c.c_uint
u.ToUnicodeEx.argtypes = [c.c_uint, c.c_uint, c.POINTER(c.c_ubyte), c.c_wchar_p, c.c_int, c.c_uint, c.c_void_p]
u.ToUnicodeEx.restype = c.c_int
layout = u.LoadKeyboardLayoutW('0000040A', 0)
assert layout, 'Spanish keyboard layout unavailable'
source = (Path(__file__).resolve().parents[1] / 'src/KeyDefinitions.h').read_text()
# USB usages to PC make scan codes for the positions exercised here.
scans = {0x33:0x27, 0x35:0x29, 0x2d:0x0c, 0x2e:0x0d, 0x34:0x28,
         0x36:0x33, 0x37:0x34, 0x38:0x35, 0x64:0x56, 0x2f:0x1a,
         0x30:0x1b, 0x1f:0x03, 0x08:0x12, 0x20:0x04}

def translate(name, shift=False, altgr=False):
    match = re.search(r'Key_' + name + r' = \{(0x[0-9a-f]+)(?:, (0x[0-9a-f]+))?\}', source)
    assert match, name
    usage = int(match[1], 16)
    implicit = int(match[2] or '0', 16)
    state = (c.c_ubyte * 256)()
    if shift: state[0x10] = state[0xa0] = 0x80
    if altgr or implicit & 0x40:
        state[0x11] = state[0x12] = state[0xa5] = 0x80
    scan = scans[usage]
    vk = u.MapVirtualKeyExW(scan, 3, layout)
    output = c.create_unicode_buffer(8)
    count = u.ToUnicodeEx(vk, scan, state, output, 8, 4, layout)
    return count, output.value

cases = [('Nie',False,False,'ñ'), ('Nie',True,False,'Ñ'),
         ('Grado',False,False,'º'), ('Grado',True,False,'ª'),
         ('Apostrophe_ES',False,False,"'"), ('Apostrophe_ES',True,False,'?'),
         ('ExclInv',False,False,'¡'), ('ExclInv',True,False,'¿'),
         ('Comma_ES',False,False,','), ('Comma_ES',True,False,';'),
         ('Period_ES',False,False,'.'), ('Period_ES',True,False,':'),
         ('Guion',False,False,'-'), ('Guion',True,False,'_'),
         ('LessThan',False,False,'<'), ('LessThan',True,False,'>'),
         ('LeftBracket',False,False,'['), ('RightBracket',False,False,']'),
         ('Backslash',False,False,'\\'), ('2',False,True,'@'),
         ('E',False,True,'€'), ('3',False,True,'#')]
for name, shift, altgr, expected in cases:
    count, actual = translate(name, shift, altgr)
    assert count > 0 and actual == expected, (name, shift, altgr, expected, count, actual)
for shift, expected in [(False, '´'), (True, '¨')]:
    count, actual = translate('Acento', shift)
    assert count < 0 and actual == expected, ('dead key', shift, count, actual)
print(f'PASS: {len(cases)} Spanish characters and 2 dead keys from firmware definitions')
