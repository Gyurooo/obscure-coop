"""Enemy test: both copies load the save, player 2 joins, the host moves both to C006 (six Mordeurs),
host player 1 walks into them; enemy positions and health on the guest must follow the host."""
import math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
hpid = int(open(os.path.join(R, sys.argv[1] + '.pid')).read())
gpid = int(open(os.path.join(R, sys.argv[2] + '.pid')).read())
LEVEL = int(sys.argv[3]) if len(sys.argv) > 3 else 52   # C006
host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
ph, pg = peek.Proc(hpid), peek.Proc(gpid)
results = []
DIK = dict(w=0x11, a=0x1E, s=0x1F, d=0x20)


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def keys(pipe, seq):
    for k in seq:
        if k.startswith('wait'):
            time.sleep(float(k[4:]))
        else:
            bot.tap(pipe, k)
            time.sleep(0.6)


def level(p):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510) if g else 0
    return p.read(mgr + 8, 1)[0] if mgr else None


def ents(p, off):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510) if g else 0
    node = p.u32(mgr + off) if mgr else 0
    out = []
    while node and len(out) < 64:
        c = p.u32(node)
        if c:
            x, y, z = struct.unpack('<3f', p.read(c + 0x20, 12))
            hp = struct.unpack('<f', p.read(c + 0x54, 4))[0]
            out.append(dict(c=c, id=p.u32(c + 0x78), pos=(x, y, z), hp=hp, state=p.u32(c + 0x60),
                            dead=bool(p.u32(c + 0x58) or p.u32(c + 0x5c))))
        node = p.u32(node + 4)
    return out


def p1(p):
    g = p.u32(peek.GAME)
    c = p.u32(p.u32(g + 0x4fc) + 0xc)
    return struct.unpack('<2f', p.read(c + 0x20, 8))


LOAD = ['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN', 'wait9']
keys(host, LOAD)
keys(guest, LOAD)
bot.press(guest, 0, 'START', 0.3)
time.sleep(1.5)
bot.tap(host, 'RETURN')
time.sleep(2)
print('host', host.cmd('level %d 0' % LEVEL))
t0 = time.time()
while time.time() - t0 < 40 and not (level(ph) == LEVEL and level(pg) == LEVEL and ents(ph, 0x58) and ents(pg, 0x58)):
    time.sleep(0.5)
time.sleep(3)
he, ge = ents(ph, 0x58), ents(pg, 0x58)
check('both copies in the level with the same number of enemies', level(ph) == LEVEL == level(pg) and len(he) == len(ge) > 0,
      'host %d guest %d' % (len(he), len(ge)))
start = [e['pos'] for e in he]

# Probe which world direction W and D move player 1 (camera-relative controls).
probe = {}
for k in ('w', 'd'):
    a = p1(ph)
    host.cmd('key %x 1' % DIK[k]); time.sleep(0.5); host.cmd('key %x 0' % DIK[k]); time.sleep(0.4)
    b = p1(ph)
    probe[k] = (b[0] - a[0], b[1] - a[1])
print('probe W', [round(v) for v in probe['w']], 'D', [round(v) for v in probe['d']])


def steer_to(target, seconds):
    t1 = time.time()
    held = set()
    worst = 0.0
    samples = []
    while time.time() - t1 < seconds:
        x, y = p1(ph)
        dx, dy = target[0] - x, target[1] - y
        want = set()
        for k, opp in (('w', 's'), ('d', 'a')):
            v = probe[k]
            dot = (dx * v[0] + dy * v[1]) / (math.hypot(*v) or 1)
            if dot > 40:
                want.add(k)
            elif dot < -40:
                want.add(opp)
        for k in held - want:
            host.cmd('key %x 0' % DIK[k])
        for k in want - held:
            host.cmd('key %x 1' % DIK[k])
        held = want
        hh, gg = ents(ph, 0x58), ents(pg, 0x58)
        if len(hh) == len(gg):
            errs = [math.dist(a['pos'], b['pos']) for a, b in zip(hh, gg)]
            worst = max([worst] + errs)
            samples.append((max(errs) if errs else 0, [a['hp'] for a in hh] == [round(b['hp'], 3) for b in gg] or
                            all(abs(a['hp'] - b['hp']) < 0.05 for a, b in zip(hh, gg))))
        time.sleep(0.2)
    for k in held:
        host.cmd('key %x 0' % DIK[k])
    return worst, samples


# Walk at the nearest enemy and stay among them for a while.
x, y = p1(ph)
near = min(he, key=lambda e: math.dist((x, y), e['pos'][:2]))
worst, samples = steer_to(near['pos'][:2], 12)
he, ge = ents(ph, 0x58), ents(pg, 0x58)
moved = [round(math.dist(a, b['pos'])) for a, b in zip(start, he)]
print('enemies moved on host:', moved, 'states', [e['state'] for e in he], 'hp', [round(e['hp'], 1) for e in he])
print('guest states', [e['state'] for e in ge], 'hp', [round(e['hp'], 1) for e in ge])
check('at least one enemy became active on the host (moved > 50)', any(m > 50 for m in moved), str(moved))
errs = [round(math.dist(a['pos'], b['pos']), 1) for a, b in zip(he, ge)]
check('enemy positions on the guest follow the host (final max error < 40)', errs and max(errs) < 40, str(errs))
check('enemy positions tracked during the fight (worst < 160)', worst < 160, '%.1f' % worst)
hp_ok = sum(1 for e, h in samples if h)
check('enemy health matches host (>= 90%% of samples)', samples and hp_ok >= 0.9 * len(samples), '%d/%d' % (hp_ok, len(samples)))
hh, gh = ents(ph, 0x1c), ents(pg, 0x1c)
hp_h = {e['id']: round(e['hp'], 1) for e in hh}
hp_g = {e['id']: round(e['hp'], 1) for e in gh}
check('player health matches host', hp_h == hp_g, 'host %s guest %s' % (hp_h, hp_g))
bot.shot(hpid, os.path.join(R, 'enemy_host.png'))
bot.shot(gpid, os.path.join(R, 'enemy_guest.png'))
print('guest', guest.cmd('sync')[:330])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
