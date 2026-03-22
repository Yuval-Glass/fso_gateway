# ==============================================================================
# FSO Gateway — Makefile
# ==============================================================================
#
# Usage:
#   make                  release build  (-O2, no sanitizers)
#   make DEBUG=1          debug build    (-g, -O0, AddressSanitizer)
#   make clean            remove build/
#   make info             print resolved variables
#   make test             build + run FEC stress test
#   make itest            build + run interleaver deterministic-order test
#   make dtest            build + run deinterleaver round-trip test
#   make btest            build + run burst-loss simulation test
#   make check            build + run ALL tests in sequence
#
# Directory contract:
#   src/                  production .c sources  (auto-discovered, recursive)
#   tests/                test-harness .c sources (compiled separately)
#   include/              shared headers
#   third_party/wirehair/ Wirehair library (.cpp + headers)
#   build/src/            production object files  (.o  .d)
#   build/tests/          test object files        (.o)
#   build/                final binaries
#
# Separation guarantee:
#   C_SRCS scans src/ only.  Nothing in tests/ ever enters the gateway binary,
#   so there is no risk of a duplicate-main link error regardless of how many
#   test files are added to tests/.
#
# Adding a new production module:
#   Drop a .c file anywhere under src/ — it is picked up automatically.
#
# Adding a new test binary:
#   1. Add a .c file to tests/.
#   2. Add an explicit link rule in the "Test targets" section below.
#   3. Add the new phony target name to the .PHONY line and to `check`.
# ==============================================================================

# ------------------------------------------------------------------------------
# Toolchain
# ------------------------------------------------------------------------------
CC  := gcc
CXX := g++

# ------------------------------------------------------------------------------
# Directory paths
# ------------------------------------------------------------------------------
TARGET := build/fso_gateway

SRC_DIR      := src
TESTS_DIR    := tests
INC_DIR      := include
BUILD_DIR    := build
WIREHAIR_DIR := third_party/wirehair

# ------------------------------------------------------------------------------
# Source discovery
#
# C_SRCS   — every .c file under src/ (recursive).
#             New modules are picked up automatically; no Makefile edit needed.
#
# CPP_SRCS — every .cpp under third_party/wirehair/ excluding the upstream
#             test / table-generation sub-trees that are not part of the
#             compiled library.
# ------------------------------------------------------------------------------
C_SRCS   := $(shell find $(SRC_DIR) -type f -name '*.c')

CPP_SRCS := $(shell find $(WIREHAIR_DIR) -type f -name '*.cpp' \
                | grep -vE '/(tests?|unit_tests?|tables?)/' )

# ------------------------------------------------------------------------------
# Object and dependency file mapping
#
# Every source file maps to a mirrored path inside build/:
#
#   src/main.c                          -> build/src/main.o
#   src/fec_wrapper.c                   -> build/src/fec_wrapper.o
#   src/interleaver.c                   -> build/src/interleaver.o
#   src/deinterleaver.c                 -> build/src/deinterleaver.o
#   third_party/wirehair/wirehair.cpp   -> build/third_party/wirehair/wirehair.o
# ------------------------------------------------------------------------------
OBJS_C   := $(patsubst %.c,   $(BUILD_DIR)/%.o, $(C_SRCS))
OBJS_CPP := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(CPP_SRCS))
OBJS     := $(OBJS_C) $(OBJS_CPP)

# Dependency files live next to their object files.
DEPS     := $(OBJS:.o=.d)

# ------------------------------------------------------------------------------
# Compiler flags
#
# -MMD -MP   generate .d dependency files as a compilation side-effect;
#            -MP adds phony targets for headers so that a deleted header does
#            not cause a stale-rule error on the next incremental build.
# -march=native
#            Enables Wirehair's GF(256) AVX2/SSE4 SIMD paths.
#            Remove for portable or cross-compiled targets.
# ------------------------------------------------------------------------------
COMMON_FLAGS := \
    -I$(INC_DIR)              \
    -I$(WIREHAIR_DIR)/include \
    -Wall                     \
    -Wextra                   \
    -Wpedantic                \
    -pthread                  \
    -march=native             \
    -MMD -MP

CFLAGS   := -std=c11   $(COMMON_FLAGS)
CXXFLAGS := -std=c++11 $(COMMON_FLAGS)
LDFLAGS  := -pthread

# ------------------------------------------------------------------------------
# Build mode
#
# DEBUG=1   enables -g, -O0, and AddressSanitizer.
#           CFLAGS, CXXFLAGS, and LDFLAGS must all carry -fsanitize=address —
#           the ASan runtime refuses to start if compile and link flags differ.
#
# default   -O2 -DNDEBUG for release throughput.
# ------------------------------------------------------------------------------
DEBUG ?= 0

ifeq ($(DEBUG),1)
    CFLAGS   += -g -O0 -fsanitize=address
    CXXFLAGS += -g -O0 -fsanitize=address
    LDFLAGS  += -fsanitize=address
    $(info [build mode] DEBUG)
else
    CFLAGS   += -O2 -DNDEBUG
    CXXFLAGS += -O2 -DNDEBUG
    $(info [build mode] RELEASE)
endif

# ==============================================================================
# Convenience aliases for frequently referenced object paths
#
# These are used by test link rules to avoid repeating long path strings and
# to make each test's dependency surface explicit and auditable.
# ==============================================================================

OBJ_INTERLEAVER   := $(BUILD_DIR)/$(SRC_DIR)/interleaver.o
OBJ_DEINTERLEAVER := $(BUILD_DIR)/$(SRC_DIR)/deinterleaver.o
OBJ_FEC_WRAPPER   := $(BUILD_DIR)/$(SRC_DIR)/fec_wrapper.o
OBJ_LOGGING       := $(BUILD_DIR)/$(SRC_DIR)/logging.o

# All production C objects except the gateway's own entry point.
# Tests that need the full module set (e.g. FEC stress) link against this.
GATEWAY_C_NO_MAIN := $(filter-out $(BUILD_DIR)/$(SRC_DIR)/main.o, $(OBJS_C))

# ==============================================================================
# Production build
# ==============================================================================

.PHONY: all
all: $(TARGET)

# ------------------------------------------------------------------------------
# Link — must use g++: Wirehair C++ objects are in the link set.
# Using gcc produces undefined references to __cxa_* and std::* symbols.
# ------------------------------------------------------------------------------
$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Linked: $@"

# ------------------------------------------------------------------------------
# Compile — production C sources  (src/**/*.c -> build/src/**/*.o)
# ------------------------------------------------------------------------------
$(BUILD_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------------------
# Compile — Wirehair C++ sources
# ------------------------------------------------------------------------------
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ------------------------------------------------------------------------------
# Dependency inclusion
# -include suppresses errors on a clean build when no .d files exist yet.
# On subsequent builds Make reads these files to discover header dependencies,
# so editing a header triggers recompilation of only the affected .o files.
# ------------------------------------------------------------------------------
-include $(DEPS)

# ==============================================================================
# Test infrastructure
# ==============================================================================

# ------------------------------------------------------------------------------
# Compile — test sources  (tests/*.c -> build/tests/*.o)
#
# A single pattern rule covers every file in tests/.  Each test binary is
# linked explicitly below so its dependency set remains auditable.
# ------------------------------------------------------------------------------
$(BUILD_DIR)/$(TESTS_DIR)/%.o: $(TESTS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------------------
# FEC stress test
#
# Exercises : fec_wrapper + Wirehair encode/decode under increasing symbol loss.
# Links     : all production C objects (no main.o) + Wirehair C++ objects.
# Driver    : g++ — Wirehair C++ objects are in the link set.
#
#   make test           build + run
#   make test DEBUG=1   build + run with ASan
# ------------------------------------------------------------------------------
TEST_TARGET := build/fec_stress_test

$(TEST_TARGET): $(BUILD_DIR)/$(TESTS_DIR)/fec_stress_test.o \
                $(GATEWAY_C_NO_MAIN)                         \
                $(OBJS_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS)
	@echo "Linked: $@"

.PHONY: test
test: $(TEST_TARGET)
	@echo ""
	@echo "--- Running FEC stress test ---"
	@$(TEST_TARGET)

# ------------------------------------------------------------------------------
# Interleaver deterministic-order test
#
# Exercises : interleaver column-major traversal and payload integrity.
# Links     : interleaver.o + logging.o — intentionally hermetic.
# Driver    : gcc — no C++ objects.
#
#   make itest           build + run
#   make itest DEBUG=1   build + run with ASan
# ------------------------------------------------------------------------------
ITEST_TARGET := build/interleaver_test

ITEST_DEPS := \
    $(BUILD_DIR)/$(TESTS_DIR)/interleaver_test.o \
    $(OBJ_INTERLEAVER)                           \
    $(OBJ_LOGGING)

$(ITEST_TARGET): $(ITEST_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)
	@echo "Linked: $@"

.PHONY: itest
itest: $(ITEST_TARGET)
	@echo ""
	@echo "--- Running interleaver deterministic-order test ---"
	@$(ITEST_TARGET)

# ------------------------------------------------------------------------------
# Deinterleaver round-trip test
#
# Exercises : interleaver → deinterleaver full round-trip.
# Links     : deinterleaver.o + interleaver.o + logging.o.
# Driver    : gcc — no C++ objects.
#
#   make dtest           build + run
#   make dtest DEBUG=1   build + run with ASan
# ------------------------------------------------------------------------------
DTEST_TARGET := build/deinterleaver_test

DTEST_DEPS := \
    $(BUILD_DIR)/$(TESTS_DIR)/deinterleaver_test.o \
    $(OBJ_DEINTERLEAVER)                           \
    $(OBJ_INTERLEAVER)                             \
    $(OBJ_LOGGING)

$(DTEST_TARGET): $(DTEST_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)
	@echo "Linked: $@"

.PHONY: dtest
dtest: $(DTEST_TARGET)
	@echo ""
	@echo "--- Running deinterleaver round-trip test ---"
	@$(DTEST_TARGET)

# ------------------------------------------------------------------------------
# Burst-loss simulation test
#
# Exercises : fec_encode_block → interleaver → burst erasure →
#             deinterleaver → fec_decode_block end-to-end.
# Links     : interleaver.o + deinterleaver.o + fec_wrapper.o + logging.o
#             + Wirehair C++ objects.
# Driver    : g++ — Wirehair C++ objects are in the link set.
#
#   make btest           build + run
#   make btest DEBUG=1   build + run with ASan
# ------------------------------------------------------------------------------
BTEST_TARGET := build/burst_sim_test

BTEST_DEPS := \
    $(BUILD_DIR)/$(TESTS_DIR)/burst_sim_test.o \
    $(OBJ_INTERLEAVER)                         \
    $(OBJ_DEINTERLEAVER)                       \
    $(OBJ_FEC_WRAPPER)                         \
    $(OBJ_LOGGING)                             \
    $(OBJS_CPP)

$(BTEST_TARGET): $(BTEST_DEPS)
	@mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS)
	@echo "Linked: $@"

.PHONY: btest
btest: $(BTEST_TARGET)
	@echo ""
	@echo "--- Running burst-loss simulation ---"
	@$(BTEST_TARGET)

# ------------------------------------------------------------------------------
# check — build and run every test in dependency order.
#
# Make aborts on the first non-zero exit, so a failing test blocks the rest.
# Run the full suite under ASan with:  make check DEBUG=1
# ------------------------------------------------------------------------------
.PHONY: check
check: test itest dtest btest
	@echo ""
	@echo "========================================"
	@echo "  All tests passed."
	@echo "========================================"

# ==============================================================================
# Utility targets
# ==============================================================================

# ------------------------------------------------------------------------------
# clean
# ------------------------------------------------------------------------------
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned: $(BUILD_DIR)/"

# ------------------------------------------------------------------------------
# info — verify source discovery and flag resolution before a build.
# Run `make info` or `make info DEBUG=1` before building for the first time.
# ------------------------------------------------------------------------------
.PHONY: info
info:
	@echo "TARGET        : $(TARGET)"
	@echo "TEST_TARGET   : $(TEST_TARGET)"
	@echo "ITEST_TARGET  : $(ITEST_TARGET)"
	@echo "DTEST_TARGET  : $(DTEST_TARGET)"
	@echo "BTEST_TARGET  : $(BTEST_TARGET)"
	@echo "DEBUG         : $(DEBUG)"
	@echo "CC            : $(CC)"
	@echo "CXX           : $(CXX)"
	@echo "CFLAGS        : $(CFLAGS)"
	@echo "CXXFLAGS      : $(CXXFLAGS)"
	@echo "LDFLAGS       : $(LDFLAGS)"
	@echo ""
	@echo "C_SRCS (src/ only) :"
	@$(foreach s, $(C_SRCS),    echo "  $(s)";)
	@echo ""
	@echo "CPP_SRCS :"
	@$(foreach s, $(CPP_SRCS),  echo "  $(s)";)
	@echo ""
	@echo "OBJS :"
	@$(foreach o, $(OBJS),      echo "  $(o)";)
	@echo ""
	@echo "GATEWAY_C_NO_MAIN :"
	@$(foreach o, $(GATEWAY_C_NO_MAIN), echo "  $(o)";)
	@echo ""
	@echo "ITEST_DEPS :"
	@$(foreach o, $(ITEST_DEPS), echo "  $(o)";)
	@echo ""
	@echo "DTEST_DEPS :"
	@$(foreach o, $(DTEST_DEPS), echo "  $(o)";)
	@echo ""
	@echo "BTEST_DEPS :"
	@$(foreach o, $(BTEST_DEPS), echo "  $(o)";)

# ==============================================================================
# Phony declarations
# All targets that do not produce a file of the same name must be listed here.
# ==============================================================================
.PHONY: all clean info test itest dtest btest check