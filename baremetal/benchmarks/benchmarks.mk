# benchmarks.mk — CoreMark and Embench-IoT rules, included by ../Makefile.
#
# The upstream sources are NOT in this repo: run benchmarks/get-benchmarks.sh
# once to clone them (pinned commits) into benchmarks/coremark/ and
# benchmarks/embench-iot/.  Only the KlaussCPU ports live here:
# benchmarks/coremark-port/ and benchmarks/embench-port/.  See
# benchmarks/README.md for the methodology and benchmarks/RESULTS.md for
# board numbers.
#
# Outputs go to bench-build/<OPT>/ (e.g. bench-build/O2/), one tree per
# optimisation level, so -O1/-O2/-Os builds sit side by side:
#   make coremark OPT=-O2   -> bench-build/O2/coremark.elf           (PERFORMANCE_RUN)
#                              bench-build/O2/coremark_validate.elf  (VALIDATION_RUN)
#   make embench  OPT=-O2   -> bench-build/O2/embench/<name>.elf      (board runs)
#                              bench-build/O2/embench-size/<name>.elf (size, dummy libs)
#   make embench-sizes OPT=-Os  — print the Embench .text size table
# The shared runtime objects (crt0.o, syscalls.o, crt-*.o …) are the ordinary
# top-level ones; `make clean` after a toolchain/ABI change, as for every
# other program here.

empty :=
space := $(empty) $(empty)

BENCH       := benchmarks
BENCH_OPT   := $(subst $(space),_,$(subst -,,$(strip $(OPT))))
BENCH_OUT   := bench-build/$(BENCH_OPT)

.PHONY: coremark coremark_validate embench embench-sizes bench-clean

# compiler-rt helpers the benchmarks need beyond the standard CRT_FP_OBJS set
# (64-bit integer <-> FP conversions; built by the crt-%.o rule).
BENCH_CRT_OBJS := $(CRT_FP_OBJS) $(patsubst %,crt-%.o,floatdidf floatundidf floatdisf floatundisf)

# ── CoreMark ──────────────────────────────────────────────────────────────────
COREMARK_SRC  := $(BENCH)/coremark
COREMARK_PORT := $(BENCH)/coremark-port
# Fixed iteration count, sized for > 10 s on the board (run rules) with
# margin for a faster compiler.  0 = let core_main.c auto-size to >= 10 s.
COREMARK_ITERATIONS ?= 2000
CM_NAMES := core_list_join core_main core_matrix core_state core_util
COREMARK_CFLAGS = $(CFLAGS) -I$(COREMARK_PORT) -I$(COREMARK_SRC) \
                  -DITERATIONS=$(COREMARK_ITERATIONS) -DFLAGS_STR='"$(OPT)"'

$(COREMARK_SRC)/coremark.h:
	@echo "CoreMark sources missing: run baremetal/benchmarks/get-benchmarks.sh" >&2; exit 1

# $(call coremark_rules,<variant>,<run define>)
define coremark_rules
$(BENCH_OUT)/$(1)/%.o: $(COREMARK_SRC)/%.c $(COREMARK_SRC)/coremark.h
	@mkdir -p $$(@D)
	$$(CC) $$(COREMARK_CFLAGS) -D$(2)=1 -c -o $$@ $$<
$(BENCH_OUT)/$(1)/core_portme.o: $(COREMARK_PORT)/core_portme.c $(COREMARK_SRC)/coremark.h
	@mkdir -p $$(@D)
	$$(CC) $$(COREMARK_CFLAGS) -D$(2)=1 -c -o $$@ $$<
$(BENCH_OUT)/$(1).elf: $(patsubst %,$(BENCH_OUT)/$(1)/%.o,$(CM_NAMES) core_portme) \
                       $(BENCH_CRT_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$$(LLD) -T $$(LD_SCRIPT) --gc-sections -o $$@ $$(RUNTIME_OBJS) $$(LIBC_OBJS) \
	    $$(BENCH_CRT_OBJS) $(patsubst %,$(BENCH_OUT)/$(1)/%.o,$(CM_NAMES) core_portme) $$(LIBC_LINK)
	@echo "==> $$@ built"
endef
$(eval $(call coremark_rules,coremark,PERFORMANCE_RUN))
$(eval $(call coremark_rules,coremark_validate,VALIDATION_RUN))

coremark: $(BENCH_OUT)/coremark.elf $(BENCH_OUT)/coremark_validate.elf
coremark_validate: $(BENCH_OUT)/coremark_validate.elf

# ── Embench-IoT ───────────────────────────────────────────────────────────────
EMBENCH_SRC  := $(BENCH)/embench-iot
EMBENCH_PORT := $(BENCH)/embench-port
# Embench's CPU_MHZ is only the iteration multiplier (LOCAL_SCALE_FACTOR *
# CPU_MHZ).  1 = each benchmark does the reference work of ~4 s on a 1 MHz
# Cortex-M4; cycles at CPU_MHZ=1 then give the per-MHz Embench speed score
# directly (baseline_ms * 1000 / cycles).
EMBENCH_CPU_MHZ ?= 1
EMBENCH_BENCHES := aha-mont64 crc32 cubic edn huffbench matmult-int minver \
                   nbody nettle-aes nettle-sha256 nsichneu picojpeg qrduino \
                   sglib-combined slre st statemate ud wikisort
EMBENCH_CFLAGS = $(CFLAGS) -I$(EMBENCH_SRC)/support -I$(EMBENCH_PORT) \
                 -DHAVE_BOARDSUPPORT_H -DCPU_MHZ=$(EMBENCH_CPU_MHZ) -DWARMUP_HEAT=1
EMB_OUT      := $(BENCH_OUT)/embench
EMB_SIZE_OUT := $(BENCH_OUT)/embench-size

$(EMBENCH_SRC)/support/support.h:
	@echo "Embench-IoT sources missing: run baremetal/benchmarks/get-benchmarks.sh" >&2; exit 1

# Shared support objects.  main.o is upstream support/main.c with main renamed
# (the port's main() wraps it); main-size.o is the unmodified one for sizing.
$(EMB_OUT)/support/main.o: $(EMBENCH_SRC)/support/main.c $(EMBENCH_SRC)/support/support.h
	@mkdir -p $(@D)
	$(CC) $(EMBENCH_CFLAGS) -Dmain=embench_main -c -o $@ $<
$(EMB_OUT)/support/main-size.o: $(EMBENCH_SRC)/support/main.c $(EMBENCH_SRC)/support/support.h
	@mkdir -p $(@D)
	$(CC) $(EMBENCH_CFLAGS) -c -o $@ $<
$(EMB_OUT)/support/beebsc.o: $(EMBENCH_SRC)/support/beebsc.c $(EMBENCH_SRC)/support/support.h
	@mkdir -p $(@D)
	$(CC) $(EMBENCH_CFLAGS) -c -o $@ $<
$(EMB_OUT)/support/boardsupport-size.o: $(EMBENCH_PORT)/boardsupport.c $(EMBENCH_SRC)/support/support.h
	@mkdir -p $(@D)
	$(CC) $(EMBENCH_CFLAGS) -DEMBENCH_SIZE_BUILD -c -o $@ $<
# Embench's dummy libraries (support/dummy-*.c) plus KlaussCPU's compiler-rt
# stubs (embench-port/dummy-libklausscpu.c, standing in for dummy-libgcc.c,
# which only covers Arm).  -fno-builtin: they redefine libc functions.
EMB_DUMMY_OBJS := $(patsubst %,$(EMB_OUT)/support/dummy-%.o,crt0 libc libm) \
                  $(EMB_OUT)/support/dummy-libklausscpu.o
$(EMB_OUT)/support/dummy-%.o: $(EMBENCH_SRC)/support/dummy-%.c $(EMBENCH_SRC)/support/support.h
	@mkdir -p $(@D)
	$(CC) $(EMBENCH_CFLAGS) -fno-builtin -w -c -o $@ $<
$(EMB_OUT)/support/dummy-libklausscpu.o: $(EMBENCH_PORT)/dummy-libklausscpu.c
	@mkdir -p $(@D)
	$(CC) $(EMBENCH_CFLAGS) -fno-builtin -c -o $@ $<

# $(call embench_rules,<benchmark>)
define embench_rules
EMB_OBJS_$(1) := $(patsubst $(EMBENCH_SRC)/src/$(1)/%.c,$(EMB_OUT)/$(1)/%.o,$(wildcard $(EMBENCH_SRC)/src/$(1)/*.c))
$(EMB_OUT)/$(1)/%.o: $(EMBENCH_SRC)/src/$(1)/%.c $(EMBENCH_SRC)/support/support.h
	@mkdir -p $$(@D)
	$$(CC) $$(EMBENCH_CFLAGS) -c -o $$@ $$<
$(EMB_OUT)/$(1)/boardsupport.o: $(EMBENCH_PORT)/boardsupport.c $(EMBENCH_SRC)/support/support.h
	@mkdir -p $$(@D)
	$$(CC) $$(EMBENCH_CFLAGS) -DEMBENCH_NAME='"$(1)"' -c -o $$@ $$<
$(EMB_OUT)/$(1).elf: $$(EMB_OBJS_$(1)) $(EMB_OUT)/$(1)/boardsupport.o \
                     $(EMB_OUT)/support/main.o $(EMB_OUT)/support/beebsc.o \
                     $(BENCH_CRT_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$$(LLD) -T $$(LD_SCRIPT) --gc-sections --wrap=verify_benchmark -o $$@ \
	    $$(RUNTIME_OBJS) $$(LIBC_OBJS) $$(BENCH_CRT_OBJS) $$(EMB_OBJS_$(1)) \
	    $(EMB_OUT)/$(1)/boardsupport.o $(EMB_OUT)/support/main.o \
	    $(EMB_OUT)/support/beebsc.o $$(LIBC_LINK)
	@echo "==> $$@ built"
# Size build: Embench methodology — dummy libc/libm/crt0/compiler-rt, no
# runtime, default lld layout so .text excludes .rodata.
$(EMB_SIZE_OUT)/$(1).elf: $$(EMB_OBJS_$(1)) $(EMB_OUT)/support/boardsupport-size.o \
                          $(EMB_OUT)/support/main-size.o $(EMB_OUT)/support/beebsc.o \
                          $(EMB_DUMMY_OBJS)
	@mkdir -p $$(@D)
	$$(LLD) --gc-sections -e _start -o $$@ $$(EMB_OBJS_$(1)) \
	    $(EMB_OUT)/support/boardsupport-size.o $(EMB_OUT)/support/main-size.o \
	    $(EMB_OUT)/support/beebsc.o $(EMB_DUMMY_OBJS)
embench-$(1): $(EMB_OUT)/$(1).elf $(EMB_SIZE_OUT)/$(1).elf
.PHONY: embench-$(1)
endef
$(foreach b,$(EMBENCH_BENCHES),$(eval $(call embench_rules,$(b))))

embench: $(foreach b,$(EMBENCH_BENCHES),$(EMB_OUT)/$(b).elf $(EMB_SIZE_OUT)/$(b).elf)
	@echo "==> Embench-IoT built in $(EMB_OUT)/ and $(EMB_SIZE_OUT)/"

# Embench size table: .text of each size build (llvm-size -A, the `.text`
# output section only — the Embench "text" metric).
embench-sizes: $(foreach b,$(EMBENCH_BENCHES),$(EMB_SIZE_OUT)/$(b).elf)
	@for b in $(EMBENCH_BENCHES); do \
	    printf '%-16s %s\n' $$b \
	      "$$($(LLVM_BIN)/llvm-size -A $(EMB_SIZE_OUT)/$$b.elf | awk '$$1==".text"{print $$2}')"; \
	done

-include $(wildcard bench-build/*/*/*.d bench-build/*/*/*/*.d)

bench-clean:
	rm -rf bench-build
