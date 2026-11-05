#!/usr/bin/env python3
"""ssh_python.py -- the board's `python` (MicroPython in ssh_shell) on stdin/stdout.

Logs in to ssh_shell over SSH, starts `python`, waits for the REPL prompt and
then relays bytes unchanged in both directions until stdin closes.  It makes
the board a MicroPython "device" for MicroPython's host tools:

    tests/run-tests.py -t exec:"python3 .../ssh_python.py" ...
    tools/pyboard.py   -d exec:"python3 .../ssh_python.py" script.py

If the SSH session ends while stdin is still open, it says why on stderr and
logs in again (up to SSH_PYTHON_RECONNECTS times, default 5), holding the
input that arrives meanwhile.  pyboard's ProcessToSerial spins forever on a
closed stdout, so a dropped session must not end this process: the test that
was running times out and the next one finds a fresh REPL.

Environment: KLAUSSCPU_BOARD_IP (default 192.168.68.59), KLAUSSCPU_SSH_USER
(admin), KLAUSSCPU_SSH_PASS (via askpass.sh, as sshrun.sh).
"""
import os
import select
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ip = os.environ.get('KLAUSSCPU_BOARD_IP', '192.168.68.59')
user = os.environ.get('KLAUSSCPU_SSH_USER', 'admin')
reconnects = int(os.environ.get('SSH_PYTHON_RECONNECTS', '5'))
env = dict(os.environ, SSH_ASKPASS=os.path.join(HERE, 'askpass.sh'),
           SSH_ASKPASS_REQUIRE='force', DISPLAY=os.environ.get('DISPLAY', ':0'))


def log(msg):
    sys.stderr.write('ssh_python: %s %s\n' % (time.strftime('%H:%M:%S'), msg))
    sys.stderr.flush()


class Session:
    def __init__(self):
        self.ssh = subprocess.Popen(
            ['ssh', '-tt', '-e', 'none', '-o', 'StrictHostKeyChecking=no',
             '-o', 'UserKnownHostsFile=/dev/null', '-o', 'PubkeyAuthentication=no',
             '-o', 'ServerAliveInterval=15', '-o', 'LogLevel=ERROR', f'{user}@{ip}'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0, env=env)
        self.fd = self.ssh.stdout.fileno()
        self.tail = bytearray()          # the last bytes from the board, for diagnostics

    def read_until(self, marker, timeout):
        buf = bytearray()
        end = time.time() + timeout
        while marker not in buf:
            left = end - time.time()
            if left <= 0:
                raise RuntimeError('no %r from the board (got %r)' % (marker, bytes(buf[-200:])))
            if not select.select([self.fd], [], [], left)[0]:
                continue
            b = os.read(self.fd, 1)
            if not b:
                raise RuntimeError('connection closed (got %r)' % bytes(buf[-200:]))
            buf += b
        return buf

    def start(self):
        self.read_until(b'$ ', 30)                 # the shell prompt
        self.ssh.stdin.write(b'python\r')
        self.read_until(b'>>> ', 30)               # MicroPython is up

    def why(self):
        """Why the session ended: ssh's status and stderr, the board's last bytes."""
        try:
            self.ssh.wait(5)
        except subprocess.TimeoutExpired:
            self.ssh.kill()
            self.ssh.wait()
        err = self.ssh.stderr.read().decode(errors='replace').strip()
        return 'ssh exit %s, stderr %r, last output %r' % (
            self.ssh.returncode, err[-300:], bytes(self.tail[-300:]))

    def close(self):
        if self.ssh.poll() is None:
            self.ssh.kill()
            self.ssh.wait()


lock = threading.Lock()          # guards sess and pending
pending = bytearray()            # input that arrived while (re)connecting
sess = None


def to_board():
    global sess
    fd = sys.stdin.fileno()
    while True:
        b = os.read(fd, 4096)
        if not b:
            break
        with lock:
            if sess is None:
                pending.extend(b)
                continue
            try:
                sess.ssh.stdin.write(b)
            except (BrokenPipeError, OSError):
                pending.extend(b)    # the reader thread reconnects and resends it
    # stdin closed: the host tool is done with the board.
    os._exit(0)


def connect(first):
    global sess
    s = Session()
    try:
        s.start()
    except RuntimeError as e:
        log('%s; %s' % (e, s.why()))
        s.close()
        return False
    with lock:
        sess = s
        data = bytes(pending)
        pending.clear()
        if data:
            s.ssh.stdin.write(data)
    if first:
        out.write(b'>>> ')
        out.flush()
    return True


out = sys.stdout.buffer
if not connect(True):
    sys.exit(1)
threading.Thread(target=to_board, daemon=True).start()
while True:
    s = sess
    b = os.read(s.fd, 4096)
    if b:
        s.tail = (s.tail + b)[-300:]
        out.write(b)
        out.flush()
        continue
    with lock:
        sess = None
    log('session ended: %s' % s.why())
    s.close()
    while True:
        if reconnects <= 0:
            log('giving up')
            os._exit(1)
        reconnects -= 1
        time.sleep(2)
        log('reconnecting')
        if connect(False):
            break
