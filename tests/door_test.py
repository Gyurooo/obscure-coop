"""Door test: host player 1 uses door 1 in the courtyard (action button facing it). Both copies must
end up in the same new level, the guest must stay responsive, and both players must be in it."""
import struct, subprocess, sys, time
sys.path.insert(0, '.')
import bot, invpeek, peek, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def responding(pid):
    out = subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pid],
                         capture_output=True, text=True).stdout.strip()
    return out == 'True'


def level(p):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510)
    return p.read(mgr + 8, 1)[0] if mgr else None


g = ph.u32(peek.GAME)
start = level(ph)
node = ph.u32(ph.u32(g + 0x510) + 0x30)
door = None
while node:
    it = ph.u32(node)
    if ph.u32(it + 4) == 1 and ph.u32(it + 8) == 1:
        door = (struct.unpack('<2f', ph.read(it + 0xc, 8)), struct.unpack('<2f', ph.read(it + 0x8c, 8)))
    node = ph.u32(node + 4)
(px, py), (ix, iy) = door
n = ((px - ix) ** 2 + (py - iy) ** 2) ** 0.5 or 1
h.cmd('teleport %.1f %.1f 3 %.3f %.3f' % (ix, iy, (px - ix) / n, (py - iy) / n))
c1 = ph.u32(ph.u32(g + 0x4fc) + 0xc)
presses = 0
for _ in range(3):   # the teleport needs about 2 s to settle; a press during it is lost, so press again
    time.sleep(2.5)
    bot.press(h, 1, 'A', 0.3)
    presses += 1
    time.sleep(1)
    if ph.u32(c1 + 0x60) == 50 or level(ph) != start:   # state 50: using the door
        break
print('A presses:', presses)
guest_states = set()
gid = pair.plays(ph, 0x4fc)
t0 = time.time()
while time.time() - t0 < 30 and not (level(ph) not in (None, start) and level(pg) == level(ph)):
    for c in invpeek.humans(pg):
        if pg.u32(c + 0x78) == gid:
            guest_states.add(pg.u32(c + 0x60))
    time.sleep(0.05)
time.sleep(4)
check('host went through the door into another level', level(ph) not in (None, start), '%s -> %s' % (start, level(ph)))
check('guest showed the door animation (its copy reached state 50)', 50 in guest_states, str(sorted(x for x in guest_states if x)))
log = open('../obscure-test2/obscoop.log', errors='replace').read()
check('guest ignored its own door level change and followed the host once', 'follows host' in log and log.count('follows host') == 1, '%d ignored, %d follows' % (log.count('ignored (the host leads)'), log.count('follows host')))
check('guest followed into the same level', level(pg) == level(ph), 'guest %s' % level(pg))
check('guest game still responding', responding(G))
check('host game still responding', responding(H))
hc = pair.plays(ph, 0x4fc), pair.plays(ph, 0x500)
check('both players in the new level on the host', all(hc), str(hc))
print('guest log lines:', subprocess.run(['grep', '-c', 'not mirrored', '../obscure-test2/obscoop.log'], capture_output=True, text=True).stdout.strip())
bot.shot(H, 'door2_host.png')
bot.shot(G, 'door2_guest.png')
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
