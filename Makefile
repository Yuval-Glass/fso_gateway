# =============================================================================
# Makefile — FSO Gateway
# =============================================================================
#
# Automatic source discovery:  all *.c under src/ are compiled.
# Include paths:               include/  third_party/wirehair/include/
# Wirehair:                    compiled from third_party/wirehair/*.cpp
#                              (no system libwirehair required)
# Build output:                build/
#
# Targets
# -------
#   make              — build the main gateway binary
#   make tests        — build all test binaries
#   make itest        — run interleaver_test
#   make dtest        — run deinterleaver_test
#   make btest        — run burst_sim_test (all modes)
#   make rtest        — run receiver_robustness_test
#   make ftest        — run fec_stress_test
#   make e2etest      — build and run end_to_end_sim_test (Task 20)
#   make ctest        — build and run channel_campaign_test
#   make alltest      — build and run all tests
#   make clean        — remove build directory
#
# Flags
# -----
#   make DEBUG=1      — add -g -fsanitize=address,undefined
#   make VERBOSE=1    — echo full compiler commands

# =============================================================================
# Toolchain
# =============================================================================

CC  := gcc
CXX := g++
AR  := ar

# =============================================================================
# Directories
# =============================================================================

SRC_DIR           := src
INC_DIR           := include
TEST_DIR          := tests
WIREHAIR_DIR      := third_party/wirehair
WIREHAIR_INC      := $(WIREHAIR_DIR)/include
BUILD_DIR         := build
OBJ_DIR           := $(BUILD_DIR)/obj
BIN_DIR           := $(BUILD_DIR)/bin
WIREHAIR_OBJS_DIR := $(OBJ_DIR)/wirehair
TEST_OBJS_DIR     := $(OBJ_DIR)/tests

# =============================================================================
# Source files
# =============================================================================

SRCS         := $(wildcard $(SRC_DIR)/*.c)
SRCS_NO_MAIN := $(filter-out $(SRC_DIR)/main.c, $(SRCS))

OBJS         := $(patsubst $(SRC_DIR)/%.c,         $(OBJ_DIR)/%.o,           $(SRCS))
OBJS_NO_MAIN := $(patsubst $(SRC_DIR)/%.c,         $(OBJ_DIR)/%.o,           $(SRCS_NO_MAIN))

WIREHAIR_SRCS := $(wildcard $(WIREHAIR_DIR)/*.cpp)
WIREHAIR_OBJS := $(patsubst $(WIREHAIR_DIR)/%.cpp, $(WIREHAIR_OBJS_DIR)/%.o, $(WIREHAIR_SRCS))

TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_OBJS := $(patsubst $(TEST_DIR)/%.c,           $(TEST_OBJS_DIR)/%.o,     $(TEST_SRCS))

# =============================================================================
# Compiler flags
# =============================================================================

CFLAGS_BASE := -std=c11 \
               -I$(INC_DIR) \
               -I$(WIREHAIR_INC) \
               -Wall \
               -Wextra \
               -Wpedantic \
               -D_POSIX_C_SOURCE=200112L

CXXFLAGS_BASE := -std=c++11 \
                 -I$(INC_DIR) \
                 -I$(WIREHAIR_INC) \
                 -O2

# gf256.cpp uses _mm_shuffle_epi8 which is an SSSE3 intrinsic.
# Applied only to the Wirehair compilation rule.
WIREHAIR_CXXFLAGS_EXTRA := -mssse3

ifeq ($(DEBUG),1)
    CFLAGS_EXTRA   := -g -O0 -fsanitize=address,undefined
    CXXFLAGS_EXTRA := -g -O0 -fsanitize=address,undefined
    LDFLAGS_EXTRA  := -fsanitize=address,undefined
else
    CFLAGS_EXTRA   := -O2
    CXXFLAGS_EXTRA :=
    LDFLAGS_EXTRA  :=
endif

CFLAGS   := $(CFLAGS_BASE)   $(CFLAGS_EXTRA)
CXXFLAGS := $(CXXFLAGS_BASE) $(CXXFLAGS_EXTRA)

# Link with g++ so C++ objects (Wirehair) resolve correctly.
LDFLAGS := $(LDFLAGS_EXTRA) -lpthread -lm -lstdc++ -lpcap

# =============================================================================
# DPDK mode — make USE_DPDK=1
# =============================================================================
# When USE_DPDK=1:
#   - Compiles packet_io_dpdk.c instead of packet_io.c (guarded by macro)
#   - Adds DPDK include paths and link flags
#   - Strips -lpcap from LDFLAGS (not needed in DPDK mode)
#   - Output binary: build/bin/fso_gateway_dpdk
#
# Prerequisites:
#   sudo apt-get install dpdk-dev
#   echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
#
ifeq ($(USE_DPDK),1)
    DPDK_CFLAGS  := -I/usr/include/dpdk \
                    -I/usr/include/x86_64-linux-gnu/dpdk \
                    -DUSE_DPDK_BUILD \
                    -march=native
    DPDK_LDFLAGS := $(shell pkg-config --libs libdpdk 2>/dev/null)

    CFLAGS  := $(filter-out -Wpedantic, $(CFLAGS)) $(DPDK_CFLAGS)
    LDFLAGS := $(filter-out -lpcap, $(LDFLAGS)) $(DPDK_LDFLAGS)
    OBJ_DIR := $(BUILD_DIR)/obj_dpdk
    OBJS    := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))
    OBJS_NO_MAIN := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS_NO_MAIN))
endif

ifeq ($(VERBOSE),1)
    Q :=
else
    Q := @
endif

# =============================================================================
# Main binary
# =============================================================================

ifeq ($(USE_DPDK),1)
TARGET         := $(BIN_DIR)/fso_gateway_dpdk
GW_RUNNER_BIN  := $(BIN_DIR)/fso_gw_runner_dpdk
else
TARGET         := $(BIN_DIR)/fso_gateway
GW_RUNNER_BIN  := $(BIN_DIR)/fso_gw_runner
endif

.PHONY: all
ifeq ($(USE_DPDK),1)
all: $(TARGET) $(GW_RUNNER_BIN)
else
all: $(TARGET)
endif

$(TARGET): $(OBJS) $(WIREHAIR_OBJS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# =============================================================================
# Object file rules
# =============================================================================

# C sources under src/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(Q)echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

# Wirehair C++ sources — SSSE3 required for gf256.cpp
$(WIREHAIR_OBJS_DIR)/%.o: $(WIREHAIR_DIR)/%.cpp | $(WIREHAIR_OBJS_DIR)
	$(Q)echo "  CXX   $<"
	$(Q)$(CXX) $(CXXFLAGS) $(WIREHAIR_CXXFLAGS_EXTRA) -c -o $@ $<

# Test C sources
$(TEST_OBJS_DIR)/%.o: $(TEST_DIR)/%.c | $(TEST_OBJS_DIR)
	$(Q)echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

# =============================================================================
# Directory creation
# =============================================================================

$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(WIREHAIR_OBJS_DIR) $(TEST_OBJS_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# Test binaries
# =============================================================================

# ---- interleaver_test -------------------------------------------------------
INTERLEAVER_TEST_BIN  := $(BIN_DIR)/interleaver_test
INTERLEAVER_TEST_OBJ  := $(TEST_OBJS_DIR)/interleaver_test.o
INTERLEAVER_TEST_DEPS := $(OBJ_DIR)/interleaver.o \
                          $(OBJ_DIR)/logging.o

$(INTERLEAVER_TEST_BIN): $(INTERLEAVER_TEST_OBJ) $(INTERLEAVER_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# ---- deinterleaver_test -----------------------------------------------------
DEINTERLEAVER_TEST_BIN  := $(BIN_DIR)/deinterleaver_test
DEINTERLEAVER_TEST_OBJ  := $(TEST_OBJS_DIR)/deinterleaver_test.o
DEINTERLEAVER_TEST_DEPS := $(OBJ_DIR)/deinterleaver.o \
                            $(OBJ_DIR)/interleaver.o \
                            $(OBJ_DIR)/logging.o

$(DEINTERLEAVER_TEST_BIN): $(DEINTERLEAVER_TEST_OBJ) $(DEINTERLEAVER_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# ---- burst_sim_test ---------------------------------------------------------
BURST_TEST_BIN  := $(BIN_DIR)/burst_sim_test
BURST_TEST_OBJ  := $(TEST_OBJS_DIR)/burst_sim_test.o
BURST_TEST_DEPS := $(OBJ_DIR)/deinterleaver.o \
                   $(OBJ_DIR)/fec_wrapper.o \
                   $(OBJ_DIR)/interleaver.o \
                   $(OBJ_DIR)/logging.o \
                   $(OBJ_DIR)/symbol.o \
                   $(WIREHAIR_OBJS)

$(BURST_TEST_BIN): $(BURST_TEST_OBJ) $(BURST_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# ---- receiver_robustness_test -----------------------------------------------
ROBUSTNESS_TEST_BIN  := $(BIN_DIR)/receiver_robustness_test
ROBUSTNESS_TEST_OBJ  := $(TEST_OBJS_DIR)/receiver_robustness_test.o
ROBUSTNESS_TEST_DEPS := $(OBJ_DIR)/deinterleaver.o \
                         $(OBJ_DIR)/fec_wrapper.o \
                         $(OBJ_DIR)/logging.o \
                         $(OBJ_DIR)/symbol.o \
                         $(WIREHAIR_OBJS)

$(ROBUSTNESS_TEST_BIN): $(ROBUSTNESS_TEST_OBJ) $(ROBUSTNESS_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# ---- fec_stress_test --------------------------------------------------------
FEC_STRESS_TEST_BIN  := $(BIN_DIR)/fec_stress_test
FEC_STRESS_TEST_OBJ  := $(TEST_OBJS_DIR)/fec_stress_test.o
FEC_STRESS_TEST_DEPS := $(OBJ_DIR)/fec_wrapper.o \
                         $(OBJ_DIR)/logging.o \
                         $(WIREHAIR_OBJS)

$(FEC_STRESS_TEST_BIN): $(FEC_STRESS_TEST_OBJ) $(FEC_STRESS_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# ---- end_to_end_sim_test  (Task 20) ----------------------------------------
E2E_TEST_BIN  := $(BIN_DIR)/end_to_end_sim_test
E2E_TEST_OBJ  := $(TEST_OBJS_DIR)/end_to_end_sim_test.o
SIM_RUNNER_OBJ := $(TEST_OBJS_DIR)/sim_runner.o
E2E_TEST_DEPS := $(OBJ_DIR)/block_builder.o \
                  $(OBJ_DIR)/deinterleaver.o \
                  $(OBJ_DIR)/fec_wrapper.o \
                  $(OBJ_DIR)/interleaver.o \
                  $(OBJ_DIR)/logging.o \
                  $(OBJ_DIR)/packet_fragmenter.o \
                  $(OBJ_DIR)/packet_reassembler.o \
                  $(OBJ_DIR)/stats.o \
                  $(OBJ_DIR)/symbol.o \
                  $(SIM_RUNNER_OBJ) \
                  $(WIREHAIR_OBJS)

$(E2E_TEST_BIN): $(E2E_TEST_OBJ) $(E2E_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# ---- channel_campaign_test --------------------------------------------------
CHANNEL_CAMPAIGN_TEST_BIN  := $(BIN_DIR)/channel_campaign_test
CHANNEL_CAMPAIGN_TEST_OBJ  := $(TEST_OBJS_DIR)/channel_campaign_test.o
CHANNEL_CAMPAIGN_TEST_DEPS := $(OBJ_DIR)/block_builder.o \
                               $(OBJ_DIR)/deinterleaver.o \
                               $(OBJ_DIR)/fec_wrapper.o \
                               $(OBJ_DIR)/interleaver.o \
                               $(OBJ_DIR)/logging.o \
                               $(OBJ_DIR)/packet_fragmenter.o \
                               $(OBJ_DIR)/packet_reassembler.o \
                               $(OBJ_DIR)/stats.o \
                               $(OBJ_DIR)/symbol.o \
                               $(SIM_RUNNER_OBJ) \
                               $(WIREHAIR_OBJS)

$(CHANNEL_CAMPAIGN_TEST_BIN): $(CHANNEL_CAMPAIGN_TEST_OBJ) $(CHANNEL_CAMPAIGN_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

# =============================================================================
# Grouped test target
# =============================================================================

.PHONY: tests
tests: $(INTERLEAVER_TEST_BIN) \
       $(DEINTERLEAVER_TEST_BIN) \
       $(BURST_TEST_BIN) \
       $(ROBUSTNESS_TEST_BIN) \
       $(FEC_STRESS_TEST_BIN) \
       $(E2E_TEST_BIN) \
       $(CHANNEL_CAMPAIGN_TEST_BIN)

# =============================================================================
# Run targets
# =============================================================================

.PHONY: itest
itest: $(INTERLEAVER_TEST_BIN)
	$(Q)echo ""
	$(Q)echo "Running interleaver_test..."
	$(Q)$(INTERLEAVER_TEST_BIN)

.PHONY: dtest
dtest: $(DEINTERLEAVER_TEST_BIN)
	$(Q)echo ""
	$(Q)echo "Running deinterleaver_test..."
	$(Q)$(DEINTERLEAVER_TEST_BIN)

.PHONY: btest
btest: $(BURST_TEST_BIN)
	$(Q)echo ""
	$(Q)echo "Running burst_sim_test (all modes)..."
	$(Q)$(BURST_TEST_BIN) all

.PHONY: rtest
rtest: $(ROBUSTNESS_TEST_BIN)
	$(Q)echo ""
	$(Q)echo "Running receiver_robustness_test..."
	$(Q)$(ROBUSTNESS_TEST_BIN)

.PHONY: ftest
ftest: $(FEC_STRESS_TEST_BIN)
	$(Q)echo ""
	$(Q)echo "Running fec_stress_test..."
	$(Q)$(FEC_STRESS_TEST_BIN)

.PHONY: e2etest
e2etest: $(E2E_TEST_BIN)
	$(Q)echo ""
	$(Q)echo "Running end_to_end_sim_test (Task 20)..."
	$(Q)$(E2E_TEST_BIN)

.PHONY: ctest
ctest: $(CHANNEL_CAMPAIGN_TEST_BIN)
	$(Q)echo ""
	$(Q)echo "Running channel_campaign_test..."
	$(Q)$(CHANNEL_CAMPAIGN_TEST_BIN)

.PHONY: alltest
alltest: tests itest dtest btest rtest ftest e2etest ctest

# =============================================================================
# Clean
# =============================================================================

.PHONY: clean
clean:
	$(Q)echo "  CLEAN $(BUILD_DIR)"
	$(Q)rm -rf $(BUILD_DIR)
# =============================================================================
# fso_gw_runner — gateway runner binary
# =============================================================================

TOOLS_DIR := tools
TOOLS_OBJS_DIR := $(OBJ_DIR)/tools

$(TOOLS_OBJS_DIR)/%.o: $(TOOLS_DIR)/%.c | $(TOOLS_OBJS_DIR)
	$(Q)echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

GW_RUNNER_OBJ  := $(TOOLS_OBJS_DIR)/fso_gw_runner.o
GW_RUNNER_DEPS := $(OBJS_NO_MAIN) $(WIREHAIR_OBJS)

$(TOOLS_OBJS_DIR):
	$(Q)mkdir -p $@

$(GW_RUNNER_BIN): $(GW_RUNNER_OBJ) $(GW_RUNNER_DEPS) | $(BIN_DIR) $(TOOLS_OBJS_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CXX) $(LDFLAGS_EXTRA) -o $@ $^ $(LDFLAGS)

.PHONY: runner
runner: $(GW_RUNNER_BIN)
