"""Player 1 dies, then the host loads the save (loadslot 0 = the load menu's two steps, or the real
pause menu with --menu). The guest must reload the same save by itself, both copies must keep running,
the dead character must be alive again on both, and player 2 must still be the guest's. Done twice."""
import struct, subprocess, sys, time
sys.path.insert(0, '.')
import bot, invpeek, pair, peek

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
MENU = '--menu' in sys.argv
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def responding(pid):
    return subprocess.run(['powershell.exe', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pid],
                          capture_output=True, text=True).stdout.strip() == 'True'


def chars(p):
    out = {}
    for c in invpeek.humans(p):
        out[p.u32(c + 0x78)] = struct.unpack('<3f', p.read(c + 0x20, 12)) + struct.unpack('<f', p.read(c + 0x54, 4))
    return out


def log(copy):
    return open('../%s/obscoop.log' % copy, errors='replace').read()


def mode(p):
    g = p.u32(peek.GAME)
    return (p.u32(g + 4), p.u32(g + 8)) if g else None


p1, p2 = pair.plays(ph, 0x4fc), pair.plays(ph, 0x500)
print(' before: host P1', p1, 'P2', p2, '| guest plays', pair.plays(pg, 0x4fc), '| host chars', chars(ph))
for rnd in (1, 2):
    print(h.cmd('hurtp 0 -500'))
    time.sleep(1)
    dead = chars(ph).get(p1)
    print(' round %d after death: host P1 char %s hp %s' % (rnd, p1, dead and dead[3]))
    hl0, gl0 = log('obscure-test3').count('telling the guest to load'), log('obscure-test2').count('guest loads the host')
    if MENU:
        n0 = log('obscure-test3').count('pause: opened')
        try:   # 3 s after the death the host gets the pause menu by itself
            pair.wait(lambda: log('obscure-test3').count('pause: opened') > n0, 10, 'pause menu opened after the death')
        except RuntimeError as e:
            print(' ', e)
        time.sleep(1.5)
        bot.shot(H, 'load%d_pause.png' % rnd)
        check('r%d pause menu came up for the host' % rnd, log('obscure-test3').count('pause: opened') > n0)
        for b, w in (('DOWN', 0.8), ('A', 2.5), ('A', 1.5), ('LEFT', 0.8), ('A', 0.5)):   # LOAD > 01 > YES
            bot.press(h, 1, b, 0.2); time.sleep(w)
    else:
        print(h.cmd('loadslot 0'))
    try:
        pair.wait(lambda: log('obscure-test2').count('guest loads the host') > gl0, 30, 'guest started loading')
        time.sleep(3)
        pair.wait(lambda: pair.in_level(ph) and pair.in_level(pg), 60, 'both in level again')
    except RuntimeError as e:
        print(' ', e, '| modes host', mode(ph), 'guest', mode(pg))
    time.sleep(4)
    hc, gc = chars(ph), chars(pg)
    print(' round %d after load: host %s | guest %s' % (rnd, hc, gc))
    check('r%d host told the guest to load' % rnd, log('obscure-test3').count('telling the guest to load') > hl0)
    check('r%d guest loaded the save' % rnd, log('obscure-test2').count('guest loads the host') > gl0)
    check('r%d both in a level' % rnd, pair.in_level(ph) and pair.in_level(pg), 'modes %s %s' % (mode(ph), mode(pg)))
    check('r%d dead character alive again on both' % rnd, hc.get(p1, (0,) * 4)[3] > 0 and gc.get(p1, (0,) * 4)[3] > 0)
    same = p1 in hc and p1 in gc and abs(hc[p1][0] - gc[p1][0]) + abs(hc[p1][1] - gc[p1][1]) < 30
    check('r%d P1 character at the same place on both' % rnd, same)
    print(' round %d host slots: P1 %s P2 %s | guest plays %s' % (rnd, pair.plays(ph, 0x4fc), pair.plays(ph, 0x500), pair.plays(pg, 0x4fc)))
    if pair.plays(ph, 0x500) is None:   # the load dropped player 2: rejoin the way a player would
        bot.press(gp, 0, 'START', 0.3); time.sleep(1.5)
        bot.press(h, 1, 'A', 0.25); time.sleep(1.5)
        if pair.plays(ph, 0x500) is None:
            bot.tap(h, 'RETURN'); time.sleep(2)
        print(' round %d rejoin: host P2 %s' % (rnd, pair.plays(ph, 0x500)))
    try:
        pair.wait(lambda: pair.plays(ph, 0x500) is not None and pair.plays(pg, 0x4fc) == pair.plays(ph, 0x500), 15, 'p2')
    except RuntimeError:
        pass
    c2 = pair.plays(ph, 0x500)
    check('r%d player 2 is the guest\'s again' % rnd, c2 is not None and pair.plays(pg, 0x4fc) == c2)
    if c2 is not None:
        a = chars(ph).get(c2)
        gp.cmd('pad 0 0 0 32000'); time.sleep(2); gp.cmd('pad 0 0'); time.sleep(2)
        b, gb = chars(ph).get(c2), chars(pg).get(c2)
        check('r%d remote pad moves the guest\'s character' % rnd, a and b and abs(b[0] - a[0]) + abs(b[1] - a[1]) > 50, '%s -> %s' % (a, b))
        check('r%d guest shows it where the host has it' % rnd, b and gb and abs(b[0] - gb[0]) + abs(b[1] - gb[1]) < 30)
    bot.shot(H, 'load%d_host.png' % rnd); bot.shot(G, 'load%d_guest.png' % rnd)
    check('r%d both responding' % rnd, responding(H) and responding(G))
    p1, p2 = pair.plays(ph, 0x4fc), pair.plays(ph, 0x500)
time.sleep(10)
check('still responding 10 s later', responding(H) and responding(G))
print(' host sync:', h.cmd('sync')[:400])
print(' guest sync:', gp.cmd('sync')[:400])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
