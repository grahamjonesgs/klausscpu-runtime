#!/usr/bin/env python3
"""zephyr/sd_mount_test.py <mountpoint> [<second mountpoint>]

Exercises the board's SD card, mounted with sshfs, the way an editor does:
every file is compared byte for byte.  Works in <mountpoint>/vs_t, which must
not exist, and removes it.  Exits 1 on a failure.  A second mount of the same
card (a second SSH session) adds tests of two sessions writing at once.

  sshfs admin@<board>:/SD: <mountpoint> -o uid=$(id -u),gid=$(id -g)
"""
import os, sys, time, stat, threading
M = sys.argv[1]
M2 = sys.argv[2] if len(sys.argv) > 2 else None
D = os.path.join(M, 'vs_t')
res = []
def check(name, fn):
    t = time.time()
    try:
        r = fn()
        ok = r is None or r is True
        msg = '' if ok else repr(r)[:90]
    except Exception as e:
        ok, msg = False, '%s: %s' % (type(e).__name__, e)
    res.append(ok)
    print('%s %-44s %5.1fs %s' % ('PASS' if ok else 'FAIL', name, time.time() - t, msg), flush=True)
def wr(p, b, mode='wb'):
    with open(p, mode) as f: f.write(b)
def rd(p):
    with open(p, 'rb') as f: return f.read()
def eq(p, b):
    g = rd(p)
    return True if g == b else 'got %d bytes %r, want %d' % (len(g), g[:30], len(b))
A = os.path.join(D, 'a.py')
check('mkdir', lambda: os.mkdir(D))
check('missing file is ENOENT', lambda: (not os.path.exists(os.path.join(D, 'nope'))))
def enoent():
    try: open(os.path.join(D, 'nope'))
    except FileNotFoundError: return True
    return 'no FileNotFoundError'
check('open missing raises FileNotFoundError', enoent)
check('create', lambda: wr(A, b'print("one")\n'))
check('read back', lambda: eq(A, b'print("one")\n'))
long = b'# a long line of text\n' * 200
check('overwrite with longer', lambda: (wr(A, long), eq(A, long))[1])
check('overwrite with SHORTER: no stale tail', lambda: (wr(A, b'x=1\n'), eq(A, b'x=1\n'))[1])
check('size after overwrite', lambda: os.stat(A).st_size == 4 or os.stat(A).st_size)
check('append', lambda: (wr(A, b'y=2\n', 'ab'), eq(A, b'x=1\ny=2\n'))[1])
def inplace():
    with open(A, 'r+b') as f:
        f.seek(2); f.write(b'9')
    return eq(A, b'x=9\ny=2\n')
check('write in the middle', inplace)
def fstat_open():
    p = os.path.join(D, 'open.txt')
    with open(p, 'wb') as f:
        f.write(b'z' * 5000); f.flush()
        n = os.fstat(f.fileno()).st_size
    return n == 5000 or n
check('size of a file still open', fstat_open)
def stat_midwrite():
    p = os.path.join(D, 'mid.txt')
    with open(p, 'wb') as f:
        f.write(b'z' * 5000); f.flush()
        time.sleep(0.2)
        mid = os.stat(p).st_size
    after = os.stat(p).st_size
    got = rd(p)
    os.remove(p)
    return (mid, after, len(got)) == (5000, 5000, 5000) or (mid, after, len(got))
check('stat during a write, then after close', stat_midwrite)
def big_midwrite():
    p = os.path.join(D, 'mid2.bin'); b = os.urandom(300000)
    with open(p, 'wb') as f:
        for i in range(0, len(b), 50000):
            f.write(b[i:i + 50000]); f.flush(); os.stat(p)
    r = eq(p, b); os.remove(p); return r
check('300 KB write with a stat after every chunk', big_midwrite)
def fd_truncate():
    # What an editor's save can do: open, truncate through the handle, write.
    p = os.path.join(D, 'fd.txt')
    wr(p, b'old contents that are long\n')
    fd = os.open(p, os.O_RDWR)
    os.ftruncate(fd, 0)
    os.write(fd, b'new\n')
    os.fsync(fd)
    os.close(fd)
    r = eq(p, b'new\n'); os.remove(p); return r
check('ftruncate on a handle, write, fsync', fd_truncate)
check('truncate to 3', lambda: (os.truncate(A, 3), eq(A, b'x=9'))[1])
check('truncate to 0', lambda: (os.truncate(A, 0), eq(A, b''))[1])
check('truncate extends with zeros', lambda: (os.truncate(A, 4), eq(A, b'\0\0\0\0'))[1])
def excl():
    try:
        fd = os.open(A, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.close(fd); return 'exclusive create replaced a file'
    except OSError: return eq(A, b'\0\0\0\0')
check('O_EXCL refuses an existing file', excl)
B = os.path.join(D, 'b.py')
check('rename to a new name', lambda: (wr(A, b'one\n'), os.rename(A, B), eq(B, b'one\n'))[2])
def atomic():
    t = os.path.join(D, 'b.py.tmp')
    wr(t, b'saved atomically\n'); os.replace(t, B)
    return eq(B, b'saved atomically\n')
check('atomic save: rename over existing', atomic)
check('old name gone', lambda: not os.path.exists(A))
check('chmod accepted', lambda: os.chmod(B, 0o644))
check('utime accepted', lambda: os.utime(B))
check('stat says regular file', lambda: stat.S_ISREG(os.stat(B).st_mode))
check('stat says directory', lambda: stat.S_ISDIR(os.stat(D).st_mode))
check('listdir', lambda: sorted(os.listdir(D)) == ['b.py', 'open.txt'] or sorted(os.listdir(D)))
sub = os.path.join(D, 'sub')
check('nested dir and file', lambda: (os.mkdir(sub), wr(os.path.join(sub, 'c.txt'), b'c'), eq(os.path.join(sub, 'c.txt'), b'c'))[2])
def nonempty():
    try: os.rmdir(sub); return 'removed a non-empty dir'
    except OSError: return True
check('rmdir refuses a non-empty dir', nonempty)
for n, size in (('100 KB', 100 * 1024), ('1 MB', 1 << 20)):
    def big(size=size):
        b = os.urandom(size); p = os.path.join(D, 'big.bin')
        wr(p, b); return eq(p, b)
    check('%s random round trip' % n, big)
def list_order():
    # Close the newest of several open files, then keep using the others.
    ps = [os.path.join(D, 'h%d.txt' % i) for i in range(3)]
    fa = open(ps[0], 'wb'); fb = open(ps[1], 'wb')
    fb.write(b'b'); fb.close()
    fc = open(ps[2], 'wb')
    fa.write(b'a'); fa.flush(); n = os.fstat(fa.fileno()).st_size
    fc.write(b'c'); fc.close(); fa.close()
    got = [rd(p) for p in ps]
    for p in ps: os.remove(p)
    return (n == 1 and got == [b'a', b'b', b'c']) or (n, got)
check('close the newest of several open files', list_order)
def rewrites():
    p = os.path.join(D, 'r.py')
    for i in range(150):
        b = (b'# version %d\n' % i) * (1 + (i * 37) % 60)
        wr(p, b)
        g = rd(p)
        if g != b: return 'rewrite %d: %d bytes, want %d' % (i, len(g), len(b))
    os.remove(p)
check('150 rapid saves of one file, varying size', rewrites)
def atomics():
    p = os.path.join(D, 's.py'); t = p + '.tmp'
    wr(p, b'start')
    for i in range(100):
        b = b'atomic %d\n' % i * (1 + i % 40)
        wr(t, b); os.replace(t, p)
        g = rd(p)
        if g != b: return 'save %d: %d bytes, want %d' % (i, len(g), len(b))
    os.remove(p)
check('100 rapid write, close, rename saves', atomics)
def second_writer():
    p = os.path.join(D, 'w.txt')
    f1 = open(p, 'wb', buffering=0); f1.write(b'first')
    f2 = open(p, 'r+b', buffering=0)
    f2.write(b'SECOND')
    f2.close()
    try:
        f1.write(b'late'); f1.close()
    except OSError:
        pass
    g = rd(p); os.remove(p)
    return g in (b'SECOND', b'SECONDlate') or g
check('second writer on an open file: no damage', second_writer)
def rename_open():
    p = os.path.join(D, 'o.txt'); q = os.path.join(D, 'o2.txt')
    f = open(p, 'wb', buffering=0); f.write(b'kept')
    os.rename(p, q)
    try:
        f.close()
    except OSError:
        pass
    r = eq(q, b'kept'); os.remove(q); return r
check('rename a file while it is being written', rename_open)
def remove_open():
    p = os.path.join(D, 'x.txt')
    f = open(p, 'wb', buffering=0); f.write(b'gone')
    os.remove(p)
    try:
        f.write(b'more'); f.close()
    except OSError:
        pass
    wr(p, b'new'); r = eq(p, b'new'); os.remove(p); return r
check('delete a file while it is being written', remove_open)
def rename_case():
    a = os.path.join(D, 'case.py'); b = os.path.join(D, 'CASE.py')
    wr(a, b'keep me\n')
    os.rename(a, b)
    names = [n for n in os.listdir(D) if n.lower() == 'case.py']
    r = eq(b, b'keep me\n'); os.remove(b)
    return (names == ['CASE.py'] and r is True) or (names, r)
check('rename that changes only the case', rename_case)
def rename_missing():
    a = os.path.join(D, 'nothere'); b = os.path.join(D, 'target.txt')
    wr(b, b'precious')
    try:
        os.rename(a, b); r = 'rename of a missing file succeeded'
    except OSError:
        r = eq(b, b'precious')
    os.remove(b); return r
check('rename of a missing file keeps the target', rename_missing)
def write_after_rename():
    a = os.path.join(D, 'wa.txt'); b = os.path.join(D, 'wb.txt')
    f = open(a, 'wb', buffering=0); f.write(b'one ')
    os.rename(a, b)
    f.write(b'two'); f.close()
    r = eq(b, b'one two'); os.remove(b); return r
check('keep writing after a rename', write_after_rename)
def rename_dir_open():
    d = os.path.join(D, 'd1'); e = os.path.join(D, 'd2'); os.mkdir(d)
    f = open(os.path.join(d, 'f.txt'), 'wb', buffering=0); f.write(b'one ')
    os.rename(d, e)
    f.write(b'two'); f.close()
    p = os.path.join(e, 'f.txt'); r = eq(p, b'one two'); os.remove(p); os.rmdir(e); return r
check('rename the folder of an open file', rename_dir_open)
def stale_reader():
    # A reader of a replaced file must not get another file's bytes.
    a = os.path.join(D, 'st.txt'); b = os.path.join(D, 'other.bin')
    old = b'O' * 20000; other = b'X' * 20000
    wr(a, old)
    f = open(a, 'rb', buffering=0); first = f.read(10)
    os.remove(a); wr(b, other)
    try:
        g = f.read(5000)
    except OSError:
        g = None
    try:
        f.close()
    except OSError:
        pass
    r = eq(b, other); os.remove(b)
    if r is not True: return r
    return (first == b'O' * 10 and (g is None or set(g) <= {ord('O')})) or ('reader got', g[:8])
check('reader of a deleted file gets no other data', stale_reader)
def parallel(mounts, name):
    errs = []
    def worker(root, n):
        p = os.path.join(root, 'vs_t', '%s%d.bin' % (name, n))
        try:
            for i in range(25):
                b = os.urandom(3000 + 4000 * n + i * 50)
                wr(p, b)
                if rd(p) != b: errs.append('%d: round %d differs' % (n, i)); return
            os.remove(p)
        except OSError as e:
            errs.append('%d: %s' % (n, e))
    ts = [threading.Thread(target=worker, args=(mounts[n % len(mounts)], n)) for n in range(4)]
    for t in ts: t.start()
    for t in ts: t.join()
    return errs[0] if errs else True
check('4 threads writing separate files', lambda: parallel([M], 'p'))
if M2:
    check('second session sees the first one\'s file', lambda: eq(os.path.join(M2, 'vs_t', 'b.py'), rd(B)))
    check('2 sessions, 4 threads writing', lambda: parallel([M, M2], 'q'))
check('remove files', lambda: (os.remove(os.path.join(sub, 'c.txt')), os.remove(os.path.join(D, 'big.bin')), os.remove(os.path.join(D, 'open.txt')))[0])
check('rmdir', lambda: os.rmdir(sub))
check('remove the test folder', lambda: (os.remove(B), os.rmdir(D))[0])
print('passed %d of %d' % (sum(res), len(res)))
sys.exit(0 if all(res) else 1)
