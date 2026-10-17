#!/usr/bin/env python3
"""VS Code-path RTOS check: drive lldb-dap over DAP with the same init/attach
commands as the RTOS launch configurations (OS plugin, debug/.rtos.elf),
stop at a source line and check the RTOS threads and each thread's stack.

usage: dap_rtos.py LLDB_DAP WORKSPACE PORT SOURCE LINE MIN_THREADS   (run_rtos_smoke.sh)
"""
import json
import subprocess
import sys
import threading
import queue

LLDB_DAP, WS, PORT, SOURCE, LINE, MIN_THREADS = (sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
                                                 int(sys.argv[5]), int(sys.argv[6]))

proc = subprocess.Popen([LLDB_DAP], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
msgs = queue.Queue()
seq = 0


def reader():
    f = proc.stdout
    while True:
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            c = f.read(1)
            if not c:
                return
            header += c
        length = int([l for l in header.split(b"\r\n") if l.startswith(b"Content-Length")][0].split(b":")[1])
        msgs.put(json.loads(f.read(length)))


threading.Thread(target=reader, daemon=True).start()


def send(command, arguments=None):
    global seq
    seq += 1
    body = json.dumps({"seq": seq, "type": "request", "command": command, "arguments": arguments or {}}).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()
    return seq


def wait(pred, what, timeout=30):
    while True:
        m = msgs.get(timeout=timeout)
        if pred(m):
            return m
        if m.get("type") == "event" and m.get("event") == "output":
            text = m["body"].get("output", "")
            if text.strip():
                print("  [output]", text.rstrip())


def request(command, arguments=None):
    s = send(command, arguments)
    r = wait(lambda m: m.get("type") == "response" and m.get("request_seq") == s, command)
    if not r.get("success"):
        print("FAILED", command, r.get("message"), r.get("body"))
    return r


def stopped():
    return wait(lambda m: m.get("type") == "event" and m.get("event") in ("stopped", "exited", "terminated"), "stop")



FAILS = []


def expect(cond, what):
    print(("ok  " if cond else "FAIL"), what)
    if not cond:
        FAILS.append(what)


request("initialize", {"clientID": "smoke", "adapterID": "lldb-dap", "linesStartAt1": True, "columnsStartAt1": True,
                       "pathFormat": "path"})
send("attach", {
    "initCommands": ["settings set target.process.python-os-plugin-path %s/debug/klausscpu_rtos.py" % WS],
    "attachCommands": [
        "script import os; lldb.debugger.HandleCommand('target create ' + "
        "os.path.realpath('%s/debug/.rtos.elf'))" % WS,
        "gdb-remote %s" % PORT],
})
wait(lambda m: m.get("type") == "event" and m.get("event") == "initialized", "initialized")
bp = request("setBreakpoints", {"source": {"path": SOURCE}, "breakpoints": [{"line": LINE}]})
expect(all(b.get("verified") for b in bp["body"]["breakpoints"]), "breakpoint at %s:%d verified" % (SOURCE, LINE))
request("configurationDone")
# VS Code asks for the thread baseline right after configurationDone; lldb-dap
# answers that one request from the list it cached at attach.
request("threads")
ev = stopped()
if ev.get("event") == "stopped" and ev["body"].get("reason") != "breakpoint":
    request("continue", {"threadId": ev["body"].get("threadId", 1)})   # the entry halt
    ev = stopped()
expect(ev.get("event") == "stopped" and ev["body"].get("reason") == "breakpoint", "stopped at the breakpoint")
threads = request("threads")["body"]["threads"]
print("threads:", [t["name"] for t in threads])
expect(len(threads) >= MIN_THREADS, "at least %d RTOS threads" % MIN_THREADS)
expect(all("[" in t["name"] for t in threads), "every thread named with its state")
top = None
for t in threads:
    frames = request("stackTrace", {"threadId": t["id"], "levels": 20}).get("body", {}).get("stackFrames", [])
    print("  %-34s %s" % (t["name"], " <- ".join(f["name"].split("(")[0] for f in frames[:4])))
    expect(len(frames) >= 1 and frames[0].get("source"), "%s has a source-level stack" % t["name"])
    if t["id"] == ev["body"].get("threadId"):
        top = frames[0]
expect(top is not None and top.get("line") == LINE, "stopped thread is at line %d" % LINE)
request("disconnect", {"terminateDebuggee": False})
proc.wait(timeout=30)
print("RESULT:", "FAIL" if FAILS else "PASS")
sys.exit(1 if FAILS else 0)
