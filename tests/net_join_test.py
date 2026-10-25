"""Two-instance input-relay test: host loads the save, guest's pad (relayed over the network) makes
player 2 join and walk. Prints PASS/FAIL lines from host memory, never from screenshots alone."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
hpid = int(open(os.path.join(R, sys.argv[1] + '.pid')).read())
gpid = int(open(os.path.join(R, sys.argv[2] + '.pid')).read())
host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
pr = peek.Proc(hpid)
results = []


def check(name, ok, detail=''):
    results.append(ok)
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail))


def seq_host(keys):
    for k in keys:
        if k.startswith('wait'):
            time.sleep(float(k[4:]))
        else:
            bot.tap(host, k)
            time.sleep(0.6)


seq_host(['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN', 'wait9'])
g = pr.u32(peek.GAME)
rp1 = pr.u32(g + 0x500)
check('save loaded, player 1 has a character', bool(pr.u32(pr.u32(g + 0x4fc) + 0xc)))
check('player 2 empty before join', pr.u32(rp1 + 0xc) == 0)

# Guest presses buttons on its own pad; the host only sees them through the network.
for b in ['A', 'START', 'A', 'START']:
    bot.press(guest, 0, b, 0.3)
    time.sleep(0.8)
bot.shot(hpid, os.path.join(R, 'net_prompt.png'))
bot.tap(host, 'RETURN')   # player 1 confirms "take control of your teammate"
time.sleep(2)
ch = pr.u32(rp1 + 0xc)
ctl = pr.u32(rp1 + 0x84)
ctype = pr.u32(pr.u32(ctl) + 4) if ctl else None
check('player 2 joined through the network', bool(ch) and ctype == 3, 'char %#x controller type %s' % (ch or 0, ctype))

# Find player 2's position: snapshot the character object, walk, diff floats.
before = pr.read(ch, 0x1000) if ch else b''
bot.shot(hpid, os.path.join(R, 'net_before_walk.png'))
guest.cmd('pad 0 0 0 -32000 0 0 0 0')
time.sleep(1.5)
guest.cmd('pad 0 0')
time.sleep(0.5)
after = pr.read(ch, 0x1000) if ch else b''
bot.shot(hpid, os.path.join(R, 'net_after_walk.png'))
moved = []
for o in range(0, min(len(before), len(after)) - 3, 4):
    a, b = struct.unpack_from('<f', before, o)[0], struct.unpack_from('<f', after, o)[0]
    if before[o:o + 4] != after[o:o + 4] and abs(a) < 1e5 and abs(b) < 1e5 and abs(a - b) > 5:
        moved.append((hex(o), round(a, 1), round(b, 1)))
print('character floats that changed by more than 5:', moved[:12])
print('host', host.cmd('net'))
print('guest', guest.cmd('net'))
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
