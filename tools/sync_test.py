"""Sync folders end to end: python -I tools/sync_test.py <exe> <workdir>

Three "devices" (three settings folders, so three sets of keys in Credential Manager and
three sync agents) share one Folder store under <workdir>. Driven through
`explorer-plus.exe --sync-cli`, the agents headless:

  A  first device: keys made, its folder uploaded (encrypted)
  B  joins with the recovery key; files go both ways; a conflict
  C  joins with a pairing code from A; A then removes C (new keys C never gets)

Optionally the same pair of devices over Azure DevOps Git, when EXPLORERPLUS_AZDO_TEST_URL
and EXPLORERPLUS_AZDO_TEST_PAT are set (a branch of its own, deleted at the end).
Everything is cleaned up: agents stopped, keys and secrets out of Credential Manager.
"""
import os, sys, time, shutil, subprocess, secrets

exe, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
if os.path.dirname(work) == work or (os.path.isdir(work) and os.listdir(work) and not os.path.exists(os.path.join(work, '.sync_test'))):
    sys.exit('refusing to use %s: give a new or empty folder' % work)

def rmtree(p):
    # (git's object files are read-only)
    def again(fn, path, _):
        os.chmod(path, 0o666)
        fn(path)
    if os.path.exists(p):
        shutil.rmtree(p, onerror=again)

rmtree(work)
os.makedirs(work)
open(os.path.join(work, '.sync_test'), 'w').close()

failures = []
def check(cond, what):
    print(('  ok   ' if cond else '  FAIL ') + what)
    if not cond:
        failures.append(what)

class Device:
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(work, name)
        self.root = os.path.join(self.dir, 'Synced')
        os.makedirs(self.dir, exist_ok=True)
        self.env = dict(os.environ)
        self.env['EXPLORERPLUS_SETTINGS'] = os.path.join(self.dir, 'settings.ini')
        self.env['EXPLORERPLUS_SYNC_KDF_KIB'] = '8192'      # Argon2id, cheap for tests
        self.env['EXPLORERPLUS_SYNC_TRASH'] = '1'           # deletions kept in the pair's folder, not the Recycle Bin
        self.started = False

    def cli(self, *args, timeout=900):
        r = subprocess.run([exe, '--sync-cli'] + list(args), env=self.env, capture_output=True, timeout=timeout)
        out = r.stdout.decode('utf-8', 'replace')
        kv = {}
        for line in out.splitlines():
            if '=' in line:
                k, v = line.split('=', 1)
                kv.setdefault(k, v)
                kv.setdefault('_all_' + k, []).append(v)
        kv['_rc'] = r.returncode
        kv['_out'] = out
        return kv

    def start(self):
        r = self.cli('start-agent', 'headless=1')
        self.started = r['_rc'] == 0
        return self.started

    def sync(self):
        return self.cli('sync-now', 'wait=1')

    def state(self):
        r = self.cli('pairs')
        return r.get('p0.state', ''), r

    def stop(self):
        if self.started:
            self.cli('quit', timeout=60)
            for _ in range(100):                               # the agent's mutex gone
                if self.cli('ping', timeout=10)['_rc'] == 2:
                    break
                time.sleep(0.1)
            self.started = False
        self.cli('forget-keys', timeout=60)

def write(p, text):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(text)

def read(p):
    try:
        with open(p, encoding='utf-8') as f:
            return f.read()
    except OSError:
        return None

def wait_state(dev, want, secs=30):
    st = ''
    for _ in range(secs * 5):
        st, _ = dev.state()
        if st in want:
            return st
        time.sleep(0.2)
    return st

def run_pair(label, type_args, cleanup=None):
    print('== %s' % label)
    a, b, c = Device(label + '-a'), Device(label + '-b'), Device(label + '-c')
    devices = [a, b, c]
    try:
        write(os.path.join(a.root, 'hello.txt'), 'hello from A')
        write(os.path.join(a.root, 'docs', 'Ünïcode name.md'), '# notes')
        r = a.cli('add-pair', 'root=' + a.root, *type_args, 'create=1')
        check(r['_rc'] == 0, 'A: pair added')
        check(a.start(), 'A: agent started')
        r = a.cli('setup-keys')
        code = r.get('code', '')
        check(r['_rc'] == 0 and len(code.replace('-', '')) == 15, 'A: keys made, recovery key %s' % ('shown' if code else 'missing'))
        r = a.sync()
        check(r.get('state') == 'up to date', 'A: first sync up to date (%s %s)' % (r.get('state'), r.get('message', '')))

        # the store holds nothing readable
        if 'type=folder' in type_args:
            store = [t.split('=', 1)[1] for t in type_args if t.startswith('cfg.path=')][0]
            names = []
            for dp, dn, fn in os.walk(store):
                names += dn + fn
            check(not any('hello' in n or 'docs' in n or 'notes' in n.lower() for n in names), 'store: no plain names')
            leaked = False
            for dp, dn, fn in os.walk(store):
                for n in fn:
                    with open(os.path.join(dp, n), 'rb') as f:
                        if b'hello from A' in f.read():
                            leaked = True
            check(not leaked, 'store: no plain contents')

        # B: the same storage; locked until it has the keys
        r = b.cli('add-pair', 'root=' + b.root, *type_args)
        check(r['_rc'] == 0, 'B: pair added')
        check(b.start(), 'B: agent started')
        st = wait_state(b, ('locked',))
        check(st == 'locked', 'B: locked before unlocking (%s)' % st)
        r = b.cli('unlock', 'mode=recovery', 'code=' + code.lower().replace('-', ' '))
        check(r['_rc'] == 0, 'B: unlocked with the recovery key (%s)' % r.get('error', ''))
        r = b.sync()
        check(r.get('state') == 'up to date', 'B: synced (%s %s)' % (r.get('state'), r.get('message', '')))
        check(read(os.path.join(b.root, 'hello.txt')) == 'hello from A', 'B: has hello.txt')
        check(read(os.path.join(b.root, 'docs', 'Ünïcode name.md')) == '# notes', 'B: has the Unicode name')

        # both ways
        write(os.path.join(b.root, 'from-b.txt'), 'B wrote this')
        write(os.path.join(b.root, 'hello.txt'), 'hello changed on B')
        b.sync()
        a.sync()
        check(read(os.path.join(a.root, 'from-b.txt')) == 'B wrote this', 'A: got from-b.txt')
        check(read(os.path.join(a.root, 'hello.txt')) == 'hello changed on B', 'A: got the change')
        os.remove(os.path.join(a.root, 'from-b.txt'))
        a.sync()
        b.sync()
        check(not os.path.exists(os.path.join(b.root, 'from-b.txt')), 'B: the deletion arrived')

        # a conflict: both change the same file before syncing
        write(os.path.join(a.root, 'both.txt'), 'base')
        a.sync()
        b.sync()
        write(os.path.join(a.root, 'both.txt'), 'A version')
        write(os.path.join(b.root, 'both.txt'), 'B version')
        a.sync()
        b.sync()
        a.sync()
        copies_a = sorted(n for n in os.listdir(a.root) if n.startswith('both'))
        copies_b = sorted(n for n in os.listdir(b.root) if n.startswith('both'))
        check(len(copies_a) == 2 and copies_a == copies_b, 'conflict: both versions kept on both (%s / %s)' % (copies_a, copies_b))
        texts = sorted(read(os.path.join(a.root, n)) for n in copies_a)
        check(texts == ['A version', 'B version'], 'conflict: contents %s' % texts)

        # status for the file list
        r = a.cli('status', 'folder=' + a.root)
        items = dict(x.split('\t', 1) for x in r.get('_all_item', []))
        check(items.get('hello.txt') == 'synced', 'status: hello.txt synced (%s)' % items.get('hello.txt'))

        # C joins with a pairing code from A
        r = a.cli('pair-offer')
        pcode = r.get('code', '')
        check(r['_rc'] == 0 and pcode, 'A: pairing code made')
        r = c.cli('add-pair', 'root=' + c.root, *type_args)
        check(c.start(), 'C: agent started')
        r = c.cli('unlock', 'mode=code', 'code=' + pcode)
        check(r['_rc'] == 0, 'C: unlocked with the pairing code (%s)' % r.get('error', ''))
        r = c.sync()
        check(read(os.path.join(c.root, 'hello.txt')) == 'hello changed on B', 'C: synced')
        r = c.cli('unlock', 'mode=code', 'code=' + pcode)
        check(r['_rc'] != 0, 'pairing code works once')

        r = a.cli('devices')
        devs = [d.split('\t') for d in r.get('_all_device', [])]
        check(len(devs) == 3, 'A: three devices listed (%d)' % len(devs))
        cdev = c.cli('keys-status').get('device', '')
        r = a.cli('remove-device', 'device=' + cdev)
        check(r['_rc'] == 0 and r.get('code'), 'A: removed C, new recovery key (%s)' % r.get('error', ''))
        r = a.cli('devices')
        check(len(r.get('_all_device', [])) == 2, 'A: two devices left')
        write(os.path.join(a.root, 'after-removal.txt'), 'C must not read this')
        a.sync()
        b.sync()
        check(read(os.path.join(b.root, 'after-removal.txt')) == 'C must not read this', 'B: still syncs after the rotation')
        c.sync()
        check(read(os.path.join(c.root, 'after-removal.txt')) is None, 'C: gets nothing new')
        st, _ = c.state()
        check(st in ('locked', 'error'), 'C: locked out (%s)' % st)
    finally:
        for d in devices:
            d.stop()
        if cleanup:
            cleanup()

store = os.path.join(work, 'store')
os.makedirs(store)
run_pair('folder', ['type=folder', 'cfg.path=' + store])

url, pat = os.environ.get('EXPLORERPLUS_AZDO_TEST_URL'), os.environ.get('EXPLORERPLUS_AZDO_TEST_PAT')
if url and pat:
    branch = 'sync-test/' + secrets.token_hex(4)

    def delete_branch():
        env = dict(os.environ)
        hdr = 'Authorization: Basic ' + __import__('base64').b64encode((':' + pat).encode()).decode()
        env.update({'GIT_CONFIG_COUNT': '2', 'GIT_CONFIG_KEY_0': 'http.extraHeader', 'GIT_CONFIG_VALUE_0': hdr,
                    'GIT_CONFIG_KEY_1': 'credential.helper', 'GIT_CONFIG_VALUE_1': '', 'GIT_TERMINAL_PROMPT': '0'})
        r = subprocess.run(['git', '-c', 'http.sslBackend=schannel', 'push', url, '--delete', branch], env=env, capture_output=True)
        print('  (branch %s %s)' % (branch, 'deleted' if r.returncode == 0 else 'NOT deleted'))

    # (the PAT goes through the environment of the CLI only: add-pair reads it from there)
    os.environ['EXPLORERPLUS_SYNC_TEST_PAT'] = pat
    run_pair('azdo', ['type=azdo', 'cfg.url=' + url, 'cfg.branch=' + branch, 'cfg.pat=env:EXPLORERPLUS_SYNC_TEST_PAT'], delete_branch)

rmtree(work)
print('%d failed' % len(failures) if failures else 'all passed')
sys.exit(1 if failures else 0)
