# =============================================================================
# Makefile — FSO Gateway
# =============================================================================
#
# Automatic source discovery:  all *.c under src/ are compiled.
# Include path:                include/
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

CC      := gcc
AR      := ar

# =============================================================================
# Directories
# =============================================================================

SRC_DIR   := src
INC_DIR   := include
TEST_DIR  := tests
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
BIN_DIR   := $(BUILD_DIR)/bin

# =============================================================================
# Source files  (automatic discovery)
# =============================================================================

# All production sources in src/
SRCS := $(wildcard $(SRC_DIR)/*.c)

# Exclude main.c when building test binaries (it defines its own main)
SRCS_NO_MAIN := $(filter-out $(SRC_DIR)/main.c, $(SRCS))

OBJS         := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))
OBJS_NO_MAIN := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS_NO_MAIN))

# =============================================================================
# Flags
# =============================================================================

CFLAGS_BASE := -std=c11 \
               -I$(INC_DIR) \
               -Wall \
               -Wextra \
               -Wpedantic \
               -D_POSIX_C_SOURCE=200112L

ifeq ($(DEBUG),1)
    CFLAGS_EXTRA := -g -O0 -fsanitize=address,undefined
    LDFLAGS_EXTRA := -fsanitize=address,undefined
else
    CFLAGS_EXTRA := -O2
    LDFLAGS_EXTRA :=
endif

CFLAGS  := $(CFLAGS_BASE) $(CFLAGS_EXTRA)
LDFLAGS := $(LDFLAGS_EXTRA) -lwirehair -lpthread -lm

ifeq ($(VERBOSE),1)
    Q :=
else
    Q := @
endif

# =============================================================================
# Main binary
# =============================================================================

TARGET := $(BIN_DIR)/fso_gateway

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# =============================================================================
# Object file rule
# =============================================================================

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(Q)echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

# =============================================================================
# Directory creation
# =============================================================================

$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# Test binaries
# =============================================================================

# ---- interleaver_test -------------------------------------------------------
INTERLEAVER_TEST_SRC := $(TEST_DIR)/interleaver_test.c
INTERLEAVER_TEST_BIN := $(BIN_DIR)/interleaver_test
INTERLEAVER_TEST_DEPS := $(OBJ_DIR)/interleaver.o \
                          $(OBJ_DIR)/logging.o

$(INTERLEAVER_TEST_BIN): $(INTERLEAVER_TEST_SRC) $(INTERLEAVER_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---- deinterleaver_test -----------------------------------------------------
DEINTERLEAVER_TEST_SRC := $(TEST_DIR)/deinterleaver_test.c
DEINTERLEAVER_TEST_BIN := $(BIN_DIR)/deinterleaver_test
DEINTERLEAVER_TEST_DEPS := $(OBJ_DIR)/deinterleaver.o \
                            $(OBJ_DIR)/interleaver.o \
                            $(OBJ_DIR)/logging.o

$(DEINTERLEAVER_TEST_BIN): $(DEINTERLEAVER_TEST_SRC) $(DEINTERLEAVER_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---- burst_sim_test ---------------------------------------------------------
BURST_TEST_SRC := $(TEST_DIR)/burst_sim_test.c
BURST_TEST_BIN := $(BIN_DIR)/burst_sim_test
BURST_TEST_DEPS := $(OBJ_DIR)/deinterleaver.o \
                   $(OBJ_DIR)/fec_wrapper.o \
                   $(OBJ_DIR)/interleaver.o \
                   $(OBJ_DIR)/logging.o

$(BURST_TEST_BIN): $(BURST_TEST_SRC) $(BURST_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---- receiver_robustness_test -----------------------------------------------
ROBUSTNESS_TEST_SRC := $(TEST_DIR)/receiver_robustness_test.c
ROBUSTNESS_TEST_BIN := $(BIN_DIR)/receiver_robustness_test
ROBUSTNESS_TEST_DEPS := $(OBJ_DIR)/deinterleaver.o \
                         $(OBJ_DIR)/fec_wrapper.o \
                         $(OBJ_DIR)/logging.o

$(ROBUSTNESS_TEST_BIN): $(ROBUSTNESS_TEST_SRC) $(ROBUSTNESS_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---- fec_stress_test --------------------------------------------------------
FEC_STRESS_TEST_SRC := $(TEST_DIR)/fec_stress_test.c
FEC_STRESS_TEST_BIN := $(BIN_DIR)/fec_stress_test
FEC_STRESS_TEST_DEPS := $(OBJ_DIR)/fec_wrapper.o \
                         $(OBJ_DIR)/logging.o

$(FEC_STRESS_TEST_BIN): $(FEC_STRESS_TEST_SRC) $(FEC_STRESS_TEST_DEPS) | $(BIN_DIR)
	$(Q)echo "  LINK  $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# =============================================================================
# Grouped test target
# =============================================================================

.PHONY: tests
tests: $(INTERLEAVER_TEST_BIN) \
       $(DEINTERLEAVER_TEST_BIN) \
       $(BURST_TEST_BIN) \
       $(ROBUSTNESS_TEST_BIN) \
       $(FEC_STRESS_TEST_BIN)

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

.PHONY: alltest
alltest: tests itest dtest btest rtest ftest

# =============================================================================
# Clean
# =============================================================================

.PHONY: clean
clean:
	$(Q)echo "  CLEAN $(BUILD_DIR)"
	$(Q)rm -rf $(BUILD_DIR)
