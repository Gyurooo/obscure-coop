"""Watchdog test: stall the guest's main thread for 8 s on purpose; the guest log must show the
watchdog samples, the character states and the recovery line."""
import sys, time
sys.path.insert(0, '.')
import pair
H, G, h, gp, ph, pg = pair.setup(sys.argv[1], sys.argv[2])
log = '../obscure-test2/obscoop.log'
print(gp.cmd('stall 8000'))
time.sleep(12)
lines = [l for l in open(log, errors='replace') if 'watchdog' in l]
for l in lines:
    print(' ', l.strip()[:200])
ok = [any('watchdog 1:' in l and '0x' in l for l in lines), any('watchdog 2:' in l for l in lines),
      any('character' in l and 'state' in l for l in lines), any('running again' in l for l in lines)]
for name, v in zip(['first sample', 'second sample', 'character states', 'recovery line'], ok):
    print('%s %s' % ('PASS' if v else 'FAIL', name))
print('RESULT:', 'PASS' if all(ok) else 'FAIL', '(%d/%d checks)' % (sum(ok), len(ok)), flush=True)
