"""Drive a running test copy of Obscure through obscoop's command pipe and take window screenshots.

Usage: python -I bot.py <pid> <command> [...]
  ping
  press <user> <button names, comma separated> [hold seconds]
  shot <out.png>
  seq <user> <spec>   spec: "A,DOWN,wait2,k:RETURN" (pad buttons for <user>; k:<KEY> taps a virtual
                      keyboard key; wait<N> sleeps N seconds), screenshot after each step
                      into tests/shots/<n>.png
Buttons: UP DOWN LEFT RIGHT START BACK LT_CLICK RT_CLICK LB RB A B X Y
"""
import ctypes, ctypes.wintypes as W, os, struct, sys, time, zlib

BUTTONS = dict(UP=0x1, DOWN=0x2, LEFT=0x4, RIGHT=0x8, START=0x10, BACK=0x20, LT_CLICK=0x40, RT_CLICK=0x80,
               LB=0x100, RB=0x200, A=0x1000, B=0x2000, X=0x4000, Y=0x8000)
KEYS = dict(ESCAPE=0x01, RETURN=0x1C, SPACE=0x39, UP=0xC8, DOWN=0xD0, LEFT=0xCB, RIGHT=0xCD, LSHIFT=0x2A,
            RCONTROL=0x9D, BACK=0x0E, W=0x11, A=0x1E, S=0x1F, D=0x20, E=0x12, Q=0x10, TAB=0x0F)
HERE = os.path.dirname(os.path.abspath(__file__))


class Pipe:
    def __init__(self, pid):
        self.f = open(r'\\.\pipe\obscoop_%d' % pid, 'r+b', buffering=0)

    def cmd(self, line):
        self.f.write((line + '\n').encode())
        out = b''
        while not out.endswith(b'\n'):
            out += self.f.read(1)
        return out.decode().strip()


def tap(pipe, name, hold=0.15):
    code = KEYS[name.upper()]
    r1 = pipe.cmd('key %x 1' % code)
    time.sleep(hold)
    r2 = pipe.cmd('key %x 0' % code)
    return r1, r2


def press(pipe, user, names, hold=0.15):
    mask = 0
    for n in names.split(','):
        mask |= BUTTONS[n.strip().upper()]
    r1 = pipe.cmd('pad %d %x' % (user, mask))
    time.sleep(hold)
    r2 = pipe.cmd('pad %d 0' % user)
    return r1, r2


user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32')
WNDENUMPROC = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)


def window_of(pid):
    found = []

    def cb(h, _):
        p = W.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            found.append(h)
        return True
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found[0] if found else None


def write_png(path, w, h, bgra):
    rows = b''.join(b'\0' + bytes(bgra[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    # BGRA -> RGBA per row (skip the filter byte)
    out = bytearray(rows)
    for y in range(h):
        base = y * (w * 4 + 1) + 1
        out[base:base + w * 4:4], out[base + 2:base + w * 4:4] = rows[base + 2:base + w * 4:4], rows[base:base + w * 4:4]
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(out), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def shot(pid, path, scale=2):
    h = window_of(pid)
    if not h:
        raise SystemExit('no window for pid %d' % pid)
    r = W.RECT()
    user32.GetClientRect(h, ctypes.byref(r))
    w, hh = r.right, r.bottom
    wdc = user32.GetDC(h)
    mdc = gdi32.CreateCompatibleDC(wdc)
    bmp = gdi32.CreateCompatibleBitmap(wdc, w, hh)
    gdi32.SelectObject(mdc, bmp)
    ok = user32.PrintWindow(h, mdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    class BIH(ctypes.Structure):
        _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG), ('biPlanes', W.WORD),
                    ('biBitCount', W.WORD), ('biCompression', W.DWORD), ('biSizeImage', W.DWORD),
                    ('biXPelsPerMeter', W.LONG), ('biYPelsPerMeter', W.LONG), ('biClrUsed', W.DWORD),
                    ('biClrImportant', W.DWORD)]
    bih = BIH(ctypes.sizeof(BIH), w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = (ctypes.c_ubyte * (w * hh * 4))()
    gdi32.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bih), 0)
    gdi32.DeleteObject(bmp); gdi32.DeleteDC(mdc); user32.ReleaseDC(h, wdc)
    data = bytes(buf)
    if scale > 1:   # nearest-neighbour downscale to keep screenshots small
        sw, sh = w // scale, hh // scale
        rows = []
        for y in range(sh):
            row = data[(y * scale) * w * 4:(y * scale + 1) * w * 4]
            rows.append(b''.join(row[x * scale * 4:x * scale * 4 + 4] for x in range(sw)))
        data, w, hh = b''.join(rows), sw, sh
    write_png(path, w, hh, data)
    return ok, w, hh


if __name__ == '__main__':
    pid, what = int(sys.argv[1]), sys.argv[2]
    if what == 'shot':
        print(shot(pid, sys.argv[3]))
    else:
        p = Pipe(pid)
        if what == 'ping':
            print(p.cmd('ping'))
        elif what == 'press':
            print(press(p, int(sys.argv[3]), sys.argv[4], float(sys.argv[5]) if len(sys.argv) > 5 else 0.15))
        elif what == 'seq':
            os.makedirs(os.path.join(HERE, 'shots'), exist_ok=True)
            for i, step in enumerate(sys.argv[4].split(',')):
                if step.startswith('wait'):
                    time.sleep(float(step[4:]))
                elif step.startswith('k:'):
                    print(tap(p, step[2:]))
                    time.sleep(0.6)
                else:
                    press(p, int(sys.argv[3]), step)
                    time.sleep(0.6)
                out = os.path.join(HERE, 'shots', '%02d_%s.png' % (i, step.replace(':', '-')))
                shot(pid, out)
                print(i, step, '->', out)
        p.f.close()
