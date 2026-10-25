"""Per-player items test: after the join each player has their own items section; opening and closing
the inventory (LB, the official button) any number of times, together or alone, must never move a
section to the other player. Reads the host's memory; prints PASS/FAIL."""
import sys, time
sys.path.insert(0, '.')
import bot, peek, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


g = ph.u32(peek.GAME)
inv = lambda i: ph.u32(ph.u32(g + 0x4fc + 4 * i) + 0x10)
time.sleep(2)
sec = lambda i: ph.u32(inv(i) + 0x24)
owner_ok = lambda i: sec(i) and ph.u32(sec(i) + 0x10) == inv(i)
start = [sec(0), sec(1)]
check('both players have their own items section', all(start) and start[0] != start[1] and owner_ok(0) and owner_ok(1), str([hex(x) for x in start]))
pads = [(h, 1), (gp, 0)]
moved = 0
wrong_mode = 0
for cycle, who in enumerate(['both', 'p1', 'p2', 'both', 'p2', 'p1', 'both']):
    use = pads if who == 'both' else [pads[0]] if who == 'p1' else [pads[1]]
    for pipe, user in use:
        pipe.cmd('pad %d 100' % user)   # hold LB
    time.sleep(1.0)
    modes = [ph.u32(inv(i) + 0x1c) for i in range(2)]
    exp = [1 if who in ('both', 'p1') else 3, 1 if who in ('both', 'p2') else 3]
    if modes != exp:
        wrong_mode += 1
        print('  cycle %d (%s): modes %s, expected %s' % (cycle, who, modes, exp))
    for pipe, user in use:
        pipe.cmd('pad %d 0' % user)
    time.sleep(1.0)
    now = [sec(0), sec(1)]
    if now != start or not (owner_ok(0) and owner_ok(1)):
        moved += 1
        print('  cycle %d (%s): sections now %s owners ok %s %s' % (cycle, who, [hex(x) for x in now], owner_ok(0), owner_ok(1)))
check('sections never moved between players (7 open/close cycles)', moved == 0, '%d cycles moved' % moved)
check('each LB opened only that player\'s inventory (7 cycles)', wrong_mode == 0, '%d cycles wrong' % wrong_mode)
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
