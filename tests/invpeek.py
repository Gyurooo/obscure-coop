"""Character inventories (CPlayerInventory at character +0x128): 8 lists {head, tail, iter, count, flag}
from +0x10, nodes {item, next}; item +4 type, +8 id, +0x3c count. In hand +0xd8, active item +0xdc."""
import struct, sys
sys.path.insert(0, '.')
import peek


def humans(p):
    g = p.u32(peek.GAME)
    mgr = p.u32(g + 0x510) if g else 0
    node = p.u32(mgr + 0x1c) if mgr else 0
    out = []
    while node:
        out.append(p.u32(node))
        node = p.u32(node + 4)
    return out


def inventory(p, c):
    inv = c + 0x128
    items = []
    for k in range(8):
        node = p.u32(inv + 0x10 + 0x14 * k)
        while node:
            it = p.u32(node)
            items.append((k, p.u32(it + 4), p.u32(it + 8), p.u32(it + 0x3c), p.u32(it)))
            node = p.u32(node + 4)
    hand, act = p.u32(inv + 0xd8), p.u32(inv + 0xdc)
    return items, (p.u32(hand + 8) if hand else None), (p.u32(act + 8) if act else None)


def show(p, label):
    for c in humans(p):
        items, hand, act = inventory(p, c)
        print('%s char %d: hand %s active %s items %s' % (label, p.u32(c + 0x78), hand, act,
              ' '.join('L%d:t%d/id%d x%d' % i[:4] for i in items)))


if __name__ == '__main__':
    for pid in sys.argv[1:]:
        show(peek.Proc(int(pid)), pid)


def bag(p):
    """Team items (categories 8..17): lists at *(*(inv+4) + 0x7bc) + 0x14 * category (FUN_00448f30)."""
    c = humans(p)[0]
    owner = p.u32(c + 0x128 + 4)
    arr = p.u32(owner + 0x7bc)
    out = []
    for k in range(8, 0x12):
        node = p.u32(arr + 0x14 * k)
        while node:
            it = p.u32(node)
            out.append((k, p.u32(it + 4), p.u32(it + 8), p.u32(it + 0x3c)))
            node = p.u32(node + 4)
    return hex(owner), out
