"""Attack test: each player aims (LT) and swings (A) three times; the number of swings (entries into
state 103) the guest shows for that character must equal the host's."""
import sys, time
sys.path.insert(0, '.')
import bot, peek, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def guest_char(cid):
    gg = pg.u32(peek.GAME)
    node = pg.u32(pg.u32(gg + 0x510) + 0x1c)
    while node:
        x = pg.u32(node)
        if pg.u32(x + 0x78) == cid:
            return x
        node = pg.u32(node + 4)


g = ph.u32(peek.GAME)
time.sleep(2)
for name, off, pipe, user in (('player 1 (host pad)', 0x4fc, h, 1), ('player 2 (remote pad)', 0x500, gp, 0)):
    c = ph.u32(ph.u32(g + off) + 0xc)
    gc = guest_char(ph.u32(c + 0x78))
    pipe.cmd('pad %d 0 0 0 0 0 255 0' % user)
    time.sleep(0.8)
    swings_h = swings_g = 0
    last_h = last_g = None
    for k in range(3):
        pipe.cmd('pad %d 1000 0 0 0 0 255 0' % user)   # LT held + A
        t0 = time.time()
        while time.time() - t0 < 1.3:
            a, b = ph.u32(c + 0x60), pg.u32(gc + 0x60)
            if a == 103 and last_h != 103:
                swings_h += 1
            if b == 103 and last_g != 103:
                swings_g += 1
            last_h, last_g = a, b
            if time.time() - t0 > 0.15:
                pipe.cmd('pad %d 0 0 0 0 0 255 0' % user)
            time.sleep(0.004)
    pipe.cmd('pad %d 0' % user)
    time.sleep(0.8)
    check('%s: host swung' % name, swings_h >= 3, '%d swings' % swings_h)
    check('%s: guest shows the same swings' % name, swings_g == swings_h, 'host %d guest %d' % (swings_h, swings_g))
s = gp.cmd('sync')
print('guest', s[s.find('events'):s.find('events') + 30])
print('host ', h.cmd('sync')[h.cmd('sync').find('events'):][:30])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
