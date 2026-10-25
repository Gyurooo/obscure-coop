"""Join test: the guest starts with no save at all; the host loads its save; the host's DLL sends that
save to the guest, which stores it as its co-op slot 1; the guest loads it; player 2 joins; both copies
must be in the same level with mirrored characters."""
import hashlib, math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
L = os.path.dirname(R)
hpid = int(open(os.path.join(R, sys.argv[1] + '.pid')).read())
gpid = int(open(os.path.join(R, sys.argv[2] + '.pid')).read())
host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
ph, pg = peek.Proc(hpid), peek.Proc(gpid)
results = []
HOST_SAVE = os.path.join(L, os.environ.get('HOST_COPY', 'obscure-test'), 'coop_profile', 'remote', 'game0.sav')
GUEST_SAVE = os.path.join(L, 'obscure-test2', 'coop_profile', 'remote', 'game0.sav')


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


def chars(p):
    g = p.u32(peek.GAME)
    node = p.u32(p.u32(g + 0x510) + 0x1c)
    out = {}
    while node:
        c = p.u32(node)
        out[p.u32(c + 0x78)] = struct.unpack('<3f', p.read(c + 0x20, 12))
        node = p.u32(node + 4)
    return out


check('guest starts without a save', not os.path.exists(GUEST_SAVE))
LOAD = ['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN', 'wait9']
keys(host, LOAD)
t0 = time.time()
while time.time() - t0 < 20 and not os.path.exists(GUEST_SAVE):
    time.sleep(0.5)
time.sleep(1)
hs = hashlib.sha256(open(HOST_SAVE, 'rb').read()).hexdigest()
gs = hashlib.sha256(open(GUEST_SAVE, 'rb').read()).hexdigest() if os.path.exists(GUEST_SAVE) else None
check('guest received the host save, byte-identical', gs == hs, 'host %s guest %s' % (hs[:12], (gs or 'none')[:12]))
keys(guest, LOAD)
bot.press(guest, 0, 'START', 0.3)
time.sleep(1.5)
bot.tap(host, 'RETURN')
time.sleep(3)
check('both copies in the same level', level(ph) is not None and level(ph) == level(pg), '%s %s' % (level(ph), level(pg)))
hc, gc = chars(ph), chars(pg)
err = {k: round(math.dist(hc[k], gc[k]), 1) for k in hc if k in gc}
check('characters mirrored', hc and set(hc) == set(gc) and max(err.values()) < 30, str(err))
g = ph.u32(peek.GAME)
check('player 2 joined on the host', bool(ph.u32(ph.u32(g + 0x500) + 0xc)))
s = guest.cmd('sync')
print('guest', s[s.find('save_tx'):s.find('save_tx') + 60])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
