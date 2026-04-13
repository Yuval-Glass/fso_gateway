#!/bin/bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

CLIENT_NS="fso_client"
SERVER_NS="fso_server"
FSO_IFACE="enp1s0f1np1"

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
    if [ ! -d "scripts" ] || [ ! -d "build" ]; then
        error "This script must be run from the fso_gateway project root directory."
        exit 1
    fi
}

kill_gateway_processes() {
    step "Stopping any running gateway_test or echo_test processes"
    pkill -TERM -x gateway_test >/dev/null 2>&1 || true
    pkill -TERM -x echo_test >/dev/null 2>&1 || true
    sleep 3
    pkill -KILL -x gateway_test >/dev/null 2>&1 || true
    pkill -KILL -x echo_test >/dev/null 2>&1 || true
}

kill_iperf_processes() {
    step "Stopping any running iperf3 processes"
    pkill -TERM -x iperf3 >/dev/null 2>&1 || true
    sleep 1
    pkill -KILL -x iperf3 >/dev/null 2>&1 || true
}

remove_netem() {
    step "Removing netem qdisc from ${FSO_IFACE}"
    tc qdisc del dev "${FSO_IFACE}" root >/dev/null 2>&1 || true
}

remove_namespaces() {
    step "Deleting namespaces ${CLIENT_NS} and ${SERVER_NS}"
    ip netns del "${CLIENT_NS}" >/dev/null 2>&1 || true
    ip netns del "${SERVER_NS}" >/dev/null 2>&1 || true
}

main() {
    require_root
    require_project_root

    kill_gateway_processes
    kill_iperf_processes
    remove_netem
    remove_namespaces

    success "FSO experiment topology torn down."
}

main "$@"
