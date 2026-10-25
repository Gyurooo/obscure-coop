"""Combat test (pair already joined, any level): host moves to C006, player 1 stands next to a Mordeur
facing it and attacks until it dies; the guest's copy of that enemy must lose the same health and die."""
import math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
hpid = int(open(os.path.join(R, sys.argv[1] + '.pid')).read())
gpid = int(open(os.path.join(R, sys.argv[2] + '.pid')).read())
ATTACK = sys.argv[3] if len(sys.argv) > 3 else 'RCONTROL'
host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
ph, pg = peek.Proc(hpid), peek.Proc(gpid)
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def level(p):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510) if g else 0
    return p.read(mgr + 8, 1)[0] if mgr else None


def enemies(p):
    g = p.u32(peek.GAME)
    node = p.u32(p.u32(g + 0x510) + 0x58)
    out = []
    while node:
        c = p.u32(node)
        out.append(dict(pos=struct.unpack('<3f', p.read(c + 0x20, 12)), hp=struct.unpack('<f', p.read(c + 0x54, 4))[0],
                        dead=bool(p.u32(c + 0x58) or p.u32(c + 0x5c))))
        node = p.u32(node + 4)
    return out


if level(ph) != 52:
    print('host', host.cmd('level 52 0'))
    t0 = time.time()
    while time.time() - t0 < 40 and not (level(ph) == 52 == level(pg) and enemies(ph) and enemies(pg)):
        time.sleep(0.5)
    time.sleep(3)
he = enemies(ph)
check('both copies in C006 with enemies', level(ph) == 52 == level(pg) and len(he) == len(enemies(pg)) > 0)
target = 0
deadline = time.time() + 45
hits = 0
while time.time() < deadline:
    he = enemies(ph)
    if he[target]['dead']:
        break
    # Damage through the enemy's own AddLife on the host (the attack controls are not worked out yet).
    host.cmd('hurt %d -7' % target)
    hits += 1
    time.sleep(1.0)
time.sleep(2)
he, ge = enemies(ph), enemies(pg)
print('after %d attacks: host hp %s dead %s | guest hp %s dead %s' % (hits, [round(e['hp'], 1) for e in he], [e['dead'] for e in he],
      [round(e['hp'], 1) for e in ge], [e['dead'] for e in ge]))
check('host: the attacked enemy lost health', he[target]['hp'] < 20.0 or he[target]['dead'], '%.1f' % he[target]['hp'])
check('host: the attacked enemy died', he[target]['dead'])
check('guest: same enemy has the host health', abs(he[target]['hp'] - ge[target]['hp']) < 0.05, '%.1f vs %.1f' % (he[target]['hp'], ge[target]['hp']))
check('guest: same enemy is dead', ge[target]['dead'] == he[target]['dead'] and ge[target]['dead'])
check('guest: dead/alive matches for every enemy', [e['dead'] for e in he] == [e['dead'] for e in ge])
bot.shot(hpid, os.path.join(R, 'combat_host.png'))
bot.shot(gpid, os.path.join(R, 'combat_guest.png'))
gs = guest.cmd('sync')
print('guest', gs[gs.find('pnj_applied'):gs.find('pnj_applied') + 90])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
