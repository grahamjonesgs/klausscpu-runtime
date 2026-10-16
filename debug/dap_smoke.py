#!/usr/bin/env python3
"""VS Code-path smoke test: drive lldb-dap over the Debug Adapter Protocol the
way VS Code does (attach with gdb-remote, line breakpoint, stack, scopes,
variables, next, step out, continue, disconnect) and check the results.

usage: dap_smoke.py LLDB_DAP ELF PORT SOURCE LINE   (run by run_debug_smoke.sh)
"""
import json
import subprocess
import sys
import threading
import queue

LLDB_DAP, ELF, PORT, SOURCE, LINE = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])

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


request("initialize", {"clientID": "smoke", "adapterID": "lldb-dap", "linesStartAt1": True, "columnsStartAt1": True,
                       "pathFormat": "path"})
# VS Code order: attach, wait for "initialized", configure, configurationDone;
# lldb-dap answers the attach request after configurationDone.
attach_seq = send("attach", {"attachCommands": ["target create %s" % ELF, "gdb-remote %s" % PORT]})
wait(lambda m: m.get("type") == "event" and m.get("event") == "initialized", "initialized")
bp = request("setBreakpoints", {"source": {"path": SOURCE}, "breakpoints": [{"line": LINE}]})
print("breakpoint:", [(b.get("verified"), b.get("line"), b.get("message")) for b in bp["body"]["breakpoints"]])
FAILS = []


def expect(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


expect(all(b.get("verified") for b in bp["body"]["breakpoints"]), "line breakpoint verified")
request("configurationDone")
r = wait(lambda m: m.get("type") == "response" and m.get("request_seq") == attach_seq, "attach")
print("attach:", r.get("success"), r.get("message"))
ev = stopped()
print("first stop:", ev["body"].get("reason"))
expect(ev["body"].get("reason") == "breakpoint", "stopped at the breakpoint")
if ev["body"].get("reason") != "breakpoint":
    request("continue", {"threadId": ev["body"].get("threadId", 1)})
    ev = stopped()
    print("stop:", ev["body"].get("reason"), ev["body"].get("description"))
tid = ev["body"]["threadId"]


def show_stack():
    st = request("stackTrace", {"threadId": tid, "levels": 10})
    for f in st["body"]["stackFrames"]:
        print("  frame %s:%s %s" % (f.get("source", {}).get("name"), f.get("line"), f["name"]))
    return st["body"]["stackFrames"]


frames = show_stack()
expect([f["name"] for f in frames[:2]] == ["dist2", "main"], "call stack dist2 <- main")
expect(frames[0].get("line") == LINE, "top frame on the breakpoint line")
seen = {}
scopes = request("scopes", {"frameId": frames[0]["id"]})
for sc in scopes["body"]["scopes"]:
    vs = request("variables", {"variablesReference": sc["variablesReference"]})
    print("  scope %s: %s" % (sc["name"], [(v["name"], v["value"]) for v in vs["body"]["variables"]][:8]))
    for v in vs["body"]["variables"]:
        if v.get("variablesReference"):
            kids = request("variables", {"variablesReference": v["variablesReference"]})
            seen[(sc["name"], v["name"])] = [(k["name"], k["value"]) for k in kids["body"]["variables"]]
            print("    %s -> %s" % (v["name"], seen[(sc["name"], v["name"])][:4]))
expect(seen.get(("Locals", "a")) == [("x", "1"), ("y", "2")], "Locals: *a = {x=1, y=2}")
expect(seen.get(("Locals", "b")) == [("x", "4"), ("y", "6")], "Locals: *b = {x=4, y=6}")
expect(any(k[0] == "Registers" for k in seen), "Registers scope")

request("next", {"threadId": tid})
ev = stopped()
print("after next:", ev["body"].get("reason"))
expect(show_stack()[0].get("line") == LINE + 1, "next moved to the following line")
request("stepOut", {"threadId": tid})
ev = stopped()
print("after stepOut:", ev["body"].get("reason"))
expect(show_stack()[0]["name"] == "main", "step out returned to main")
request("setBreakpoints", {"source": {"path": SOURCE}, "breakpoints": []})
request("continue", {"threadId": tid})
ev = stopped()
print("after continue:", ev.get("event"), ev["body"].get("reason"), ev["body"].get("description"))
expect("SIGTRAP" in (ev["body"].get("description") or ""), "continue stopped at __builtin_debugtrap (SIGTRAP)")
request("disconnect", {"terminateDebuggee": True})
proc.wait(timeout=10)
print("lldb-dap exit code:", proc.returncode)
sys.exit(1 if FAILS else 0)
