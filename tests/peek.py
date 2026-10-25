"""Read-only memory peek into a running Obscure.exe (ReadProcessMemory, no debugger attach)."""
import ctypes, ctypes.wintypes as W, struct, sys, subprocess

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
k32.OpenProcess.restype = W.HANDLE
k32.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
GAME = 0x6ab8e4  # DAT_006ab8e4, global game object pointer (static analysis)


def pid_of(name='Obscure.exe'):
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ' + name, '/FO', 'CSV', '/NH'], capture_output=True, text=True, encoding='mbcs', errors='replace').stdout
    rows = [l.split('","') for l in out.strip().splitlines() if l.startswith('"')]
    return [int(r[1]) for r in rows]


class Proc:
    def __init__(self, pid):
        self.h = k32.OpenProcess(0x0010 | 0x0400, False, pid)   # VM_READ | QUERY_INFORMATION
        if not self.h:
            raise OSError(ctypes.get_last_error())
        mods = (W.HMODULE * 256)(); need = W.DWORD()
        psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 1)  # LIST_MODULES_32BIT
        self.base = mods[0] or 0x400000

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if b else None

    def va(self, static_va):  # rebase a static address if the exe was relocated
        return static_va - 0x400000 + self.base


def report(p):
    g = p.u32(p.va(GAME))
    print('module base %#x  game object %#x' % (p.base, g or 0))
    if not g:
        return
    print('game+8 (state) =', p.u32(g + 8), ' level name =', (p.read(g + 0x590, 32) or b'').split(b'\0')[0], ' entry =', p.u32(g + 0x5d0))
    for i, off in enumerate((0x4fc, 0x500)):
        rp = p.u32(g + off)
        line = 'real player %d @ %#x' % (i, rp or 0)
        if rp:
            ch = p.u32(rp + 0xc); ctl = p.u32(rp + 0x84)
            cobj = p.u32(ctl) if ctl else None
            line += '  character %#x' % (ch or 0)
            if ch:
                hp = struct.unpack('<f', p.read(ch + 0x54, 4))[0]
                line += ' (+0x54 %.3f +0x58 %s +0x5c %s +0x68 %s)' % (hp, p.u32(ch + 0x58), p.u32(ch + 0x5c), p.u32(ch + 0x68))
            if cobj:
                line += '  controller %#x type %s' % (cobj, p.u32(cobj + 4))
        print(line)
    flags = p.u32(g + 0xa24)
    if flags:
        f = p.read(flags, 161)
        print('story flags (nonzero):', {i: b for i, b in enumerate(f) if b})


if __name__ == '__main__':
    pids = pid_of()
    print('Obscure.exe pids', pids)
    for pid in pids:
        report(Proc(pid))
