# mallockit. All paths are relative: the repository may live in a directory
# whose name contains spaces or non-ASCII characters.
UNAME   := $(shell uname -s)
ifeq ($(origin CC),default)
CC      := cc
endif
CXX     ?= c++
PY      ?= python3
JOBS    ?= 4
WERROR  ?=
OPT     ?= -O2

# Sanitizer builds need a clang with the runtimes (Apple's clang lacks TSan
# for some targets; Homebrew LLVM has them all).
ifeq ($(UNAME),Darwin)
SAN_CC  ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang) clang)
SAN_SYSROOT := -isysroot $(shell xcrun --show-sdk-path 2>/dev/null)
SHLIB   := dylib
SHFLAGS := -dynamiclib -install_name @rpath/libmallockit.dylib
PRELOAD := DYLD_INSERT_LIBRARIES
else
SAN_CC  ?= clang
SHLIB   := so
SHFLAGS := -shared
PRELOAD := LD_PRELOAD
PLATDEF := -D_DEFAULT_SOURCE -D_GNU_SOURCE
endif

# The allocator must not let the compiler turn its own code into calls to
# malloc/calloc (e.g. malloc+memset -> calloc).
NOBUILTIN := -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
CFLAGS_BASE := -std=c11 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -fPIC $(PLATDEF) $(WERROR)
LIBFLAGS := $(CFLAGS_BASE) $(NOBUILTIN)
TESTFLAGS := $(CFLAGS_BASE) -Itests
LDLIBS  := -lpthread

LIB_SRC := src/os.c src/segment.c src/page.c src/heap.c src/alloc.c src/debug.c src/override.c
LIB_HDR := src/internal.h include/mallockit.h $(LIB_SRC)
UNITY   := src/mallockit.c

# Variants: rel = API only (static, used by tests and benchmarks),
# ovr = replaces malloc (shared library), dbg/dbgovr = guard mode.
# Each variant is one object compiled from the unity file.
OBJ_rel    := build/obj/rel/mallockit.o
OBJ_ovr    := build/obj/ovr/mallockit.o
OBJ_dbg    := build/obj/dbg/mallockit.o
OBJ_dbgovr := build/obj/dbgovr/mallockit.o

UNIT_SRC := $(wildcard tests/t_*.c) tests/test_main.c
GUARD_SRC := tests/guard/t_guard.c tests/test_main.c

LIBS := build/libmallockit.a build/libmallockit.$(SHLIB) build/libmallockit-debug.a build/libmallockit-debug.$(SHLIB)
BINS := build/test_unit build/test_guard build/smoke build/smoke_cxx build/trace_replay build/ubench

.PHONY: all build test test-unit test-guard test-override lint tsan ubsan asan sanitizers bench bench-quick \
        score page mutants ablation realprogs clean
all: build
build: $(LIBS) $(BINS)

build/obj/rel/%.o: src/%.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) $(OPT) -c $< -o $@
build/obj/ovr/%.o: src/%.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) $(OPT) -DMK_OVERRIDE=1 -c $< -o $@
build/obj/dbg/%.o: src/%.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) -O1 -DMK_DEBUG=1 -c $< -o $@
build/obj/dbgovr/%.o: src/%.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) -O1 -DMK_DEBUG=1 -DMK_OVERRIDE=1 -c $< -o $@

build/libmallockit.a: $(OBJ_rel)
	@rm -f $@
	ar rcs $@ $^
build/libmallockit-debug.a: $(OBJ_dbg)
	@rm -f $@
	ar rcs $@ $^
build/libmallockit.$(SHLIB): $(OBJ_ovr)
	$(CC) $(SHFLAGS) $^ $(LDLIBS) -o $@
build/libmallockit-debug.$(SHLIB): $(OBJ_dbgovr)
	$(CC) $(SHFLAGS) $^ $(LDLIBS) -o $@

build/test_unit: $(UNIT_SRC) tests/test.h build/libmallockit.a
	$(CC) $(TESTFLAGS) -O1 $(UNIT_SRC) build/libmallockit.a $(LDLIBS) -o $@
build/test_guard: $(GUARD_SRC) tests/test.h build/libmallockit-debug.a
	$(CC) $(TESTFLAGS) -O1 -DMK_DEBUG=1 $(GUARD_SRC) build/libmallockit-debug.a $(LDLIBS) -o $@
build/smoke: tests/smoke/smoke.c
	@mkdir -p build
	$(CC) $(CFLAGS_BASE) -O1 $< $(LDLIBS) -ldl -o $@ 2>/dev/null || $(CC) $(CFLAGS_BASE) -O1 $< $(LDLIBS) -o $@
build/smoke_cxx: tests/smoke/smoke_cxx.cpp
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall $< -lpthread -o $@
build/trace_replay: bench/trace_replay.c
	@mkdir -p build
	$(CC) $(CFLAGS_BASE) -O2 $< $(LDLIBS) -o $@
build/ubench: bench/ubench.c build/libmallockit.a
	$(CC) $(CFLAGS_BASE) -O2 $< build/libmallockit.a $(LDLIBS) -o $@

# ------------------------------------------------------------------ tests
test: build test-unit test-guard test-override

test-unit: build/test_unit
	./build/test_unit

test-guard: build/test_guard
	./build/test_guard

# Runs ordinary programs on top of the shared library (and checks that the
# allocator really was in use).
test-override: build/libmallockit.$(SHLIB) build/libmallockit-debug.$(SHLIB) build/smoke build/smoke_cxx
	MALLOCKIT_EXPECT=1 $(PRELOAD)=build/libmallockit.$(SHLIB) ./build/smoke
	MALLOCKIT_EXPECT=1 $(PRELOAD)=build/libmallockit-debug.$(SHLIB) ./build/smoke
	$(PRELOAD)=build/libmallockit.$(SHLIB) ./build/smoke_cxx
	./build/smoke

# ------------------------------------------------------------ sanitizers
# TSan/UBSan/ASan instrument the allocator and the tests (the tests call
# mk_* directly, so the sanitizer's own malloc does not get in the way).
SANFLAGS_tsan  := -fsanitize=thread
SANFLAGS_ubsan := -fsanitize=undefined -fno-sanitize-recover=all
SANFLAGS_asan  := -fsanitize=address -fno-omit-frame-pointer
build/san/%/test_unit: $(LIB_HDR) $(UNIT_SRC) tests/test.h
	@mkdir -p $(dir $@)
	$(SAN_CC) $(SAN_SYSROOT) $(CFLAGS_BASE) $(NOBUILTIN) -Itests -O1 $(SANFLAGS_$*) $(UNITY) $(UNIT_SRC) $(LDLIBS) -o $@
build/san/%/test_guard: $(LIB_HDR) $(GUARD_SRC) tests/test.h
	@mkdir -p $(dir $@)
	$(SAN_CC) $(SAN_SYSROOT) $(CFLAGS_BASE) $(NOBUILTIN) -Itests -O1 -DMK_DEBUG=1 $(SANFLAGS_$*) $(UNITY) $(GUARD_SRC) $(LDLIBS) -o $@

tsan: build/san/tsan/test_unit
	TSAN_OPTIONS="halt_on_error=1" MK_TEST_SCALE=$${MK_TEST_SCALE:-4} ./build/san/tsan/test_unit
ubsan: build/san/ubsan/test_unit build/san/ubsan/test_guard
	./build/san/ubsan/test_unit && ./build/san/ubsan/test_guard
asan: build/san/asan/test_unit build/san/asan/test_guard
	./build/san/asan/test_unit && ./build/san/asan/test_guard
sanitizers: tsan ubsan

# ------------------------------------------------------------------ lint
lint:
	$(MAKE) clean
	$(MAKE) -j$(JOBS) build WERROR=-Werror
	$(PY) -m py_compile bench/*.py analysis/*.py tools/*.py
	sh -n bench/fetch.sh
	bash -n 跑跑看.command

# ------------------------------------------------------------ benchmarks
# Competitors and the mimalloc-bench workloads are fetched into BENCH_DIR
# at pinned commits (never committed here); see bench/fetch.sh.
BENCH_DIR ?= build/ext
bench: build
	sh bench/fetch.sh $(BENCH_DIR)
	$(PY) bench/run.py --ext $(BENCH_DIR) --out results/full

bench-quick: build
	./build/ubench --quick

score: build
	$(PY) bench/score.py --ext $(BENCH_DIR) --out results/score

page:
	$(PY) analysis/page.py --results results --out docs/results.html

mutants:
	$(PY) tools/mutants.py

ablation: build
	$(PY) tools/ablation.py --ext $(BENCH_DIR)

realprogs: build
	$(PY) tools/realprogs.py --ext $(BENCH_DIR)

clean:
	rm -rf build/obj build/san build/*.a build/*.so build/*.dylib build/test_* build/smoke* build/trace_replay build/ubench
