"""Weapon give test: on the host, player 1 gives the weapon in hand to player 2 (the game's own Give
calls through the "give" pipe command), then player 2 gives it back. After each give the guest's
character inventories and weapons in hand must match the host's, and both games must keep running."""
import subprocess, sys, time
sys.path.insert(0, '.')
import bot, invpeek, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def responding(pid):
    return subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pid],
                          capture_output=True, text=True).stdout.strip() == 'True'


def view(p):
    out = {}
    for c in invpeek.humans(p):
        items, hand, _ = invpeek.inventory(p, c)
        out[p.u32(c + 0x78)] = (sorted((t, i, n) for _, t, i, n, _ in items), hand)
    return out


def weapons(v, cid):
    return [(t, i) for t, i, _ in v[cid][0] if t != 117]


p1, p2 = pair.plays(ph, 0x4fc), pair.plays(ph, 0x500)
time.sleep(3)
start = view(ph)
check('guest inventories match the host at the start', view(pg) == start, '')
print('  start:', start)
for step, (a, b, giver, taker) in enumerate([(0, 1, p1, p2), (1, 0, p2, p1)]):
    before = view(ph)
    print('  give %d -> %d:' % (a, b), h.cmd('give %d %d' % (a, b)))
    time.sleep(3)
    hv, gv = view(ph), view(pg)
    moved = set(weapons(before, giver)) - set(weapons(hv, giver))
    check('host: player %d\'s weapon moved to player %d' % (a + 1, b + 1), moved and moved <= set(weapons(hv, taker)), str(moved))
    check('guest: inventories match the host after the give', gv == hv, '\n    host %s\n    guest %s' % (hv, gv))
    check('both games still responding', responding(H) and responding(G))
print(h.cmd('sync').split(' | ')[0][-80:])
print(gp.cmd('sync').split(' | ')[0][-80:])
bot.shot(G, 'give_guest.png')
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
