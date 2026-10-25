"""Picture mirror: the host shows an item picture the way a pickup does ("view" test command =
SFLB_10(name, 1, 0, 0)); the guest must show it too, and close it when player 1 closes the host's
with A. Both games must keep running."""
import subprocess, sys, time
sys.path.insert(0, '.')
import bot, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def responding(pid):
    return subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pid],
                          capture_output=True, text=True).stdout.strip() == 'True'


viewer = lambda p: p.u32(p.u32(0x6ab8f0) + 4)
time.sleep(2)
for name in ('vcompass', 'vflash'):
    h.cmd('view ' + name)
    time.sleep(2)
    check('%s: shown on the host' % name, viewer(ph) == 1)
    check('%s: shown on the guest' % name, viewer(pg) == 1)
    bot.shot(G, 'view_guest_%s.png' % name, scale=1)
    bot.press(h, 1, 'A', 0.3)
    time.sleep(2)
    check('%s: closed on both after player 1 pressed A' % name, viewer(ph) == 0 and viewer(pg) == 0, 'host %s guest %s' % (viewer(ph), viewer(pg)))
check('both games still responding', responding(H) and responding(G))
print(' ', gp.cmd('sync').split('views')[1][:20])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
