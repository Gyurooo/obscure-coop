"""props_test.py <host> <guest>: movable props (chairs, wheeled objects) mirrored from the host to the guest.
Level 54 has 6 chairs and 4 wheeled objects. Checks: same list on both copies; a prop moved on the host with the engine's
setter follows on the guest; a character teleported into a chair pushes it on the host and the guest follows."""
import math, re, struct, sys, time
sys.path.insert(0, '.')
import bot, peek, pair
pair.setup(sys.argv[1], sys.argv[2])
H, G = int(open(sys.argv[1] + '.pid').read()), int(open(sys.argv[2] + '.pid').read())
h, g, ph, pg = bot.Pipe(H), bot.Pipe(G), peek.Proc(H), peek.Proc(G)
results = []
def check(name, ok, detail=''):
    results.append(bool(ok)); print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)
def props(pipe):
    r = pipe.cmd('props'); out = {}
    for m in re.finditer(r'(\d+)/(\d+)@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)', r):
        out[(int(m.group(1)), int(m.group(2)))] = tuple(float(m.group(i)) for i in (3, 4, 5))
    return r, out
def lvl(p):
    gm = p.u32(peek.GAME); mgr = p.u32(gm + 0x510); return p.read(mgr + 8, 1)[0] if mgr else None
print(h.cmd('level 54 0'))
t0 = time.time()
while time.time() - t0 < 60 and not (lvl(ph) == 54 and lvl(pg) == 54): time.sleep(1)
time.sleep(8)
rh, hp = props(h); rg, gp = props(g)
print('host:', rh[:200]); print('guest:', rg[:200])
check('both copies are in level 54 and list the same props', lvl(ph) == 54 and lvl(pg) == 54 and len(hp) >= 6 and set(hp) == set(gp), '%d vs %d props' % (len(hp), len(gp)))
def dist(a, b): return math.dist(a, b)
check('initial positions agree', all(dist(hp[k], gp[k]) < 1.0 for k in hp if k in gp), 'worst %.2f' % max([dist(hp[k], gp[k]) for k in hp if k in gp] or [0]))
casters = [k for k in hp if k[0] == 2]; chairs = [k for k in hp if k[0] == 1]
# 1. engine setter on the host
if casters:
    k = casters[0]; x, y, z = hp[k]
    print(h.cmd('propset %d %d %.1f %.1f %.1f' % (k[0], k[1], x + 60, y + 40, z))); time.sleep(3)
    _, hp2 = props(h); _, gp2 = props(g)
    check('a wheeled object moved on the host follows on the guest', dist(hp2[k], (x + 60, y + 40, z)) < 2 and dist(gp2[k], hp2[k]) < 2, 'host %s guest %s' % (tuple(round(v) for v in hp2[k]), tuple(round(v) for v in gp2[k])))
# 2. a character placed in contact with a prop shoves it through the game's own physics (walking input is not reliable
# right after a level change, so the contact comes from a teleport next to the prop)
pushed = None
for k in casters + chairs:
    x, y, z = hp[k]
    for ox, oy in ((-110, 0), (110, 0), (0, -110), (0, 110), (-70, 0), (70, 0), (0, -70), (0, 70), (-40, 0), (40, 0), (0, -40), (0, 40)):
        _, base = props(h)
        h.cmd('tp 1 %.1f %.1f 6' % (x + 500, y + 500))
        h.cmd('tp 0 %.1f %.1f 6' % (x + ox, y + oy)); time.sleep(2.5)
        _, hp3 = props(h); _, gp3 = props(g)
        moved = dist(hp3[k], base[k])
        if moved > 8:
            pushed = (k, ox, oy, hp3, gp3, moved); break
    if pushed: break
check('a character in contact with a prop shoves it on the host', pushed is not None, ('prop %s from (%d,%d), moved %.0f units' % (pushed[0], pushed[1], pushed[2], pushed[5])) if pushed else 'nothing moved')
if pushed:
    k = pushed[0]; time.sleep(1.5); _, hp4 = props(h); _, gp4 = props(g)
    check('and the guest shows the prop where the host has it', dist(gp4[k], hp4[k]) < 2, 'host %s guest %s' % (tuple(round(v) for v in hp4[k]), tuple(round(v) for v in gp4[k])))
    others = [kk for kk in hp4 if kk in gp4]
    check('every prop agrees at the end', all(dist(hp4[kk], gp4[kk]) < 2 for kk in others), 'worst %.1f' % max(dist(hp4[kk], gp4[kk]) for kk in others))
print('RESULT: %s (%d/%d checks)' % ('PASS' if all(results) else 'FAIL', sum(results), len(results)))
