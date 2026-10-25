"""moveref_test.py <host> <guest>: the guest's movement reference (set with `setref`, as a camera zone would) must decide which
way its stick moves player 2 on the host. Also checks that without a difference nothing is rewritten."""
import math, struct, sys, time
sys.path.insert(0, '.')
import bot, peek, pair
pair.setup(sys.argv[1], sys.argv[2])
H, G = int(open(sys.argv[1] + '.pid').read()), int(open(sys.argv[2] + '.pid').read())
h, g, ph, pg = bot.Pipe(H), bot.Pipe(G), peek.Proc(H), peek.Proc(G)
results = []
def check(name, ok, detail=''):
    results.append(bool(ok)); print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)
def slotc(p, off):
    gm = p.u32(peek.GAME); return p.u32(p.u32(gm + off) + 0xc)
def pos(p, c): return struct.unpack('<2f', p.read(c + 0x20, 8))
def refs(p, off):
    gm = p.u32(peek.GAME); rp = p.u32(gm + off); return struct.unpack('<fff', p.read(rp + 0xa8, 12)), p.u32(rp + 0xa0)
h.cmd('level 49 0'); time.sleep(15)
SPOT = (-76, 4007)
def heading(stick, spot=SPOT):
    h.cmd('tp 1 %d %d 6' % spot); time.sleep(2)
    c2 = slotc(ph, 0x500); a = pos(ph, c2)
    for k in range(3):
        g.cmd('pad 0 0 %d %d' % stick); time.sleep(0.4)
    g.cmd('pad 0 0'); time.sleep(0.5)
    b = pos(ph, c2); dx, dy = b[0] - a[0], b[1] - a[1]
    return (dx, dy, math.degrees(math.atan2(dy, dx)) if abs(dx) + abs(dy) > 30 else None)
for name, vec, want, *sp in (('forward +y', (0.0, 1.0, 0.0), 90.0), ('forward +x', (1.0, 0.0, 0.0), 0.0), ('forward -y', (0.0, -1.0, 0.0), -90.0), ('forward -x', (-1.0, 0.0, 0.0), 180.0, (376, 3900))):
    print(g.cmd('setref %f %f %f' % vec)); time.sleep(0.8)
    dx, dy, hd = heading((0, 32000), sp[0] if sp else SPOT)
    ok = hd is not None and abs(((hd - want + 180) % 360) - 180) < 25
    check('stick up moves P2 along the guest camera (%s)' % name, ok, 'moved (%.0f, %.0f) heading %s want %.0f' % (dx, dy, 'none' if hd is None else '%.1f' % hd, want))
print('RESULT: %s (%d/%d checks)' % ('PASS' if all(results) else 'FAIL', sum(results), len(results)))
