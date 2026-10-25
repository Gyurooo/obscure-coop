"""Host camera: the shared camera (game+0x24 = followed player slot) must stay on the host's player 1.
1) the camtoggle command (the game's own toggle, as the +0x34 button does) is undone within a second;
2) each remote-pad button is pressed: the camera stays on player 1 (and the button that tried is reported)."""
import sys, time
sys.path.insert(0, '.')
import bot, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


g = ph.u32(0x6ab8e4)
cam = lambda: ph.u32(g + 0x24)
restored = lambda: int(h.cmd('sync').split('cam ')[1].split()[0])
time.sleep(2)
check('camera on player 1 at start', cam() == 0, str(cam()))
r0 = restored()
h.cmd('camtoggle')
time.sleep(1.0)
check('toggle to player 2 is undone', cam() == 0 and restored() > r0, 'cam %d restored %d' % (cam(), restored() - r0))
seen = {}
for b in ['BACK', 'Y', 'X', 'B', 'RB', 'LT_CLICK', 'RT_CLICK', 'UP', 'DOWN', 'LEFT', 'RIGHT']:
    r = restored()
    bot.press(gp, 0, b, 0.2)
    time.sleep(0.8)
    seen[b] = (cam(), restored() - r)
grabbed = [b for b, (c, n) in seen.items() if n]
print('  buttons that took the camera (undone):', grabbed, seen)
check('camera on player 1 after every remote button', all(c == 0 for c, n in seen.values()))
bot.shot(H, 'camera_host.png')
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
