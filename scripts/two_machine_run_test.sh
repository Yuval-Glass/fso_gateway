#!/bin/bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

BURST_LOSS_PCT="${BURST_LOSS_PCT:-0}"
BURST_ENTER_PCT="${BURST_ENTER_PCT:-5}"
BURST_EXIT_PCT="${BURST_EXIT_PCT:-50}"
BASELINE_MODE="${BASELINE_MODE:-0}"
IPERF_DURATION="${IPERF_DURATION:-30}"
IPERF_PARALLEL="${IPERF_PARALLEL:-1}"
IPERF_BITRATE="${IPERF_BITRATE:-100M}"
IPERF_UDP="${IPERF_UDP:-0}"
GW_K="${GW_K:-2}"
GW_M="${GW_M:-1}"
GW_DEPTH="${GW_DEPTH:-2}"
GW_SYMBOL_SIZE="${GW_SYMBOL_SIZE:-750}"
GW_DURATION="${GW_DURATION:-60}"
SERVER_IP="${SERVER_IP:-10.10.0.2}"
FSO_IFACE="${FSO_IFACE:-enp1s0f1np1}"
LAN_IFACE="${LAN_IFACE:-veth-cli-br}"
STATS_DIR="${STATS_DIR:-build/stats}"

CLIENT_NS="ns_client"
GATEWAY_LOG="/tmp/gateway_A.log"
IPERF_CLIENT_LOG="/tmp/iperf3_client.log"

GATEWAY_PID=""
NETEM_APPLIED=0
LATEST_STATS_CSV=""
REPORT_FILE=""
IPERF_SUMMARY_TEXT=""

if command -v tput >/dev/null 2>&1 && [ -t 1 ]; then
    RED="$(tput setaf 1)"
    GREEN="$(tput setaf 2)"
    YELLOW="$(tput setaf 3)"
    BOLD="$(tput bold)"
    RESET="$(tput sgr0)"
else
    RED=""
    GREEN=""
    YELLOW=""
    BOLD=""
    RESET=""
fi

step() {
    echo "${BOLD}==>${RESET} $*"
}

warn() {
    echo "${YELLOW}WARNING:${RESET} $*" >&2
}

error() {
    echo "${RED}ERROR:${RESET} $*" >&2
}

success() {
    echo "${GREEN}$*${RESET}"
}

require_root() {
    if [ "${EUID}" -ne 0 ]; then
        error "This script must be run as root."
        exit 1
    fi
}

require_project_root() {
    if [ ! -d "scripts" ] || [ ! -d "build" ] || [ ! -d "build/bin" ]; then
        error "This script must be run from the fso_gateway project root."
        exit 1
    fi
}

require_cmd() {
    local cmd="$1"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        error "Required command not found: ${cmd}"
        exit 1
    fi
}

require_file() {
    local path="$1"
    if [ ! -e "${path}" ]; then
        error "Required file not found: ${path}"
        exit 1
    fi
}

check_namespace() {
    step "Checking namespace ${CLIENT_NS}"
    if ! ip netns list | awk '{print $1}' | grep -qx "${CLIENT_NS}"; then
        error "Namespace ${CLIENT_NS} does not exist."
        error "Run ./scripts/two_machine_setup_A.sh first."
        exit 1
    fi
}

validate_parameters() {
    step "Validating parameters"

    case "${BASELINE_MODE}" in
        0|1) ;;
        *)
            error "BASELINE_MODE must be 0 or 1."
            exit 1
            ;;
    esac

    case "${IPERF_UDP}" in
        0|1) ;;
        *)
            error "IPERF_UDP must be 0 or 1."
            exit 1
            ;;
    esac

    if [ "${GW_DURATION}" -le "${IPERF_DURATION}" ]; then
        warn "GW_DURATION is not greater than IPERF_DURATION. This may terminate the gateway too early."
    fi

    mkdir -p "${STATS_DIR}"
}

print_mode_banner() {
    echo
    if [ "${BASELINE_MODE}" -eq 1 ]; then
        success "MODE: BASELINE (raw forwarding, no FEC)"
    else
        success "MODE: FSO GATEWAY (FEC + Interleaving active)"
    fi

    if [ "${IPERF_UDP}" -eq 1 ]; then
        success "IPERF MODE: UDP"
    else
        success "IPERF MODE: TCP"
    fi
    echo
}

print_parameters() {
    step "Run parameters"
    echo "  BURST_LOSS_PCT=${BURST_LOSS_PCT}"
    echo "  BURST_ENTER_PCT=${BURST_ENTER_PCT}"
    echo "  BURST_EXIT_PCT=${BURST_EXIT_PCT}"
    echo "  BASELINE_MODE=${BASELINE_MODE}"
    echo "  IPERF_DURATION=${IPERF_DURATION}"
    echo "  IPERF_PARALLEL=${IPERF_PARALLEL}"
    echo "  IPERF_BITRATE=${IPERF_BITRATE}"
    echo "  IPERF_UDP=${IPERF_UDP}"
    echo "  GW_K=${GW_K}"
    echo "  GW_M=${GW_M}"
    echo "  GW_DEPTH=${GW_DEPTH}"
    echo "  GW_SYMBOL_SIZE=${GW_SYMBOL_SIZE}"
    echo "  GW_DURATION=${GW_DURATION}"
    echo "  SERVER_IP=${SERVER_IP}"
    echo "  FSO_IFACE=${FSO_IFACE}"
    echo "  LAN_IFACE=${LAN_IFACE}"
    echo "  STATS_DIR=${STATS_DIR}"
    echo
}

remove_existing_netem() {
    step "Removing any existing netem qdisc on ${FSO_IFACE}"
    tc qdisc del dev "${FSO_IFACE}" root >/dev/null 2>&1 || true
    NETEM_APPLIED=0
}

apply_netem_if_needed() {
    if [ "${BURST_LOSS_PCT}" -gt 0 ]; then
        step "Applying Gilbert-Elliott burst loss on ${FSO_IFACE}"
        echo "  enter=${BURST_ENTER_PCT}% exit=${BURST_EXIT_PCT}% loss=${BURST_LOSS_PCT}%"
        tc qdisc add dev "${FSO_IFACE}" root netem \
            loss gemodel "${BURST_ENTER_PCT}%" "${BURST_EXIT_PCT}%" "${BURST_LOSS_PCT}%" 0%
        NETEM_APPLIED=1
    else
        step "No packet loss applied"
        echo "  Baseline channel run with no netem loss."
    fi
}

clear_old_logs() {
    step "Clearing previous temporary logs"
    rm -f "${GATEWAY_LOG}" "${IPERF_CLIENT_LOG}"
}

start_gateway_A() {
    if [ "${BASELINE_MODE}" -eq 1 ]; then
        step "Starting echo_test on Machine A"
        require_file "./build/bin/echo_test"
        ./build/bin/echo_test \
            --lan-iface "${LAN_IFACE}" \
            --fso-iface "${FSO_IFACE}" \
            --duration "${GW_DURATION}" \
            > "${GATEWAY_LOG}" 2>&1 &
    else
        step "Starting gateway_test on Machine A"
        require_file "./build/bin/gateway_test"
        ./build/bin/gateway_test \
            --lan-iface "${LAN_IFACE}" \
            --fso-iface "${FSO_IFACE}" \
            --duration "${GW_DURATION}" \
            --k "${GW_K}" \
            --m "${GW_M}" \
            --depth "${GW_DEPTH}" \
            --symbol-size "${GW_SYMBOL_SIZE}" \
            > "${GATEWAY_LOG}" 2>&1 &
    fi

    GATEWAY_PID=$!
    sleep 3

    if ! kill -0 "${GATEWAY_PID}" >/dev/null 2>&1; then
        error "Machine A gateway process failed to stay running."
        if [ -f "${GATEWAY_LOG}" ]; then
            echo
            echo "---- ${GATEWAY_LOG} ----" >&2
            tail -n 50 "${GATEWAY_LOG}" >&2 || true
            echo "------------------------" >&2
        fi
        exit 1
    fi
}

build_machine_b_command() {
    if [ "${BASELINE_MODE}" -eq 1 ]; then
        cat <<EOF
sudo ./build/bin/echo_test \\
  --lan-iface enp1s0f0np0 \\
  --fso-iface enp1s0f1np1 \\
  --duration ${GW_DURATION}
EOF
    else
        cat <<EOF
sudo ./build/bin/gateway_test \\
  --lan-iface enp1s0f0np0 \\
  --fso-iface enp1s0f1np1 \\
  --duration ${GW_DURATION} \\
  --k ${GW_K} \\
  --m ${GW_M} \\
  --depth ${GW_DEPTH} \\
  --symbol-size ${GW_SYMBOL_SIZE}
EOF
    fi
}

print_machine_b_instructions() {
    local gateway_cmd
    gateway_cmd="$(build_machine_b_command)"

    echo
    echo "╔══════════════════════════════════════════════════════════════════════╗"
    echo "║  ACTION REQUIRED ON MACHINE B                                      ║"
    echo "║                                                                    ║"
    echo "║  1) Make sure setup was already done:                              ║"
    echo "║     sudo ./scripts/two_machine_setup_B.sh                          ║"
    echo "║                                                                    ║"
    echo "║  2) Start iPerf3 server on Machine B:                              ║"
    echo "║     iperf3 -s                                                      ║"
    echo "║                                                                    ║"
    echo "║  3) Start Machine B gateway now with this exact command:           ║"
    echo "║                                                                    ║"
    echo "╚══════════════════════════════════════════════════════════════════════╝"
    echo
    echo "${gateway_cmd}"
    echo
    echo "Wait for Machine B gateway to print its running message"
    echo "(for gateway_test: \"Both pipeline threads running\") before continuing."
    echo
    read -r -p "Press Enter when Machine B gateway is ready..."
    echo
}

run_iperf_client() {
    step "Running iPerf3 client from namespace ${CLIENT_NS}"

    if [ "${IPERF_UDP}" -eq 1 ]; then
        ip netns exec "${CLIENT_NS}" iperf3 \
            -c "${SERVER_IP}" \
            -u \
            -t "${IPERF_DURATION}" \
            -P "${IPERF_PARALLEL}" \
            -b "${IPERF_BITRATE}" \
            -J \
            --logfile "${IPERF_CLIENT_LOG}"
    else
        ip netns exec "${CLIENT_NS}" iperf3 \
            -c "${SERVER_IP}" \
            -t "${IPERF_DURATION}" \
            -P "${IPERF_PARALLEL}" \
            -b "${IPERF_BITRATE}" \
            -J \
            --logfile "${IPERF_CLIENT_LOG}"
    fi
}

stop_gateway_A() {
    if [ -n "${GATEWAY_PID}" ] && kill -0 "${GATEWAY_PID}" >/dev/null 2>&1; then
        step "Stopping Machine A gateway"
        kill -TERM "${GATEWAY_PID}" >/dev/null 2>&1 || true
        sleep 3
        kill -KILL "${GATEWAY_PID}" >/dev/null 2>&1 || true
    fi
    GATEWAY_PID=""
}

remove_netem_final() {
    step "Removing netem from ${FSO_IFACE}"
    tc qdisc del dev "${FSO_IFACE}" root >/dev/null 2>&1 || true
    NETEM_APPLIED=0
}

print_machine_b_stop_instructions() {
    echo
    echo "Please stop the gateway process manually on Machine B now."
    echo "Also stop the iperf3 server on Machine B if it is still running."
    echo
}

parse_iperf_summary() {
    step "Parsing iPerf3 JSON results"

    if [ ! -f "${IPERF_CLIENT_LOG}" ]; then
        error "iPerf3 client log not found: ${IPERF_CLIENT_LOG}"
        exit 1
    fi

    IPERF_SUMMARY_TEXT="$(
python3 - "${IPERF_CLIENT_LOG}" <<'PY'
import json
import sys

path = sys.argv[1]

def human_bits_per_sec(value):
    units = ["bps", "Kbps", "Mbps", "Gbps", "Tbps"]
    v = float(value)
    idx = 0
    while abs(v) >= 1000.0 and idx < len(units) - 1:
        v /= 1000.0
        idx += 1
    return f"{v:.2f} {units[idx]}"

def human_bytes(value):
    units = ["B", "KB", "MB", "GB", "TB"]
    v = float(value)
    idx = 0
    while abs(v) >= 1024.0 and idx < len(units) - 1:
        v /= 1024.0
        idx += 1
    return f"{v:.2f} {units[idx]}"

with open(path, "r", encoding="utf-8") as fh:
    data = json.load(fh)

end = data.get("end", {})
sum_received = end.get("sum_received", {})
sum_sent = end.get("sum_sent", {})
sum_all = end.get("sum", {})
streams = end.get("streams", [])

throughput_bps = None
bytes_total = None
loss_pct = None
protocol = None

if "udp" in sum_received:
    protocol = "UDP"
    udp = sum_received["udp"]
    throughput_bps = udp.get("bits_per_second")
    bytes_total = udp.get("bytes")
    loss_pct = udp.get("lost_percent")
elif "bits_per_second" in sum_received:
    protocol = "TCP"
    throughput_bps = sum_received.get("bits_per_second")
    bytes_total = sum_received.get("bytes")
elif "bits_per_second" in sum_sent:
    protocol = "TCP"
    throughput_bps = sum_sent.get("bits_per_second")
    bytes_total = sum_sent.get("bytes")
elif "bits_per_second" in sum_all:
    protocol = "TCP"
    throughput_bps = sum_all.get("bits_per_second")
    bytes_total = sum_all.get("bytes")
elif streams:
    protocol = "UNKNOWN"

lines = []
lines.append(f"Protocol: {protocol or 'UNKNOWN'}")

if throughput_bps is not None:
    lines.append(f"Total throughput: {human_bits_per_sec(throughput_bps)}")
else:
    lines.append("Total throughput: unavailable")

if bytes_total is not None:
    lines.append(f"Total data transferred: {human_bytes(bytes_total)}")
else:
    lines.append("Total data transferred: unavailable")

if loss_pct is not None:
    lines.append(f"Packet loss %: {loss_pct:.2f}%")
else:
    lines.append("Packet loss %: N/A (not UDP or unavailable)")

print("\n".join(lines))
PY
)"
}

print_iperf_summary() {
    echo
    echo "=== IPERF3 RESULTS ==="
    echo "${IPERF_SUMMARY_TEXT}"
    echo
}

print_gateway_log_tail() {
    echo "=== GATEWAY A LOG TAIL ==="
    if [ -f "${GATEWAY_LOG}" ]; then
        tail -n 20 "${GATEWAY_LOG}"
    else
        echo "No gateway log found."
    fi
    echo
}

find_latest_stats_csv() {
    LATEST_STATS_CSV="$(find "${STATS_DIR}" -maxdepth 1 -type f -name '*.csv' | xargs -r ls -1t 2>/dev/null | head -n 1 || true)"
}

print_latest_stats_csv() {
    step "Printing most recent CSV from ${STATS_DIR}"
    find_latest_stats_csv
    if [ -n "${LATEST_STATS_CSV}" ] && [ -f "${LATEST_STATS_CSV}" ]; then
        echo "Most recent CSV: ${LATEST_STATS_CSV}"
        cat "${LATEST_STATS_CSV}"
    else
        echo "No CSV files found in ${STATS_DIR}."
    fi
    echo
}

save_report() {
    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    REPORT_FILE="${STATS_DIR}/experiment_${timestamp}.txt"

    step "Saving experiment report to ${REPORT_FILE}"

    find_latest_stats_csv

    {
        echo "FSO Two-Machine Experiment Report"
        echo "Timestamp: $(date --iso-8601=seconds)"
        echo

        if [ "${BASELINE_MODE}" -eq 1 ]; then
            echo "Mode: BASELINE (raw forwarding, no FEC)"
        else
            echo "Mode: FSO GATEWAY (FEC + Interleaving active)"
        fi

        if [ "${IPERF_UDP}" -eq 1 ]; then
            echo "iPerf mode: UDP"
        else
            echo "iPerf mode: TCP"
        fi
        echo

        echo "Parameters:"
        echo "  BURST_LOSS_PCT=${BURST_LOSS_PCT}"
        echo "  BURST_ENTER_PCT=${BURST_ENTER_PCT}"
        echo "  BURST_EXIT_PCT=${BURST_EXIT_PCT}"
        echo "  BASELINE_MODE=${BASELINE_MODE}"
        echo "  IPERF_DURATION=${IPERF_DURATION}"
        echo "  IPERF_PARALLEL=${IPERF_PARALLEL}"
        echo "  IPERF_BITRATE=${IPERF_BITRATE}"
        echo "  IPERF_UDP=${IPERF_UDP}"
        echo "  GW_K=${GW_K}"
        echo "  GW_M=${GW_M}"
        echo "  GW_DEPTH=${GW_DEPTH}"
        echo "  GW_SYMBOL_SIZE=${GW_SYMBOL_SIZE}"
        echo "  GW_DURATION=${GW_DURATION}"
        echo "  SERVER_IP=${SERVER_IP}"
        echo "  FSO_IFACE=${FSO_IFACE}"
        echo "  LAN_IFACE=${LAN_IFACE}"
        echo "  STATS_DIR=${STATS_DIR}"
        echo

        echo "=== IPERF3 SUMMARY ==="
        echo "${IPERF_SUMMARY_TEXT}"
        echo

        echo "=== GATEWAY A LOG ==="
        if [ -f "${GATEWAY_LOG}" ]; then
            cat "${GATEWAY_LOG}"
        else
            echo "No gateway log found."
        fi
        echo

        echo "=== MOST RECENT CSV ==="
        if [ -n "${LATEST_STATS_CSV}" ] && [ -f "${LATEST_STATS_CSV}" ]; then
            echo "File: ${LATEST_STATS_CSV}"
            cat "${LATEST_STATS_CSV}"
        else
            echo "No CSV files found in ${STATS_DIR}."
        fi
        echo
    } > "${REPORT_FILE}"

    success "Experiment report saved: ${REPORT_FILE}"
}

cleanup_on_exit() {
    local rc=$?
    if [ -n "${GATEWAY_PID}" ] && kill -0 "${GATEWAY_PID}" >/dev/null 2>&1; then
        warn "Cleaning up Machine A gateway process"
        kill -TERM "${GATEWAY_PID}" >/dev/null 2>&1 || true
        sleep 3
        kill -KILL "${GATEWAY_PID}" >/dev/null 2>&1 || true
    fi

    if [ "${NETEM_APPLIED}" -eq 1 ]; then
        warn "Cleaning up netem on ${FSO_IFACE}"
        tc qdisc del dev "${FSO_IFACE}" root >/dev/null 2>&1 || true
    fi

    exit "${rc}"
}

trap cleanup_on_exit EXIT

main() {
    require_root
    require_project_root
    require_cmd ip
    require_cmd tc
    require_cmd iperf3
    require_cmd python3

    validate_parameters
    check_namespace
    print_mode_banner
    print_parameters
    clear_old_logs
    remove_existing_netem
    apply_netem_if_needed
    start_gateway_A
    print_machine_b_instructions
    run_iperf_client
    stop_gateway_A
    remove_netem_final
    print_machine_b_stop_instructions
    parse_iperf_summary
    print_iperf_summary
    print_gateway_log_tail
    print_latest_stats_csv
    save_report

    success "Experiment complete."
}

main "$@"
