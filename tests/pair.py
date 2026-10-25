"""Shared two-copy setup for tests: host loads its save, the guest waits for the transferred save,
loads it, player 2 joins. Each step is verified from memory; raises RuntimeError with the step name."""
import os, struct, time
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
GUEST_SAVE = os.path.join(os.path.dirname(R), 'obscure-test2', 'coop_profile', 'remote', 'game0.sav')


def keys(pipe, seq):
    for k in seq:
        if k.startswith('wait'):
            time.sleep(float(k[4:]))
        else:
            bot.tap(pipe, k)
            time.sleep(0.6)


def in_level(p):
    g = p.u32(peek.GAME)
    return bool(g) and p.u32(g + 4) == 2 and bool(p.u32(p.u32(g + 0x4fc) + 0xc))


def plays(p, off):
    g = p.u32(peek.GAME)
    rp = p.u32(g + off) if g else 0
    c = p.u32(rp + 0xc) if rp else 0
    return p.u32(c + 0x78) if c else None


def wait(cond, seconds, step):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if cond():
            return
        time.sleep(0.5)
    raise RuntimeError('setup step failed: ' + step)


def setup(hname, gname):
    hpid = int(open(os.path.join(R, hname + '.pid')).read())
    gpid = int(open(os.path.join(R, gname + '.pid')).read())
    host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
    ph, pg = peek.Proc(hpid), peek.Proc(gpid)
    keys(host, ['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN'])
    wait(lambda: in_level(ph), 30, 'host in level')
    wait(lambda: os.path.exists(GUEST_SAVE), 20, 'guest received the save')
    try:   # 1.0: the guest loads the host's save by itself (PKT_LOAD)
        wait(lambda: in_level(pg), 30, 'guest loaded the save by itself')
    except RuntimeError:
        keys(guest, ['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN'])
        wait(lambda: in_level(pg), 30, 'guest in level')
    time.sleep(2)
    bot.press(guest, 0, 'START', 0.3)
    time.sleep(1.5)
    bot.press(host, 1, 'A', 0.25)   # player 1 on game controller 2 (two-pad settings)
    time.sleep(1.5)
    if plays(ph, 0x500) is None:
        bot.tap(host, 'RETURN')     # player 1 on the keyboard
    wait(lambda: plays(ph, 0x500) is not None, 10, 'player 2 joined')
    wait(lambda: plays(pg, 0x4fc) == plays(ph, 0x500), 10, 'guest plays the player-2 character')
    return hpid, gpid, host, guest, ph, pg
