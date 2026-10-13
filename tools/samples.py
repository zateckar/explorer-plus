"""Sum up the UI thread's sampled call stacks (EXPLORERPLUS_SAMPLE, see src/perf.cpp).

Usage: python -I tools/samples.py <samples file> <explorer-plus.debug> [thread] [from_ms] [to_ms] [top]

Prints, for the samples of 	hread (ui: the default; enum: the listing worker) between from_ms
and to_ms:
  inclusive  the app's functions by the time spent in them and below (Windows' code included)
  self       where the time went: the innermost app function and the module running then
Samples are ~0.5 ms apart, so a count of 2 is about a millisecond.
"""
import sys, subprocess, collections

path, debug = sys.argv[1], sys.argv[2]
thread = sys.argv[3] if len(sys.argv) > 3 else 'ui'
lo = float(sys.argv[4]) if len(sys.argv) > 4 else 0
hi = float(sys.argv[5]) if len(sys.argv) > 5 else 1e12
top = int(sys.argv[6]) if len(sys.argv) > 6 else 40

samples = []
for line in open(path, encoding='ascii', errors='replace'):
    parts = line.split()
    if not parts:
        continue
    t = float(parts[0])
    if lo <= t <= hi and parts[1] == thread:
        samples.append([p.rsplit('+', 1) for p in parts[2:]])

# the app's addresses, named in one addr2line run (return addresses: one byte back is the call)
addrs = sorted({int(off, 16) for s in samples for mod, off in s if mod == 'exe'})
names, lines = {}, {}
if addrs:
    out = subprocess.run(['addr2line', '-f', '-C', '-e', debug] + ['%x' % (a - 1) for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    for i, a in enumerate(addrs):
        fn = out[2 * i] if 2 * i < len(out) else '?'
        names[a] = fn if len(fn) < 110 else fn[:107] + '...'
        loc = out[2 * i + 1] if 2 * i + 1 < len(out) else '?'
        lines[a] = loc.replace('\\', '/').rsplit('/', 1)[-1].split(' ')[0]

def frame_name(mod, off):
    return names.get(int(off, 16), '?') if mod == 'exe' else mod

incl, selfs = collections.Counter(), collections.Counter()
for s in samples:
    seen = set()
    for mod, off in s:
        if mod == 'exe':
            n = frame_name(mod, off)
            if n not in seen:
                seen.add(n)
                incl[n] += 1
    inner = next(('%s  %s' % (frame_name(m, o), lines.get(int(o, 16), '')) for m, o in s if m == 'exe'), '(no app frame)')
    leaf = s[0][0] if s else '?'
    selfs[(inner, leaf)] += 1

n = len(samples)
print('%d samples (%.1f ms .. %.1f ms)' % (n, lo, min(hi, 1e9)))
print('\ninclusive')
for name, c in incl.most_common(top):
    print('%6d %5.1f%%  %s' % (c, 100.0 * c / max(n, 1), name))
print('\nself (innermost app function / module running)')
for (name, mod), c in selfs.most_common(top):
    print('%6d %5.1f%%  %-75s %s' % (c, 100.0 * c / max(n, 1), name, mod))
