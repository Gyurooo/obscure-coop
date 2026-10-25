"""sbreak_test.py <host> <guest> [level]: breakable scene objects (windows in walls, vending machines) broken on the host are broken on the guest."""
import re, sys, time
sys.path.insert(0, '.')
import bot, peek, pair
pair.setup(sys.argv[1], sys.argv[2])
H, G = int(open(sys.argv[1] + '.pid').read()), int(open(sys.argv[2] + '.pid').read())
h, g, ph, pg = bot.Pipe(H), bot.Pipe(G), peek.Proc(H), peek.Proc(G)
results = []
def check(name, ok, detail=''):
    results.append(bool(ok)); print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)
def sb(pipe):
    r = pipe.cmd('sbrk'); return {int(a): int(b) for a, b in re.findall(r' (\d+)=(-?\d+)', r)}
def lvl(p):
    gm = p.u32(peek.GAME); mgr = p.u32(gm + 0x510); return p.read(mgr + 8, 1)[0] if mgr else None
level = int(sys.argv[3]) if len(sys.argv) > 3 else 17
print(h.cmd('level %d 0' % level))
t0 = time.time()
while time.time() - t0 < 60 and not (lvl(ph) == level and lvl(pg) == level): time.sleep(1)
time.sleep(6)
hb, gb = sb(h), sb(g)
print("host", hb); print("guest", gb)
check('both copies list the same breakable objects in the same state', len(hb) >= 1 and hb == gb, '%d objects' % len(hb))
keys = sorted(k for k in hb if hb[k] == 0)   # some start broken, some have no group (-1)
assert keys, 'nothing unbroken to break'
print(h.cmd('sbreakit %d' % keys[0])); time.sleep(3)
hb2, gb2 = sb(h), sb(g)
check('an object broken on the host is broken on the guest', hb2[keys[0]] == 1 and gb2[keys[0]] == 1, 'host %s guest %s' % (hb2[keys[0]], gb2[keys[0]]))
check('the others are untouched, both copies agree', all(hb2[k] == hb[k] for k in hb if k != keys[0]) and hb2 == gb2, 'host %s guest %s' % (hb2, gb2))
print('RESULT: %s (%d/%d checks)' % ('PASS' if all(results) else 'FAIL', sum(results), len(results)))
