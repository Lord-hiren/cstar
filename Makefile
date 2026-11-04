# cstar — build system.
# Produces build/cstar (CLI driver + frontend + codegen) with the embedded
# runtime sources (build/runtime_embed.h, generated from runtime/pentest_rt.*),
# plus build/pentest, a legacy alias of the same driver (kept until v0.4).

CXX = clang++
CC = clang
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Isrc -Ibuild

# The runtime TU is precompiled at build time with exactly the flags the
# driver uses for its on-demand path, then embedded in `build/cstar`.
# A first-ever `run`/`build` links against that object instead of spending
# ~6 s compiling 8k lines of C (TODO.txt cold-start speed goal). The
# build machine's /proc/cpuinfo `flags` line is embedded as well: the
# driver only uses the object when the host reports the same ISA
# fingerprint, and otherwise falls back to compiling + caching the object.
RT_CFLAGS = -std=c11 -O3 -march=native
RT_OBJ = build/pentest_rt.o
RT_CPU = build/pentest_rt.cpu

SRC = src/main.cpp src/lexer.cpp src/parser.cpp src/semantic.cpp \
      src/diagnostics.cpp src/codegen.cpp src/modules.cpp
OBJ = $(patsubst src/%.cpp,build/%.o,$(SRC))
DEP = $(OBJ:.o=.d)

all: build/cstar build/pentest

build/cstar: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) -lcurl

# Legacy alias (until v0.4): `pentest` runs the same driver. Falls back to a
# copy where symlinks are unavailable.
build/pentest: build/cstar
	ln -sf cstar $@ 2>/dev/null || cp -f build/cstar $@

build/%.o: src/%.cpp build/runtime_embed.h | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

build/embed: tools/embed.cpp | build
	$(CXX) $(CXXFLAGS) -o $@ $<

build/runtime_embed.h: runtime/pentest_rt.c runtime/pentest_rt.h $(RT_OBJ) $(RT_CPU) tools/embed.cpp build/embed | build
	build/embed $@ runtime/pentest_rt.c runtime/pentest_rt.h $(RT_OBJ) $(RT_CPU)

# Precompiled runtime object, embedded into the driver at link time.
$(RT_OBJ): runtime/pentest_rt.c runtime/pentest_rt.h | build
	$(CC) $(RT_CFLAGS) -I runtime -c runtime/pentest_rt.c -o $@

# Build-machine ISA fingerprint for the embedded object. Empty when
# /proc/cpuinfo is unavailable; the driver then compiles the runtime itself.
$(RT_CPU): | build
	@(awk '/^flags[[:space:]]*:/{print; exit}' /proc/cpuinfo 2>/dev/null || true) > $@

build:
	mkdir -p build

test: build/cstar build/pentest
	bash tests/run_tests.sh

clean:
	rm -rf build

.PHONY: all test clean

-include $(DEP)
