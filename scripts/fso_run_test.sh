#!/bin/bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

BURST_LOSS_PCT="${BURST_LOSS_PCT:-0}"
BURST_ENTER_PCT="${BURST_ENTER_PCT:-5}"
BURST_EXIT_PCT="${BURST_EXIT_PCT:-50}"
IPERF_DURATION="${IPERF_DURATION:-30}"
IPERF_PARALLEL="${IPERF_PARALLEL:-4}"
IPERF_BITRATE="${IPERF_BITRATE:-100M}"
GW_K="${GW_K:-2}"
GW_M="${GW_M:-1}"
GW_DEPTH="${GW_DEPTH:-2}"
GW_SYMBOL_SIZE="${GW_SYMBOL_SIZE:-750}"
GW_DURATION="${GW_DURATION:-60}"
FSO_IFACE="${FSO_IFACE:-enp1s0f1np1}"
LAN_IFACE="${LAN_IFACE:-enp1s0f0np0}"
STATS_DIR="${STATS_DIR:-build/stats}"
BASELINE_MODE="${BASELINE_MODE:-0}"

CLIENT_NS="fso_client"
SERVER_NS="fso_server"

IPERF_SERVER_LOG="/tmp/iperf3_server.log"
IPERF_CLIENT_LOG="/tmp/iperf3_client.log"
GATEWAY_LOG="/tmp/gateway_test.log"

GATEWAY_PID=""
IPERF_SERVER_STARTED=0
NETEM_APPLIED=0
LATEST_STATS_CSV=""
LATEST_HIST_CSV=""
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

info() {
    echo "$*"
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
        error "This script must be run from the fso_gateway project root directory."
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

cleanup() {
    local exit_code=$?

    step "Cleaning up test processes and netem"

    if [ -n "${GATEWAY_PID}" ] && kill -0 "${GATEWAY_PID}" >/dev/null 2>&1; then
        warn "Stopping forwarding process PID ${GATEWAY_PID}"
        kill -TERM "${GATEWAY_PID}" >/dev/null 2>&1 || true
        sleep 3
        kill -KILL "${GATEWAY_PID}" >/dev/null 2>&1 || true
    fi

    if [ "${IPERF_SERVER_STARTED}" -eq 1 ]; then
        warn "Stopping iperf3 server"
        pkill -TERM -x iperf3 >/dev/null 2>&1 || true
        sleep 1
        pkill -KILL -x iperf3 >/dev/null 2>&1 || true
    fi

    if [ "${NETEM_APPLIED}" -eq 1 ]; then
        warn "Removing netem qdisc from ${FSO_IFACE}"
        tc qdisc del dev "${FSO_IFACE}" root >/dev/null 2>&1 || true
    fi

    exit "${exit_code}"
}

trap cleanup EXIT

check_namespaces() {
    step "Checking required network namespaces"
    if ! ip netns list | awk '{print $1}' | grep -qx "${CLIENT_NS}"; then
        error "Namespace ${CLIENT_NS} does not exist. Run ./scripts/fso_setup.sh first."
        exit 1
    fi
    if ! ip netns list | awk '{print $1}' | grep -qx "${SERVER_NS}"; then
        error "Namespace ${SERVER_NS} does not exist. Run ./scripts/fso_setup.sh first."
        exit 1
    fi
}

validate_parameters() {
    step "Validating run parameters"

    case "${BASELINE_MODE}" in
        0|1) ;;
        *)
            error "BASELINE_MODE must be 0 or 1."
            exit 1
            ;;
    esac

    if [ "${GW_DURATION}" -le $((IPERF_DURATION + 10)) ]; then
        error "GW_DURATION must be greater than IPERF_DURATION + 10."
        error "Current values: GW_DURATION=${GW_DURATION}, IPERF_DURATION=${IPERF_DURATION}"
        exit 1
    fi

    mkdir -p "${STATS_DIR}"
}

print_mode_banner() {
    if [ "${BASELINE_MODE}" -eq 1 ]; then
        success "MODE: BASELINE (raw forwarding, no FEC)"
    else
        success "MODE: FSO GATEWAY (FEC + Interleaving active)"
    fi
}

clear_old_logs() {
    step "Clearing previous temporary log files"
    rm -f "${IPERF_SERVER_LOG}" "${IPERF_CLIENT_LOG}" "${GATEWAY_LOG}"
}

remove_existing_netem() {
    step "Removing any existing netem qdisc from ${FSO_IFACE}"
    tc qdisc del dev "${FSO_IFACE}" root >/dev/null 2>&1 || true
    NETEM_APPLIED=0
}

apply_netem_if_needed() {
    if [ "${BURST_LOSS_PCT}" -gt 0 ]; then
        step "Applying Gilbert-Elliott burst loss model on ${FSO_IFACE}"
        info "netem: enter=${BURST_ENTER_PCT}% exit=${BURST_EXIT_PCT}% burst_loss=${BURST_LOSS_PCT}%"
        tc qdisc add dev "${FSO_IFACE}" root netem \
            loss gemodel "${BURST_ENTER_PCT}%" "${BURST_EXIT_PCT}%" "${BURST_LOSS_PCT}%" 0%
        NETEM_APPLIED=1
    else
        step "No packet loss applied (baseline run)"
        info "No packet loss applied (baseline run)."
    fi
}

start_iperf_server() {
    step "Starting iperf3 server in namespace ${SERVER_NS}"
    pkill -TERM -x iperf3 >/dev/null 2>&1 || true
    sleep 1
    pkill -KILL -x iperf3 >/dev/null 2>&1 || true

    ip netns exec "${SERVER_NS}" iperf3 -s -D --logfile "${IPERF_SERVER_LOG}"
    IPERF_SERVER_STARTED=1
    sleep 1

    if ! pgrep -x iperf3 >/dev/null 2>&1; then
        error "iperf3 server failed to start."
        exit 1
    fi
}

start_forwarder() {
    if [ "${BASELINE_MODE}" -eq 1 ]; then
        step "Starting baseline raw forwarder (echo_test)"
        require_file "./build/bin/echo_test"
        ./build/bin/echo_test \
            --lan-iface "${LAN_IFACE}" \
            --fso-iface "${FSO_IFACE}" \
            --duration "${GW_DURATION}" \
            > "${GATEWAY_LOG}" 2>&1 &
    else
        step "Starting gateway_test"
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
    sleep 2

    if ! kill -0 "${GATEWAY_PID}" >/dev/null 2>&1; then
        error "Forwarding process failed to start. See ${GATEWAY_LOG}"
        [ -f "${GATEWAY_LOG}" ] && tail -n 100 "${GATEWAY_LOG}" >&2 || true
        exit 1
    fi
}

run_iperf_client() {
    step "Running iperf3 client from namespace ${CLIENT_NS}"
    ip netns exec "${CLIENT_NS}" iperf3 \
        -c 10.10.1.1 \
        -t "${IPERF_DURATION}" \
        -P "${IPERF_PARALLEL}" \
        -b "${IPERF_BITRATE}" \
        -J \
        --logfile "${IPERF_CLIENT_LOG}"
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
import math
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
summary = []
throughput_bps = None
bytes_total = None
loss_pct = None
protocol = None

sum_received = end.get("sum_received", {})
sum_sent = end.get("sum_sent", {})
sum_all = end.get("sum", {})
streams = end.get("streams", [])

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

if protocol is None and streams:
    protocol = "UNKNOWN"

summary.append(f"Protocol: {protocol or 'UNKNOWN'}")
if throughput_bps is not None:
    summary.append(f"Total throughput: {human_bits_per_sec(throughput_bps)}")
else:
    summary.append("Total throughput: unavailable")

if bytes_total is not None:
    summary.append(f"Total data transferred: {human_bytes(bytes_total)}")
else:
    summary.append("Total data transferred: unavailable")

if loss_pct is not None:
    summary.append(f"Packet loss %: {loss_pct:.2f}%")
else:
    summary.append("Packet loss %: N/A (not UDP or unavailable)")

print("\n".join(summary))
PY
)"
}

find_latest_stats_files() {
    if [ "${BASELINE_MODE}" -eq 1 ]; then
        LATEST_STATS_CSV=""
        LATEST_HIST_CSV=""
        return 0
    fi

    if [ ! -d "${STATS_DIR}" ]; then
        LATEST_STATS_CSV=""
        LATEST_HIST_CSV=""
        return 0
    fi

    LATEST_HIST_CSV="$(find "${STATS_DIR}" -maxdepth 1 -type f -name '*.csv' | grep -Ei '(hist|burst)' | xargs -r ls -1t 2>/dev/null | head -n 1 || true)"
    LATEST_STATS_CSV="$(find "${STATS_DIR}" -maxdepth 1 -type f -name '*.csv' | grep -Eiv '(hist|burst)' | xargs -r ls -1t 2>/dev/null | head -n 1 || true)"
}

print_results() {
    echo
    echo "=== IPERF3 RESULTS ==="
    echo "${IPERF_SUMMARY_TEXT}"

    echo
    echo "=== GATEWAY STATS ==="
    if [ "${BASELINE_MODE}" -eq 1 ]; then
        echo "Baseline mode: gateway stats collection skipped."
    else
        find_latest_stats_files

        if [ -n "${LATEST_STATS_CSV}" ] && [ -f "${LATEST_STATS_CSV}" ]; then
            echo "--- Most recent stats CSV: ${LATEST_STATS_CSV} ---"
            cat "${LATEST_STATS_CSV}"
        else
            echo "No gateway stats CSV found in ${STATS_DIR}."
        fi

        echo
        if [ -n "${LATEST_HIST_CSV}" ] && [ -f "${LATEST_HIST_CSV}" ]; then
            echo "--- Most recent burst histogram CSV: ${LATEST_HIST_CSV} ---"
            cat "${LATEST_HIST_CSV}"
        else
            echo "No burst histogram CSV found in ${STATS_DIR}."
        fi
    fi
}

save_report() {
    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    REPORT_FILE="${STATS_DIR}/experiment_${timestamp}.txt"

    step "Saving experiment report to ${REPORT_FILE}"

    {
        echo "FSO Experiment Report"
        echo "Timestamp: $(date --iso-8601=seconds)"
        echo

        if [ "${BASELINE_MODE}" -eq 1 ]; then
            echo "Mode: BASELINE (raw forwarding, no FEC)"
        else
            echo "Mode: FSO GATEWAY (FEC + Interleaving active)"
        fi
        echo

        echo "Parameters:"
        echo "  BURST_LOSS_PCT=${BURST_LOSS_PCT}"
        echo "  BURST_ENTER_PCT=${BURST_ENTER_PCT}"
        echo "  BURST_EXIT_PCT=${BURST_EXIT_PCT}"
        echo "  IPERF_DURATION=${IPERF_DURATION}"
        echo "  IPERF_PARALLEL=${IPERF_PARALLEL}"
        echo "  IPERF_BITRATE=${IPERF_BITRATE}"
        echo "  GW_K=${GW_K}"
        echo "  GW_M=${GW_M}"
        echo "  GW_DEPTH=${GW_DEPTH}"
        echo "  GW_SYMBOL_SIZE=${GW_SYMBOL_SIZE}"
        echo "  GW_DURATION=${GW_DURATION}"
        echo "  LAN_IFACE=${LAN_IFACE}"
        echo "  FSO_IFACE=${FSO_IFACE}"
        echo "  STATS_DIR=${STATS_DIR}"
        echo "  BASELINE_MODE=${BASELINE_MODE}"
        echo

        echo "=== IPERF3 SUMMARY ==="
        echo "${IPERF_SUMMARY_TEXT}"
        echo

        echo "=== FORWARDER LOG ==="
        if [ -f "${GATEWAY_LOG}" ]; then
            cat "${GATEWAY_LOG}"
        else
            echo "No forwarder log found."
        fi
        echo

        if [ "${BASELINE_MODE}" -eq 1 ]; then
            echo "=== GATEWAY STATS ==="
            echo "Baseline mode: gateway stats collection skipped."
            echo
        else
            find_latest_stats_files

            echo "=== GATEWAY STATS CSV ==="
            if [ -n "${LATEST_STATS_CSV}" ] && [ -f "${LATEST_STATS_CSV}" ]; then
                echo "File: ${LATEST_STATS_CSV}"
                cat "${LATEST_STATS_CSV}"
            else
                echo "No gateway stats CSV found in ${STATS_DIR}."
            fi
            echo

            echo "=== BURST HISTOGRAM CSV ==="
            if [ -n "${LATEST_HIST_CSV}" ] && [ -f "${LATEST_HIST_CSV}" ]; then
                echo "File: ${LATEST_HIST_CSV}"
                cat "${LATEST_HIST_CSV}"
            else
                echo "No burst histogram CSV found in ${STATS_DIR}."
            fi
            echo
        fi
    } > "${REPORT_FILE}"

    success "Experiment report saved: ${REPORT_FILE}"
}

main() {
    require_root
    require_project_root
    require_cmd ip
    require_cmd tc
    require_cmd iperf3
    require_cmd python3

    validate_parameters
    check_namespaces
    print_mode_banner
    clear_old_logs
    remove_existing_netem
    apply_netem_if_needed
    start_iperf_server
    start_forwarder
    run_iperf_client
    parse_iperf_summary

    step "Stopping forwarding process after iperf3 completion"
    if [ -n "${GATEWAY_PID}" ] && kill -0 "${GATEWAY_PID}" >/dev/null 2>&1; then
        kill -TERM "${GATEWAY_PID}" >/dev/null 2>&1 || true
        sleep 3
        kill -KILL "${GATEWAY_PID}" >/dev/null 2>&1 || true
    fi
    GATEWAY_PID=""

    step "Stopping iperf3 server after client completion"
    pkill -TERM -x iperf3 >/dev/null 2>&1 || true
    sleep 1
    pkill -KILL -x iperf3 >/dev/null 2>&1 || true
    IPERF_SERVER_STARTED=0

    step "Removing netem after test completion"
    tc qdisc del dev "${FSO_IFACE}" root >/dev/null 2>&1 || true
    NETEM_APPLIED=0

    print_results
    save_report

    success "FSO run completed successfully."
}

main "$@"
