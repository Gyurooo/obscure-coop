"""Item test: both copies load the save, player 2 joins, host player 1 picks up the CD in the courtyard
with the action key (RETURN); the guest must mark the same item collected and take it out of its world."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bot, peek

R = os.path.dirname(os.path.abspath(__file__))
hpid = int(open(os.path.join(R, sys.argv[1] + '.pid')).read())
gpid = int(open(os.path.join(R, sys.argv[2] + '.pid')).read())
host, guest = bot.Pipe(hpid), bot.Pipe(gpid)
ph, pg = peek.Proc(hpid), peek.Proc(gpid)
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def keys(pipe, seq):
    for k in seq:
        if k.startswith('wait'):
            time.sleep(float(k[4:]))
        else:
            bot.tap(pipe, k)
            time.sleep(0.6)


def items(p):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510) if g else 0
    node = p.u32(mgr + 0x30) if mgr else 0
    out = {}
    while node and len(out) < 400:
        it = p.u32(node)
        if it:
            out[(p.u32(it + 4), p.u32(it + 8))] = dict(it=it, collected=p.u32(it + 0xfc), pos=struct.unpack('<3f', p.read(it + 0xc, 12)))
        node = p.u32(node + 4)
    return out


LOAD = ['wait1', 'DOWN', 'RETURN', 'wait2', 'RETURN', 'wait1', 'LEFT', 'RETURN', 'wait9']
keys(host, LOAD)
keys(guest, LOAD)
bot.press(guest, 0, 'START', 0.3)
time.sleep(1.5)
bot.tap(host, 'RETURN')
time.sleep(2)
hi, gi = items(ph), items(pg)
cd = [k for k in hi if k[0] == 162]
check('the CD exists on both copies, not collected', cd and cd[0] in gi and not hi[cd[0]]['collected'] and not gi[cd[0]]['collected'], str(cd))
key = cd[0]
x, y, z = hi[key]['pos']
host.cmd('teleport %.1f %.1f 3.0 -1 0' % (x + 40, y))   # stand next to it, facing it
time.sleep(1.5)
bot.tap(host, 'RETURN', 0.25)   # action key
time.sleep(2.5)
hi, gi = items(ph), items(pg)
check('host player 1 collected the CD', hi[key]['collected'] == 1)
check('guest marked the same CD collected', gi[key]['collected'] == 1)
gs = guest.cmd('sync')
check('guest removed it from its world', 'items_removed 1' in gs, gs[gs.find('items_applied'):gs.find('items_applied') + 60])
# only collectable kinds carry a collected flag at +0xfc (spawn markers, type 2, use it for something else)
others = [k for k in hi if k != key and k in gi and hi[k]['collected'] in (0, 1) and gi[k]['collected'] in (0, 1)
          and hi[k]['collected'] != gi[k]['collected']]   # +0xfc is a flag only where it reads 0/1 (the padlock keeps a pointer there)
check('no other item differs in collected state', not others, str(others[:5]))
bot.shot(hpid, os.path.join(R, 'item_host.png'))
bot.shot(gpid, os.path.join(R, 'item_guest.png'))
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)))
