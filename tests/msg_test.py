"""Message mirror: the host shows a pickup text (the CD item's own text id, +0x110, as the pickup code
does); the guest must replay it once; both keep running."""
import subprocess, sys, time
sys.path.insert(0, '.')
import bot, pair
H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


msgs = lambda: int(gp.cmd('sync').split('msgs ')[1].split()[0])
g = ph.u32(0x6ab8e4)
node, text = ph.u32(ph.u32(g + 0x510) + 0x30), None
while node:
    it = ph.u32(node)
    if ph.u32(it + 4) == 162:
        text = ph.u32(it + 0x110)
    node = ph.u32(node + 4)
print('CD text id', hex(text or 0))
time.sleep(2)
m0 = msgs()
print(h.cmd('msg %x 4000' % text))
time.sleep(1.0)
bot.shot(G, 'msg_guest.png', scale=1)
bot.shot(H, 'msg_host.png', scale=1)
time.sleep(1.0)
check('guest replayed the host\'s message', msgs() - m0 >= 1, '%d new' % (msgs() - m0))
time.sleep(2)
r = [subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % p], capture_output=True, text=True).stdout.strip() for p in (H, G)]
check('both games responding', r == ['True', 'True'])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
