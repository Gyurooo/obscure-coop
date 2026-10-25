"""Level and story-flag test: both copies load the save, player 2 joins, the story flags must match,
then the host requests another level and the guest must follow it (and only it)."""
import math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
hpid = int(open(os.path.join(R, sys.argv[1] + '.pid')).read())
gpid = int(open(os.path.join(R, sys.argv[2] + '.pid')).read())
TARGET = int(sys.argv[3]) if len(sys.argv) > 3 else 230   # WMORD
host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
ph, pg = peek.Proc(hpid), peek.Proc(gpid)
results = []


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


def flags(p):
    g = p.u32(peek.GAME)
    a = p.u32(g + 0xa24) if g else 0
    return p.read(a, 161) if a else None


def chars(p, off=0x1c):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510) if g else 0
    node = p.u32(mgr + off) if mgr else 0
    out = {}
    while node and len(out) < 64:
        c = p.u32(node)
        if c:
            out[p.u32(c + 0x78)] = struct.unpack('<3f', p.read(c + 0x20, 12)) + (c,)
        node = p.u32(node + 4)
    return out


LOAD = ['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN', 'wait9']
keys(host, LOAD)
keys(guest, LOAD)
bot.press(guest, 0, 'START', 0.3)
time.sleep(1.5)
bot.tap(host, 'RETURN')
time.sleep(3)
start_level = level(ph)
check('both in the same level after loading', start_level is not None and start_level == level(pg), '%s %s' % (start_level, level(pg)))
fh, fg = flags(ph), flags(pg)
diff = [i for i in range(161) if fh and fg and fh[i] != fg[i]]
check('story flags identical on host and guest', fh and fg and not diff, 'differ at %s' % diff)

print('host', host.cmd('level %d 0' % TARGET))
t0 = time.time()
while time.time() - t0 < 40 and not (level(ph) == TARGET and level(pg) == TARGET and chars(ph) and chars(pg)):
    time.sleep(0.5)
time.sleep(4)
print('after %.0f s: host level %s guest level %s' % (time.time() - t0, level(ph), level(pg)))
check('host changed level', level(ph) == TARGET)
check('guest followed the host into the level', level(pg) == TARGET)
gs = guest.cmd('sync')
check('guest applied the level from the host', 'levels_applied 1' in gs or 'levels_applied 2' in gs, gs[:160])
hc, gc = chars(ph), chars(pg)
err = {cid: round(math.dist(hc[cid][:3], gc[cid][:3]), 1) for cid in hc if cid in gc}
check('human characters mirrored in the new level', hc and set(hc) == set(gc) and max(err.values()) < 30, 'ids %s %s err %s' % (sorted(hc), sorted(gc), err))
fh, fg = flags(ph), flags(pg)
diff = [i for i in range(161) if fh and fg and fh[i] != fg[i]]
check('story flags identical after the level change', fh and fg and not diff, 'differ at %s' % diff)
ph_e, pg_e = chars(ph, 0x58), chars(pg, 0x58)
print('enemy list mgr+0x58: host %d entries %s, guest %d entries' % (len(ph_e), sorted(ph_e)[:12], len(pg_e)))
bot.shot(hpid, os.path.join(R, 'level_host.png'))
bot.shot(gpid, os.path.join(R, 'level_guest.png'))
print('host', host.cmd('sync')[:200])
print('guest', gs[:200])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
