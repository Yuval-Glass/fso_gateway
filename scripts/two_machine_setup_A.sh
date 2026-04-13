#!/bin/bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

CLIENT_NS="ns_client"
VETH_NS="veth-cli"
VETH_MAIN="veth-cli-br"

MAIN_IP_CIDR="10.10.0.254/24"
CLIENT_IP_CIDR="10.10.0.1/24"
CLIENT_GW="10.10.0.254"

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

cleanup_existing() {
    step "Cleaning existing namespace and veth interfaces"
    ip netns del "${CLIENT_NS}" >/dev/null 2>&1 || true
    ip link del "${VETH_MAIN}" >/dev/null 2>&1 || true
    ip link del "${VETH_NS}" >/dev/null 2>&1 || true
}

create_namespace() {
    step "Creating namespace ${CLIENT_NS}"
    ip netns add "${CLIENT_NS}"
}

create_veth_pair() {
    step "Creating veth pair ${VETH_NS} <-> ${VETH_MAIN}"
    ip link add "${VETH_NS}" type veth peer name "${VETH_MAIN}"
}

move_veth_into_namespace() {
    step "Moving ${VETH_NS} into namespace ${CLIENT_NS}"
    ip link set "${VETH_NS}" netns "${CLIENT_NS}"
}

configure_mtu() {
    step "Setting MTU=9000 on ${VETH_MAIN} and ${VETH_NS}"
    ip link set dev "${VETH_MAIN}" mtu 9000
    ip netns exec "${CLIENT_NS}" ip link set dev "${VETH_NS}" mtu 9000
}

assign_addresses() {
    step "Assigning IP addresses"
    ip addr flush dev "${VETH_MAIN}" >/dev/null 2>&1 || true
    ip addr add "${MAIN_IP_CIDR}" dev "${VETH_MAIN}"

    ip netns exec "${CLIENT_NS}" ip addr flush dev "${VETH_NS}" >/dev/null 2>&1 || true
    ip netns exec "${CLIENT_NS}" ip addr add "${CLIENT_IP_CIDR}" dev "${VETH_NS}"
}

bring_interfaces_up() {
    step "Bringing interfaces up"
    ip link set dev "${VETH_MAIN}" up
    ip netns exec "${CLIENT_NS}" ip link set lo up
    ip netns exec "${CLIENT_NS}" ip link set dev "${VETH_NS}" up
}

configure_route() {
    step "Adding default route in ${CLIENT_NS} via ${CLIENT_GW}"
    ip netns exec "${CLIENT_NS}" ip route replace default via "${CLIENT_GW}" dev "${VETH_NS}"
}

enable_ip_forwarding() {
    step "Enabling IPv4 forwarding"
    sysctl -w net.ipv4.ip_forward=1 >/dev/null
}

verify_connectivity() {
    step "Verifying connectivity from ${CLIENT_NS} to ${CLIENT_GW}"
    ip netns exec "${CLIENT_NS}" ping -c 2 -W 1 "${CLIENT_GW}"
}

print_summary() {
    echo
    success "Topology summary:"
    echo "  Namespace:        ${CLIENT_NS}"
    echo "  Main interface:   ${VETH_MAIN} -> ${MAIN_IP_CIDR}"
    echo "  Namespace iface:  ${VETH_NS} -> ${CLIENT_IP_CIDR}"
    echo "  Default gateway:  ${CLIENT_GW}"
    echo
    echo "Main namespace:"
    ip -brief address show dev "${VETH_MAIN}"
    echo
    echo "${CLIENT_NS}:"
    ip netns exec "${CLIENT_NS}" ip -brief address show dev "${VETH_NS}"
    ip netns exec "${CLIENT_NS}" ip route show
    echo
}

main() {
    require_root
    require_project_root
    require_cmd ip
    require_cmd ping
    require_cmd sysctl

    cleanup_existing
    create_namespace
    create_veth_pair
    move_veth_into_namespace
    configure_mtu
    assign_addresses
    bring_interfaces_up
    configure_route
    enable_ip_forwarding
    verify_connectivity
    print_summary

    success "Machine A setup complete."
}

main "$@"
