#!/usr/bin/env python3
"""llext_sd.py -- talk to the ssh_shell app on the board: the SD card's files
over HTTP, and the LLEXT `run` / `svc` commands over SSH.

The ssh_shell app must already be running on the board (llext_sd.sh loads it
under the board lock).  Every action prints one line per file or extension
and exits 1 if anything failed.

  llext_sd.py list <out.tsv>
      Every file and directory on the card (recursive HTTP listing).
  llext_sd.py backup <dir>
      Download every *.llext on the card to <dir>/<path on the card>, and
      check each download's size against the listing.
  llext_sd.py upload <backup-dir> <file>...
      PUT each file to the card's root under its own name, then read it back
      and compare.  A file that would overwrite one on the card is refused
      unless <backup-dir> holds a copy of the same size (llext_sd.sh backs
      the card up first).
  llext_sd.py run <out-dir> <name>[:<input>,<input>...]...
      `run <name>` over SSH, typing each <input> line at the program's "> "
      prompt; the session is saved to <out-dir>/<name>.ssh.  Prints
      "<name> exit <code>" (or "load-failed <code>", "timeout").
  llext_sd.py svc <out-dir> <name> <url-path> <regex>
      `svc load <name>`, GET <url-path> over HTTP and match <regex> against
      the body, `svc list`, `svc stop`.  Saved to <out-dir>/<name>.ssh.

Environment: KLAUSSCPU_BOARD_IP (default 192.168.68.59), KLAUSSCPU_SSH_USER
(admin), KLAUSSCPU_SSH_PASS (the ssh_server.c default), LLEXT_RUN_TIMEOUT
(seconds per `run`, default 180).
"""
import html
import os
import re
import sys
import time
import urllib.request

IP = os.environ.get("KLAUSSCPU_BOARD_IP", "192.168.68.59")
USER = os.environ.get("KLAUSSCPU_SSH_USER", "admin")
PASS = os.environ.get("KLAUSSCPU_SSH_PASS", "klausscpu")
RUN_TMO = int(os.environ.get("LLEXT_RUN_TIMEOUT", "180"))
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def http(method, path, data=None, timeout=60):
    req = urllib.request.Request(f"http://{IP}{path}", data=data, method=method)
    if data is not None:
        req.add_header("Content-Length", str(len(data)))
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def listing():
    """[(kind, path, size)] for the whole card; size is -1 for a directory."""
    rows = []

    def walk(url):
        _, page = http("GET", url + "?list")
        page = page.decode("latin-1")
        # <li><a href="/x">x</a></li><li ...>123 bytes</li> for a file
        for m in re.finditer(r'<a href="([^"]+)">[^<]*</a></li>'
                             r'(?:<li[^>]*>(\d+) bytes</li>)?', page):
            href = html.unescape(m.group(1))
            if not href.startswith(url) or href == url:
                continue
            if href.endswith("/"):
                rows.append(("D", href, -1))
                walk(href)
            else:
                rows.append(("F", href, int(m.group(2) or -1)))

    walk("/")
    return rows


def cmd_list(out):
    rows = listing()
    with open(out, "w") as f:
        for k, p, s in rows:
            f.write(f"{k}\t{p}\t{s}\n")
    print(f"{len(rows)} entries")
    return 0


def cmd_backup(d):
    bad = 0
    for k, p, s in listing():
        if k != "F" or not p.lower().endswith(".llext"):
            continue
        dst = os.path.join(d, p.lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        _, body = http("GET", p)
        with open(dst, "wb") as f:
            f.write(body)
        ok = s < 0 or len(body) == s
        bad += not ok
        print(f"{'ok' if ok else 'SIZE-MISMATCH'} {p} {len(body)}")
    return 1 if bad else 0


def cmd_upload(backup, files):
    on_card = {p: s for k, p, s in listing() if k == "F"}
    bad = 0
    for fn in files:
        name = "/" + os.path.basename(fn)
        if name in on_card:
            saved = os.path.join(backup, name.lstrip("/"))
            if not (os.path.isfile(saved) and os.path.getsize(saved) == on_card[name]):
                print(f"REFUSED {name}: on the card but not backed up in {backup}")
                bad += 1
                continue
        data = open(fn, "rb").read()
        st, _ = http("PUT", name, data)
        _, back = http("GET", name)
        ok = st == 201 and back == data
        bad += not ok
        print(f"{'ok' if ok else 'FAILED'} {name} {len(data)} (HTTP {st})")
    return 1 if bad else 0


def ssh():
    """A logged-in shell session.  A login straight after the previous
    session hung up sometimes gets the banner but no prompt: try again."""
    for attempt in range(3):
        try:
            return ssh_once()
        except Exception as e:  # noqa: BLE001 - pexpect EOF/TIMEOUT or no prompt
            print(f"ssh login attempt {attempt + 1} failed: {type(e).__name__}", file=sys.stderr)
            time.sleep(5)
    return ssh_once()


def ssh_once():
    import pexpect
    s = pexpect.spawn("ssh", ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                              "-o", "PubkeyAuthentication=no", "-o", "LogLevel=ERROR", "-tt",
                              f"{USER}@{IP}"], timeout=60)
    s.expect("assword:")
    s.sendline(PASS)
    s.expect("type 'help'")
    # The prompt can lag the banner on a freshly booted board: nudge it.
    for _ in range(6):
        if s.expect([r"\$ ", pexpect.TIMEOUT], timeout=5) == 0:
            return s
        s.send("\r")
    s.close(force=True)
    raise RuntimeError("no shell prompt")


def session_log(s, path):
    s.logfile_read = open(path, "wb")


def finish(s):
    """Hang up (the shell has no exit command) and clean the session log."""
    s.close(force=True)
    time.sleep(2)   # let the board release the session slot
    if s.logfile_read:
        s.logfile_read.close()
        raw = open(s.logfile_read.name, "rb").read()
        open(s.logfile_read.name, "wb").write(ANSI.sub(b"", raw).replace(b"\r", b""))


def cmd_run(out, specs):
    import pexpect
    os.makedirs(out, exist_ok=True)
    bad = 0
    for spec in specs:
        name, _, inp = spec.partition(":")
        inputs = [i for i in inp.split(",") if i] if inp else []
        s = ssh()
        session_log(s, os.path.join(out, name + ".ssh"))
        s.send(f"run {name}\r")
        done = [r"Program exited: (-?\d+)", r"Load/run failed \(code (-?\d+)\)", pexpect.EOF]
        res = None
        t0 = time.time()
        for line in inputs:
            i = s.expect([r"> "] + done, timeout=RUN_TMO)
            if i != 0:
                res = i - 1
                break
            time.sleep(0.3)
            s.send(line + "\r")
        try:
            if res is None:
                res = s.expect(done, timeout=RUN_TMO)
            if res == 0:
                status = f"exit {s.match.group(1).decode()}"
                bad += s.match.group(1) != b"0"
            elif res == 1:
                status = f"load-failed {s.match.group(1).decode()}"
                bad += 1
            else:
                status = "disconnected"
                bad += 1
        except pexpect.TIMEOUT:
            status = "timeout"
            bad += 1
        s.expect([r"\$ ", pexpect.EOF, pexpect.TIMEOUT], timeout=5)
        finish(s)
        print(f"{name} {status} ({time.time() - t0:.1f}s)", flush=True)
    return 1 if bad else 0


def cmd_svc(out, name, url, regex):
    import pexpect
    os.makedirs(out, exist_ok=True)
    s = ssh()
    session_log(s, os.path.join(out, name + ".ssh"))
    s.send(f"svc load {name}\r")
    i = s.expect([r"Service '[^']+' running", r"Load failed \((-?\d+)\)", pexpect.EOF, pexpect.TIMEOUT],
                 timeout=60)
    svc = re.sub(r"\..*", "", os.path.basename(name))
    status = ["loaded", "load-failed", "disconnected", "timeout"][i]
    body = b""
    if i == 0:
        try:
            st, body = http("GET", url, timeout=30)
            status = f"HTTP {st}"
        except Exception as e:  # noqa: BLE001 - report any HTTP failure
            status = f"HTTP error {e}"
        s.expect(r"\$ ")
        s.send("svc list\r")
        s.expect(r"\$ ")
        s.send(f"svc stop {svc}\r")
        s.expect([r"Service '[^']+' stopped", r"Stop failed.*", pexpect.TIMEOUT], timeout=30)
    s.expect([r"\$ ", pexpect.EOF, pexpect.TIMEOUT], timeout=5)
    finish(s)
    with open(os.path.join(out, name + ".http"), "wb") as f:
        f.write(body)
    ok = i == 0 and re.search(regex, body.decode("latin-1")) is not None
    print(f"{name} {'ok' if ok else 'FAILED'} {status} {url} -> {body[:120].decode('latin-1')}", flush=True)
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    a = argv[1]
    if a == "list" and len(argv) == 3:
        return cmd_list(argv[2])
    if a == "backup" and len(argv) == 3:
        return cmd_backup(argv[2])
    if a == "upload" and len(argv) >= 4:
        return cmd_upload(argv[2], argv[3:])
    if a == "run" and len(argv) >= 4:
        return cmd_run(argv[2], argv[3:])
    if a == "svc" and len(argv) == 6:
        return cmd_svc(*argv[2:6])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
