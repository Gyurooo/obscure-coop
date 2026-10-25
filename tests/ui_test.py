"""Inventory menu on the guest's screen: the remote player (guest pad 0 = host player 2) opens the
items menu, scrolls, opens the action menu and closes it. The guest's own player's menu must follow
(mode, selected item, action menu, highlighted action); player 1 opening theirs must not open the
guest's; the bag must not change on either side."""
import struct, subprocess, sys, time
sys.path.insert(0, '.')
import bot, invpeek, pair

H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def menu(p, slot):
    g = p.u32(0x6ab8e4)
    rp = p.u32(g + slot)
    inv, c = p.u32(rp + 0x10), p.u32(rp + 0xc)
    act = p.u32(c + 0x204)
    return dict(mode=p.u32(inv + 0x1c), menu=p.u32(inv + 0x34), action=p.u32(inv + 0x3c) if p.u32(inv + 0x34) else None,
                item=(p.u32(act + 4), p.u32(act + 8)) if act else None)


host_p2 = lambda: menu(ph, 0x500)
guest_me = lambda: menu(pg, 0x4fc)
bag0 = (sorted(invpeek.bag(ph)[1]), sorted(invpeek.bag(pg)[1]))
time.sleep(2)
check('closed on both at start', host_p2()['mode'] == 3 and guest_me()['mode'] == 3, '%s %s' % (host_p2(), guest_me()))
gp.cmd('pad 0 100')
time.sleep(1.0)
check('remote LB: guest menu opens like the host\'s player 2', guest_me()['mode'] == host_p2()['mode'] == 1, '%s %s' % (host_p2(), guest_me()))
same = 0
for k in range(3):
    gp.cmd('pad 0 100 0 0 0 -32000'); time.sleep(0.2); gp.cmd('pad 0 100'); time.sleep(0.6)
    a, b = host_p2(), guest_me()
    same += a['item'] == b['item']
    print('  scroll %d host %s guest %s' % (k, a, b))
check('scrolling: guest shows the same selected item (3 steps)', same == 3)
bot.shot(G, 'ui_guest_open.png', scale=1)
for k in range(4):   # the action menu needs a real item: go to the CD (162)
    if host_p2()['item'] and host_p2()['item'][0] == 162:
        break
    gp.cmd('pad 0 100 0 0 0 -32000'); time.sleep(0.2); gp.cmd('pad 0 100'); time.sleep(0.6)
gp.cmd('pad 0 1100'); time.sleep(0.2); gp.cmd('pad 0 100'); time.sleep(0.8)
a, b = host_p2(), guest_me()
check('remote A: guest shows the action menu with the same action', a['menu'] == b['menu'] == 1 and a['action'] == b['action'], '%s %s' % (a, b))
bot.shot(G, 'ui_guest_action.png', scale=1)
gp.cmd('pad 0 0'); time.sleep(1.0)
a, b = host_p2(), guest_me()
check('release: menu closed on both', a['mode'] == b['mode'] == 3 and not b['menu'], '%s %s' % (a, b))
h.cmd('pad 1 100'); time.sleep(1.0)
check('player 1 opening theirs does not open the guest\'s', menu(ph, 0x4fc)['mode'] == 1 and guest_me()['mode'] == 3, '%s %s' % (menu(ph, 0x4fc), guest_me()))
h.cmd('pad 1 0'); time.sleep(4)
check('bag unchanged on both sides', (sorted(invpeek.bag(ph)[1]), sorted(invpeek.bag(pg)[1])) == bag0)
print(' ', gp.cmd('sync').split('events')[1][:60])
print('RESULT:', 'PASS' if all(results) else 'FAIL', '(%d/%d checks)' % (sum(results), len(results)), flush=True)
