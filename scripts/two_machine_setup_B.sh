#!/bin/bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

LAN_IFACE="enp1s0f0np0"
FSO_IFACE="enp1s0f1np1"
SERVER_IP_CIDR="10.10.0.2/24"

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

require_iface() {
    local iface="$1"
    if ! ip link show dev "${iface}" >/dev/null 2>&1; then
        error "Interface does not exist: ${iface}"
        exit 1
    fi
}

flush_existing_ips() {
    step "Flushing existing IP addresses from ${LAN_IFACE}"
    ip addr flush dev "${LAN_IFACE}"
}

assign_ip() {
    step "Assigning ${SERVER_IP_CIDR} to ${LAN_IFACE}"
    ip addr add "${SERVER_IP_CIDR}" dev "${LAN_IFACE}"
}

set_mtu() {
    step "Setting MTU=9000 on ${LAN_IFACE} and ${FSO_IFACE}"
    ip link set dev "${LAN_IFACE}" mtu 9000
    ip link set dev "${FSO_IFACE}" mtu 9000
}

bring_up_interfaces() {
    step "Bringing up ${LAN_IFACE} and ${FSO_IFACE}"
    ip link set dev "${LAN_IFACE}" up
    ip link set dev "${FSO_IFACE}" up
}

print_summary() {
    echo
    success "Topology summary:"
    echo "  Server interface: ${LAN_IFACE} -> ${SERVER_IP_CIDR}"
    echo "  FSO interface:    ${FSO_IFACE} (raw symbols)"
    echo
    ip -brief address show dev "${LAN_IFACE}"
    ip -brief address show dev "${FSO_IFACE}"
    echo
}

main() {
    require_root
    require_project_root
    require_cmd ip
    require_iface "${LAN_IFACE}"
    require_iface "${FSO_IFACE}"

    flush_existing_ips
    assign_ip
    set_mtu
    bring_up_interfaces
    print_summary

    success "Machine B setup complete."
}

main "$@"
