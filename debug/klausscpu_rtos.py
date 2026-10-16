"""lldb OS plugin: FreeRTOS tasks and Zephyr threads as debugger threads.

Load it before connecting (VS Code's launch.json does this):

    (lldb) settings set target.process.python-os-plugin-path <repo>/debug/klausscpu_rtos.py

Every time the target stops, the plugin reads the kernel's thread lists
through lldb (using the program's debug info, so no structure offsets are
hard-coded) and reports one thread per task.  The running task is backed
by the CPU's own registers; the others are unwound from the context their
port saved when they were switched out.  Both the FreeRTOS port
(freertos/portable/KlaussCPU/port.S) and the Zephyr arch
(zephyr-ws/klausscpu-zephyr/arch/klausscpu/core/swap.S) save the same
frame at the task's saved stack pointer:

    [+0]   R15 ... [+120] R0    (16 x 64-bit)
    [+128] IRET word: [31:0] PC, [38] Z, [36] C, [35] V, [34] S, [42:39] INT_MASK
    SP after the IRET = saved SP + 136

Requirements: FreeRTOS works as built.  Zephyr needs CONFIG_THREAD_MONITOR=y
(the kernel's thread list) and, for names, CONFIG_THREAD_NAME=y.  Before the
scheduler starts, or for bare-metal programs, no threads are reported and
lldb shows the CPU as a single thread.

Works the same on the emulator (klausscc --gdbserver) and, later, on the
board: it only reads memory.
"""

import struct

import lldb

FRAME_BYTES = 136
# Zephyr thread_state bits (include/zephyr/kernel_structs.h, v3.7).
_ZEPHYR_STATES = [(1 << 4, "Suspended"), (1 << 3, "Dead"), (1 << 2, "Prestart"),
                  (1 << 1, "Pending"), (1 << 7, "Ready")]
# Register layout matching klausscc's gdbserver (target.xml regnums 0-19).
_REGS = (
    [("r%d" % n, 64, n) for n in range(16)]
    + [("sp", 32, 16), ("pc", 32, 17), ("flags", 32, None), ("int_mask", 32, None)]
)
_GENERIC = {"r0": "arg1", "r1": "arg2", "r2": "arg3", "r3": "arg4", "r15": "fp",
            "sp": "sp", "pc": "pc", "flags": "flags"}


class OperatingSystemPlugIn(object):
    def __init__(self, process):
        self.process = process
        self.target = process.GetTarget()
        self.registers = None
        self.threads = []
        self.frames = {}  # tid -> saved SP for tasks that are not running

    # ---- helpers ---------------------------------------------------------

    def _global(self, name):
        v = self.target.FindFirstGlobalVariable(name)
        return v if v.IsValid() else None

    def _read_u64(self, addr):
        err = lldb.SBError()
        v = self.process.ReadUnsignedFromMemory(addr, 8, err)
        return v if err.Success() else None

    @staticmethod
    def _cstr(value):
        s = (value.GetSummary() or "").strip('"')
        # a name read from uninitialised memory: don't print the garbage
        return s if s.isprintable() and "\\" not in s else "?"

    # ---- FreeRTOS -------------------------------------------------------------

    def _freertos_lists(self):
        lists = []
        ready = self._global("pxReadyTasksLists")
        if ready:
            for i in range(ready.GetNumChildren()):
                lists.append(("Ready (prio %d)" % i, ready.GetChildAtIndex(i)))
        for name, state in (("xDelayedTaskList1", "Blocked"), ("xDelayedTaskList2", "Blocked"),
                            ("xPendingReadyList", "Ready (pending)"),
                            ("xSuspendedTaskList", "Suspended"),
                            ("xTasksWaitingTermination", "Deleted")):
            v = self._global(name)
            if v:
                lists.append((state, v))
        return lists

    def _freertos_threads(self):
        current = self._global("pxCurrentTCB")
        if current is None:
            return None
        # Before vTaskStartScheduler (e.g. halted at the entry on the board,
        # where RAM still holds the previous program) the lists are garbage:
        # show the CPU's own thread.
        running = self._global("xSchedulerRunning")
        if current.GetValueAsUnsigned() == 0 or (running is not None and running.GetValueAsUnsigned() == 0):
            return []
        current_tcb = current.GetValueAsUnsigned()
        threads = []
        seen = set()
        for state, lst in self._freertos_lists():
            end = lst.GetChildMemberWithName("xListEnd")
            end_addr = end.GetLoadAddress()
            item = end.GetChildMemberWithName("pxNext")
            for _ in range(256):  # guard against a corrupt list
                addr = item.GetValueAsUnsigned()
                if addr == 0 or addr == end_addr:
                    break
                node = item.Dereference()
                tcb = node.GetChildMemberWithName("pvOwner").Cast(
                    current.GetType()).Dereference()
                tid = tcb.GetLoadAddress()
                if tid not in seen:
                    seen.add(tid)
                    name = self._cstr(tcb.GetChildMemberWithName("pcTaskName"))
                    top = tcb.GetChildMemberWithName("pxTopOfStack").GetValueAsUnsigned()
                    threads.append((tid, name, state, top))
                item = node.GetChildMemberWithName("pxNext")
        if current_tcb not in seen:
            tcb = current.Dereference()
            threads.append((current_tcb, self._cstr(tcb.GetChildMemberWithName("pcTaskName")),
                            "Running", 0))
        return [(tid, name, "Running" if tid == current_tcb else state, top, tid == current_tcb)
                for tid, name, state, top in threads]

    # ---- Zephyr ---------------------------------------------------------------

    def _zephyr_threads(self):
        kernel = self._global("_kernel")
        if kernel is None:
            return None
        head = kernel.GetChildMemberWithName("threads")
        if not head.IsValid():
            return None  # CONFIG_THREAD_MONITOR not enabled
        cpus = kernel.GetChildMemberWithName("cpus")
        cpu0 = cpus.GetChildAtIndex(0)
        current = cpu0.GetChildMemberWithName("current").GetValueAsUnsigned()
        idle = cpu0.GetChildMemberWithName("idle_thread").GetValueAsUnsigned()
        threads = []
        thread = head
        for _ in range(256):
            addr = thread.GetValueAsUnsigned()
            if addr == 0:
                break
            t = thread.Dereference()
            name = t.GetChildMemberWithName("name")
            name = self._cstr(name) if name.IsValid() else ""
            name = name or "thread@0x%x" % addr
            base = t.GetChildMemberWithName("base")
            prio = base.GetChildMemberWithName("prio").GetValueAsSigned()
            bits = base.GetChildMemberWithName("thread_state").GetValueAsUnsigned()
            handle = t.GetChildMemberWithName("switch_handle").GetValueAsUnsigned()
            if addr == current:
                state = "Running"
            elif addr == idle:
                state = "Idle"  # never on the run queue
            else:
                # Not queued and not pending: sleeping on a timeout (k_msleep).
                state = next((n for bit, n in _ZEPHYR_STATES if bits & bit), "Sleeping")
            state = "%s, prio %d" % (state, prio)
            threads.append((addr, name, state, handle, addr == current))
            thread = t.GetChildMemberWithName("next_thread")
        return threads

    # ---- OS plugin interface ----------------------------------------------------

    def get_thread_info(self):
        found = self._freertos_threads()
        if found is None:
            found = self._zephyr_threads()
        self.threads = []
        self.frames = {}
        for tid, name, state, saved_sp, running in found or []:
            info = {"tid": tid, "name": "%s [%s]" % (name, state), "state": "stopped",
                    "stop_reason": "none"}
            if running:
                info["core"] = 0  # backed by the CPU's registers and stop reason
            else:
                self.frames[tid] = saved_sp
            self.threads.append(info)
        return self.threads

    def get_register_info(self):
        if self.registers is None:
            regs = []
            offset = 0
            for name, bits, dwarf in _REGS:
                r = {"name": name, "bitsize": bits, "offset": offset, "encoding": "uint",
                     "format": "hex", "set": 0}
                if dwarf is not None:
                    r["dwarf"] = dwarf
                    r["gcc"] = dwarf
                if name in _GENERIC:
                    r["generic"] = _GENERIC[name]
                regs.append(r)
                offset += bits // 8
            self.registers = {"sets": ["General Purpose Registers"], "registers": regs}
        return self.registers

    def get_register_data(self, tid):
        saved_sp = self.frames.get(tid)
        if not saved_sp:
            return None
        err = lldb.SBError()
        raw = self.process.ReadMemory(saved_sp, FRAME_BYTES, err)
        if not err.Success() or len(raw) != FRAME_BYTES:
            return None
        saved = struct.unpack("<17Q", raw)  # R15..R0, IRET word
        gprs = list(reversed(saved[:16]))
        word = saved[16]
        flags = (((word >> 38) & 1) << 3) | (((word >> 34) & 1) << 2) \
            | (((word >> 36) & 1) << 1) | ((word >> 35) & 1)
        return struct.pack("<16QIIII", *gprs, (saved_sp + FRAME_BYTES) & 0xFFFFFFFF,
                           word & 0xFFFFFFFF, flags, (word >> 39) & 0xF)
