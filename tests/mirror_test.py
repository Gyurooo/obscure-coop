"""Mirror test: host and guest load the same save; the guest's pad (relayed) joins player 2;
bots walk both players; every 0.25 s the human characters' positions are read from both processes
and compared by character id. PASS if the guest tracks the host within the threshold."""
import math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
hpid = int(open(os.path.join(R, sys.argv[1] + '.pid')).read())
gpid = int(open(os.path.join(R, sys.argv[2] + '.pid')).read())
THRESH = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
ph, pg = peek.Proc(hpid), peek.Proc(gpid)
results = []


def check(name, ok, detail=''):
    results.append(ok)
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def keys(pipe, seq):
    for k in seq:
        if k.startswith('wait'):
            time.sleep(float(k[4:]))
        else:
            bot.tap(pipe, k)
            time.sleep(0.6)


def chars(p):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510) if g else 0
    node = p.u32(mgr + 0x1c) if mgr else 0
    out = {}
    while node and len(out) < 16:
        c = p.u32(node)
        if c:
            x, y, z = struct.unpack('<3f', p.read(c + 0x20, 12))
            out[p.u32(c + 0x78)] = (x, y, z, c, p.u32(c + 0x60), p.u32(c + 0xf0))
        node = p.u32(node + 4)
    return out


LOAD = ['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN', 'wait9']
keys(host, LOAD)
keys(guest, LOAD)
hc, gc = chars(ph), chars(pg)
check('both copies loaded with the same characters', set(hc) == set(gc) and len(hc) > 0, 'host %s guest %s' % (sorted(hc), sorted(gc)))

bot.press(guest, 0, 'START', 0.3)   # START on player 2's pad raises "Player 2 joins the game" (run 16)
time.sleep(1.5)
bot.tap(host, 'RETURN')
time.sleep(2)
g = ph.u32(peek.GAME)
p2char = ph.u32(ph.u32(g + 0x500) + 0xc)
check('player 2 joined on the host', bool(p2char))
gg = pg.u32(peek.GAME)
gp1 = pg.u32(pg.u32(gg + 0x4fc) + 0xc)
p2id = ph.u32(p2char + 0x78) if p2char else None
check('guest player 1 controls the guest-played character', gp1 and pg.u32(gp1 + 0x78) == p2id, 'id %s' % p2id)

# Walk: host player 1 with W, guest's pad stick for player 2, in a few legs; sample errors.
start = {cid: v[:3] for cid, v in chars(ph).items()}
p1id = ph.u32(ph.u32(ph.u32(g + 0x4fc) + 0xc) + 0x78)
worst = {}
samples = 0
anim_same = anim_total = 0
legs = [('w', (0, 30000)), ('d', (30000, 0)), ('s', (0, -30000)), ('a', (-30000, 0))]
DIK = dict(w=0x11, a=0x1E, s=0x1F, d=0x20)
for key, (lx, ly) in legs:
    host.cmd('key %x 1' % DIK[key])
    guest.cmd('pad 0 0 %d %d 0 0 0 0' % (lx, ly))
    t0 = time.time()
    while time.time() - t0 < 1.5:
        hc, gc = chars(ph), chars(pg)
        for cid, (x, y, z, _, st, an) in hc.items():
            if cid in gc:
                d = math.dist((x, y, z), gc[cid][:3])
                worst[cid] = max(worst.get(cid, 0), d)
                if cid in (p1id, p2id):
                    anim_total += 1
                    anim_same += (st, an) == gc[cid][4:6]
        samples += 1
        time.sleep(0.25)
    host.cmd('key %x 0' % DIK[key])
    guest.cmd('pad 0 0')
time.sleep(1)
hc, gc = chars(ph), chars(pg)
final = {cid: round(math.dist(hc[cid][:3], gc[cid][:3]), 1) for cid in hc if cid in gc}
moved = {cid: round(math.dist(start[cid], hc[cid][:3]), 1) for cid in hc if cid in start}
print('distance moved on the host per id', moved)
check('host player 1 moved (> 100)', moved.get(p1id, 0) > 100, 'id %s' % p1id)
check('host player 2 moved by the guest pad (> 100)', moved.get(p2id, 0) > 100, 'id %s' % p2id)
bot.shot(hpid, os.path.join(R, 'mirror_host.png'))
bot.shot(gpid, os.path.join(R, 'mirror_guest.png'))
print('samples', samples, 'worst error per id', {k: round(v, 1) for k, v in worst.items()}, 'final error', final)
check('positions match after the walk (final error < %.0f)' % THRESH, final and max(final.values()) < THRESH, str(final))
check('positions track during the walk (worst error < %.0f)' % (THRESH * 4), worst and max(worst.values()) < THRESH * 4, str({k: round(v) for k, v in worst.items()}))
print('state/anim agreement %d/%d' % (anim_same, anim_total))
check('movement state and animation set agree (>= 80%)', anim_total and anim_same >= 0.8 * anim_total, '%d/%d' % (anim_same, anim_total))
print('host', host.cmd('net'))
print('guest', guest.cmd('net'))
print('guest', guest.cmd('sync')[:300])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
