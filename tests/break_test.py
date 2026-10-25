"""break_test.py <host> <guest>: breakable windows (item type) broken on the host are broken on the guest. Level 71 has 5."""
import re, sys, time
sys.path.insert(0, '.')
import bot, peek, pair
pair.setup(sys.argv[1], sys.argv[2])
H, G = int(open(sys.argv[1] + '.pid').read()), int(open(sys.argv[2] + '.pid').read())
h, g, ph, pg = bot.Pipe(H), bot.Pipe(G), peek.Proc(H), peek.Proc(G)
results = []
def check(name, ok, detail=''):
    results.append(bool(ok)); print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)
def brk(pipe):
    r = pipe.cmd('brk'); out = {}
    for m in re.finditer(r'(\d+)/(\d+) v([0-9a-f]+) b(\d+) t(\d+)', r): out[(int(m.group(1)), int(m.group(2)))] = (int(m.group(4)), int(m.group(5)))
    return out
def lvl(p):
    gm = p.u32(peek.GAME); mgr = p.u32(gm + 0x510); return p.read(mgr + 8, 1)[0] if mgr else None
level = int(sys.argv[3]) if len(sys.argv) > 3 else 71
print(h.cmd('level %d 0' % level))
t0 = time.time()
while time.time() - t0 < 60 and not (lvl(ph) == level and lvl(pg) == level): time.sleep(1)
time.sleep(6)
hb, gb = brk(h), brk(g)
print('host', hb); print('guest', gb)
check('both copies list the same breakable items, none broken', len(hb) >= 2 and set(hb) == set(gb) and not any(v[0] for v in hb.values()), '%d items' % len(hb))
keys = sorted(hb)
print(h.cmd('breakit %d %d' % keys[0])); time.sleep(3)
hb2, gb2 = brk(h), brk(g)
check('a window broken on the host (animated) is broken on the guest', hb2[keys[0]][0] == 1 and gb2[keys[0]][0] == 1, 'host %s guest %s' % (hb2[keys[0]], gb2[keys[0]]))
others = [k for k in keys[1:]]
check('the other windows are untouched on both', all(hb2[k][0] == 0 and gb2[k][0] == 0 for k in others))
if len(keys) > 1:
    print(h.cmd('breakit %d %d 1' % keys[1])); time.sleep(3)
    hb3, gb3 = brk(h), brk(g)
    check('an instant break on the host is broken on the guest too', hb3[keys[1]][0] == 1 and gb3[keys[1]][0] == 1, 'host %s guest %s' % (hb3[keys[1]], gb3[keys[1]]))
print('RESULT: %s (%d/%d checks)' % ('PASS' if all(results) else 'FAIL', sum(results), len(results)))
