#!/bin/bash
#
# scripts/run_dpdk_gateway.sh — Start the FSO gateway in DPDK 10 Gbps mode.
#
# Usage:
#   sudo ./scripts/run_dpdk_gateway.sh [--side A|B] [options]
#
# Environment overrides (or edit defaults below):
#   LAN_IFACE       kernel name of the LAN NIC   (e.g. enp3s0f0)
#   FSO_IFACE       kernel name of the FSO NIC   (e.g. enp3s0f1)
#   GW_K            FEC source symbols            (default 10)
#   GW_M            FEC repair symbols            (default 4)
#   GW_DEPTH        interleaver depth             (default 8)
#   GW_SYMBOL_SIZE  symbol size in bytes          (default 1514 = max standard Ethernet frame)
#   GW_DURATION     run duration in seconds, 0=forever (default 0)
#   DPDK_CORES      CPU list for EAL -l flag      (default 0-3)
#   HUGEPAGES_2M    number of 2 MB hugepages to allocate (default 1024)
#
# Operator setup (one time, per machine reboot):
#   sudo ./scripts/run_dpdk_gateway.sh --setup-hugepages
#
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
LAN_IFACE="${LAN_IFACE:-}"
FSO_IFACE="${FSO_IFACE:-}"
GW_K="${GW_K:-10}"
GW_M="${GW_M:-4}"
GW_DEPTH="${GW_DEPTH:-8}"
GW_SYMBOL_SIZE="${GW_SYMBOL_SIZE:-1514}"
DPDK_CORES="${DPDK_CORES:-0-3}"
HUGEPAGES_2M="${HUGEPAGES_2M:-1024}"
SIDE=""
SETUP_HUGEPAGES_ONLY=0

BINARY="./build/bin/fso_gw_runner_dpdk"

# ---------------------------------------------------------------------------
# Colour helpers
# ---------------------------------------------------------------------------
if command -v tput >/dev/null 2>&1 && [ -t 1 ]; then
    RED="$(tput setaf 1)"; GREEN="$(tput setaf 2)"
    YELLOW="$(tput setaf 3)"; BOLD="$(tput bold)"; RESET="$(tput sgr0)"
else
    RED=""; GREEN=""; YELLOW=""; BOLD=""; RESET=""
fi

step()    { echo "${BOLD}==>${RESET} $*"; }
warn()    { echo "${YELLOW}WARNING:${RESET} $*" >&2; }
error()   { echo "${RED}ERROR:${RESET} $*" >&2; }
success() { echo "${GREEN}$*${RESET}"; }

# ---------------------------------------------------------------------------
# Hugepage setup
# ---------------------------------------------------------------------------
setup_hugepages() {
    step "Configuring ${HUGEPAGES_2M} × 2 MB hugepages"
    echo "${HUGEPAGES_2M}" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

    local actual
    actual=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if [ "${actual}" -lt "${HUGEPAGES_2M}" ]; then
        warn "Only ${actual} hugepages allocated (requested ${HUGEPAGES_2M})."
        warn "Try: echo ${HUGEPAGES_2M} | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
    else
        success "Hugepages configured: ${actual} × 2 MB = $((actual * 2)) MB reserved"
    fi
}

# ---------------------------------------------------------------------------
# PCI address helpers
# ---------------------------------------------------------------------------
get_pci() {
    local iface="$1"
    local path="/sys/class/net/${iface}/device"
    if [ ! -e "${path}" ]; then
        error "Interface '${iface}' not found in sysfs."
        exit 1
    fi
    readlink -f "${path}" | xargs basename
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --side)          SIDE="$2"; shift 2 ;;
            --lan-iface)     LAN_IFACE="$2"; shift 2 ;;
            --fso-iface)     FSO_IFACE="$2"; shift 2 ;;
            --k)             GW_K="$2"; shift 2 ;;
            --m)             GW_M="$2"; shift 2 ;;
            --depth)         GW_DEPTH="$2"; shift 2 ;;
            --symbol-size)   GW_SYMBOL_SIZE="$2"; shift 2 ;;
            --cores)         DPDK_CORES="$2"; shift 2 ;;
            --hugepages)     HUGEPAGES_2M="$2"; shift 2 ;;
            --setup-hugepages)
                SETUP_HUGEPAGES_ONLY=1; shift ;;
            -h|--help)
                print_help; exit 0 ;;
            *)
                error "Unknown option: $1"; exit 1 ;;
        esac
    done

    # Side presets for two-machine setup
    if [ "${SIDE}" = "A" ]; then
        LAN_IFACE="${LAN_IFACE:-enp1s0f0np0}"
        FSO_IFACE="${FSO_IFACE:-enp1s0f1np1}"
    elif [ "${SIDE}" = "B" ]; then
        LAN_IFACE="${LAN_IFACE:-enp1s0f0np0}"
        FSO_IFACE="${FSO_IFACE:-enp1s0f1np1}"
    fi
}

print_help() {
    cat <<EOF
Usage: sudo $0 [options]

Options:
  --side A|B          Preset LAN/FSO interfaces for machine A or B
  --lan-iface <iface> LAN NIC kernel interface name
  --fso-iface <iface> FSO NIC kernel interface name
  --k N               FEC source symbols (default ${GW_K})
  --m N               FEC repair symbols (default ${GW_M})
  --depth N           Interleaver depth (default ${GW_DEPTH})
  --symbol-size N     Symbol size bytes (default ${GW_SYMBOL_SIZE})
  --cores LIST        EAL CPU list (default "${DPDK_CORES}")
  --hugepages N       2MB hugepage count (default ${HUGEPAGES_2M})
  --setup-hugepages   Only configure hugepages, then exit
  -h, --help          Show this help

Environment variables (same names, take precedence over script defaults):
  LAN_IFACE, FSO_IFACE, GW_K, GW_M, GW_DEPTH, GW_SYMBOL_SIZE,
  GW_DURATION, DPDK_CORES, HUGEPAGES_2M

Example (Machine A):
  sudo $0 --side A

Example (Machine B):
  sudo $0 --side B

Example (explicit interfaces):
  sudo $0 --lan-iface eth0 --fso-iface eth1 --k 10 --m 4 --depth 8
EOF
}

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
preflight() {
    if [ "${EUID}" -ne 0 ]; then
        error "Must be run as root (needed for DPDK EAL and hugepages)."
        exit 1
    fi

    if [ -z "${LAN_IFACE}" ] || [ -z "${FSO_IFACE}" ]; then
        error "LAN_IFACE and FSO_IFACE must be set."
        error "Use --side A|B or --lan-iface / --fso-iface."
        exit 1
    fi

    if [ ! -f "${BINARY}" ]; then
        error "DPDK gateway binary not found: ${BINARY}"
        error "Build it with:  make USE_DPDK=1 runner"
        exit 1
    fi

    local hp_count
    hp_count=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
    if [ "${hp_count}" -lt 64 ]; then
        warn "Only ${hp_count} hugepages available. DPDK requires at least 64."
        warn "Running hugepage setup now..."
        setup_hugepages
    fi

    # Enable jumbo frames on the FSO link so FEC wire frames (FEC header 18B +
    # symbol payload) are not truncated by the NIC.  The mlx5 bifurcated PMD
    # honours the kernel MTU even after DPDK takes the port.
    step "Setting FSO NIC MTU to 9000 (jumbo frames)"
    if ip link set "${FSO_IFACE}" mtu 9000 2>/dev/null; then
        success "  ${FSO_IFACE} MTU → 9000"
    else
        warn "  Could not set MTU on ${FSO_IFACE} — large frames may be dropped"
    fi
}

# ---------------------------------------------------------------------------
# Show PCI addresses
# ---------------------------------------------------------------------------
show_pci_info() {
    local lan_pci fso_pci
    lan_pci=$(get_pci "${LAN_IFACE}" 2>/dev/null || echo "unknown")
    fso_pci=$(get_pci "${FSO_IFACE}" 2>/dev/null || echo "unknown")

    step "NIC configuration"
    echo "  LAN: ${LAN_IFACE}  PCI=${lan_pci}"
    echo "  FSO: ${FSO_IFACE}  PCI=${fso_pci}"
    echo
    echo "  (override PCI detection via FSO_LAN_PCI / FSO_FSO_PCI env vars)"
    echo

    # Export so packet_io_dpdk.c sysfs lookup can be skipped if user wants
    # (the C code reads these env vars first)
    export FSO_LAN_PCI="${lan_pci}"
    export FSO_FSO_PCI="${fso_pci}"
}

# ---------------------------------------------------------------------------
# Print configuration summary
# ---------------------------------------------------------------------------
print_config() {
    step "DPDK gateway configuration"
    echo "  Binary:       ${BINARY}"
    echo "  LAN iface:    ${LAN_IFACE}"
    echo "  FSO iface:    ${FSO_IFACE}"
    echo "  k=${GW_K} m=${GW_M} depth=${GW_DEPTH} symbol_size=${GW_SYMBOL_SIZE}"
      echo "  DPDK cores:   ${DPDK_CORES}"
    local hp_count
    hp_count=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    echo "  Hugepages:    ${hp_count} × 2 MB"
    echo
}

# ---------------------------------------------------------------------------
# Launch
# ---------------------------------------------------------------------------
run_gateway() {
    export DPDK_CORES="${DPDK_CORES}"

    success "Starting FSO gateway (DPDK mode)..."
    echo

    exec "${BINARY}" \
        --lan-iface "${LAN_IFACE}" \
        --fso-iface "${FSO_IFACE}" \
        --k "${GW_K}" \
        --m "${GW_M}" \
        --depth "${GW_DEPTH}" \
        --symbol-size "${GW_SYMBOL_SIZE}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
parse_args "$@"

if [ "${SETUP_HUGEPAGES_ONLY}" -eq 1 ]; then
    setup_hugepages
    success "Hugepage setup complete."
    exit 0
fi

preflight
show_pci_info
print_config
run_gateway
