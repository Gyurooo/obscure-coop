"""Player 2 door test: the remote player (guest pad 0) uses door 1. Both copies must reach the same new
level with both players present on each, and the guest must keep running while player 2 then walks
away from player 1 (camera zone changes)."""
import struct, subprocess, sys, time
sys.path.insert(0, '.')
import bot, invpeek, peek, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def responding(pid):
    return subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pid],
                          capture_output=True, text=True).stdout.strip() == 'True'


def level(p):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510)
    return p.read(mgr + 8, 1)[0] if mgr else None


def positions(p):
    return {p.u32(c + 0x78): struct.unpack('<2f', p.read(c + 0x20, 8)) for c in invpeek.humans(p)}


def applied():
    s = gp.cmd('sync')
    return int(s.split('applied ')[1].split()[0])


g = ph.u32(peek.GAME)
start = level(ph)
px, py, ix, iy = -2158.88, 108.31, -2093.88, 33.31   # door 1 of level 193 (courtyard), see door_test.py
n = ((px - ix) ** 2 + (py - iy) ** 2) ** 0.5
p1, p2 = pair.plays(ph, 0x4fc), pair.plays(ph, 0x500)
c2 = ph.u32(ph.u32(g + 0x500) + 0xc)
print(h.cmd('tp 1 %.1f %.1f 3 %.3f %.3f' % (ix, iy, (px - ix) / n, (py - iy) / n)))
presses = 0
for _ in range(3):
    time.sleep(2.5)
    bot.press(gp, 0, 'A', 0.3)   # the remote player's controller
    presses += 1
    time.sleep(1)
    if ph.u32(c2 + 0x60) == 50 or level(ph) != start:
        break
print('A presses:', presses, 'p2 state', ph.u32(c2 + 0x60))
guest_states = set()
gid = pair.plays(ph, 0x500)
t0 = time.time()
while time.time() - t0 < 30 and not (level(ph) not in (None, start) and level(pg) == level(ph)):
    for c in invpeek.humans(pg):
        if pg.u32(c + 0x78) == gid:
            guest_states.add(pg.u32(c + 0x60))
    time.sleep(0.05)
time.sleep(5)
check('host changed level after player 2 used the door', level(ph) not in (None, start), '%s -> %s' % (start, level(ph)))
check('guest showed the door animation (its copy reached state 50)', 50 in guest_states, str(sorted(x for x in guest_states if x)))
log = open('../obscure-test2/obscoop.log', errors='replace').read()
check('guest ignored its own door level change and followed the host once', 'follows host' in log and log.count('follows host') == 1, '%d ignored, %d follows' % (log.count('ignored (the host leads)'), log.count('follows host')))
check('guest is in the same level', level(pg) == level(ph), 'guest %s' % level(pg))
hp, gpos = positions(ph), positions(pg)
print('  host chars', hp)
print('  guest chars', gpos)
check('host: both players are in the new level', p1 in hp and p2 in hp, 'p1 %s p2 %s' % (p1, p2))
check('guest: both players are in the new level', p1 in gpos and p2 in gpos)
close = all(k in gpos and abs(hp[k][0] - gpos[k][0]) + abs(hp[k][1] - gpos[k][1]) < 60 for k in (p1, p2))
check('guest positions of both players match the host', close)
bot.shot(H, 'p2door_host.png')
bot.shot(G, 'p2door_guest.png')
# player 2 walks away from player 1: several camera zones
frozen = None
for step, (lx, ly) in enumerate([(0, 32000), (32000, 0), (0, -32000), (-32000, 0)] * 2):
    gp.cmd('pad 0 0 %d %d' % (lx, ly))
    for _ in range(3):
        a = applied()
        time.sleep(1)
        if not responding(G) or applied() == a:
            frozen = frozen or 'step %d' % step
    print('  step %d p2 at %s guest applied %d' % (step, positions(ph).get(p2), applied()) if not frozen else '  frozen at ' + frozen, flush=True)
    if frozen:
        break
gp.cmd('pad 0 0')
check('guest kept running and applying state while player 2 walked away (24 s)', not frozen, frozen or '')
check('host still responding', responding(H))
bot.shot(H, 'p2walk_host.png')
bot.shot(G, 'p2walk_guest.png')
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
