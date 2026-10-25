"""Player 1 dies (hurtp 0 -500 = the game's AddLife). The host swaps the real players (survivor = slot 1).
The guest must keep playing its own living character, never switch onto the dead one, keep running,
and still move when the remote pad is used."""
import struct, subprocess, sys, time
sys.path.insert(0, '.')
import bot, invpeek, pair, peek

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def responding(pid):
    return subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pid],
                          capture_output=True, text=True).stdout.strip() == 'True'


def pos(p, cid):
    for c in invpeek.humans(p):
        if p.u32(c + 0x78) == cid:
            return struct.unpack('<2f', p.read(c + 0x20, 8))


p1, p2 = pair.plays(ph, 0x4fc), pair.plays(ph, 0x500)
print(' host slots before: P1 char', p1, 'P2 char', p2, '| guest plays', pair.plays(pg, 0x4fc))
print(h.cmd('hurtp 0 -500'))
time.sleep(8)
g = ph.u32(peek.GAME)
check('host got the pause menu after the death', ph.u32(g + 0x60c) == 1 and 'pause: opened' in open('../obscure-test3/obscoop.log', errors='replace').read())
bot.press(h, 1, 'A', 0.2); time.sleep(2)   # RESUME
check('RESUME closes it', ph.u32(g + 0x60c) == 0)
print(' host slots after: slot1 char', pair.plays(ph, 0x4fc), 'slot2 char', pair.plays(ph, 0x500), '| guest plays', pair.plays(pg, 0x4fc))
log = open('../obscure-test2/obscoop.log', errors='replace').read()
hlog = open('../obscure-test3/obscoop.log', errors='replace').read()
check('host: the game swapped the players', 'swapped the players' in hlog and pair.plays(ph, 0x4fc) == p2)
check('guest still plays its own character', pair.plays(pg, 0x4fc) == p2)
check('guest never switched onto the dead character', 'switch to character id %d' % p1 not in log)
check('both games responding', responding(H) and responding(G))
a = pos(ph, p2)
gp.cmd('pad 0 0 0 32000'); time.sleep(2); gp.cmd('pad 0 0'); time.sleep(2)
b, gb = pos(ph, p2), pos(pg, p2)
check('remote pad still moves the guest\'s character', abs(b[0] - a[0]) + abs(b[1] - a[1]) > 50, '%s -> %s' % (a, b))
check('guest shows it where the host has it', abs(b[0] - gb[0]) + abs(b[1] - gb[1]) < 30)
bot.shot(H, 'death_host.png'); bot.shot(G, 'death_guest.png')
time.sleep(10)
check('still responding 10 s later', responding(H) and responding(G))
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
