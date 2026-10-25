"""Team bag test: the host uses up items from the shared bag (the "baguse" pipe command: the game's own
remove of 1, as using an energy drink does). After each use the guest's bag must match the host's
(types, ids, counts), the character inventories must still match, and both games must keep running."""
import subprocess, sys, time
sys.path.insert(0, '.')
import invpeek, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def responding(pid):
    return subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pid],
                          capture_output=True, text=True).stdout.strip() == 'True'


def bag(p):
    return sorted(e[1:] for e in invpeek.bag(p)[1])


def chars(p):
    return {p.u32(c + 0x78): sorted(i[1:4] for i in invpeek.inventory(p, c)[0]) for c in invpeek.humans(p)}


time.sleep(3)
print('  host bag at start:', bag(ph))
check('guest bag matches the host at the start', bag(pg) == bag(ph), str(bag(pg)))
for use in range(3):
    before = bag(ph)
    print('  baguse:', h.cmd('baguse'))
    time.sleep(3)
    hb, gb = bag(ph), bag(pg)
    check('use %d: host bag changed' % (use + 1), hb != before, str(hb))
    check('use %d: guest bag matches the host' % (use + 1), gb == hb, '\n    host %s\n    guest %s' % (hb, gb))
check('character inventories still match', chars(pg) == chars(ph))
check('both games still responding', responding(H) and responding(G))
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
