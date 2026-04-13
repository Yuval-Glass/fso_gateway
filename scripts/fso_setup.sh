#!/bin/bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

CLIENT_NS="fso_client"
SERVER_NS="fso_server"

CLIENT_VETH_NS="veth-cli"
CLIENT_VETH_MAIN="veth-cli-br"
SERVER_VETH_NS="veth-srv"
SERVER_VETH_MAIN="veth-srv-br"

LAN_IFACE="enp1s0f0np0"
FSO_IFACE="enp1s0f1np1"

CLIENT_IP="10.10.0.1/24"
CLIENT_GW="10.10.0.2"
MAIN_CLIENT_IP="10.10.0.2/24"

SERVER_IP="10.10.1.1/24"
SERVER_GW="10.10.1.2"
MAIN_SERVER_IP="10.10.1.2/24"

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

require_iface() {
    local iface="$1"
    if ! ip link show dev "${iface}" >/dev/null 2>&1; then
        error "Required interface does not exist: ${iface}"
        exit 1
    fi
}

cleanup_old_topology() {
    step "Cleaning any existing FSO topology"
    ip netns del "${CLIENT_NS}" >/dev/null 2>&1 || true
    ip netns del "${SERVER_NS}" >/dev/null 2>&1 || true

    ip link del "${CLIENT_VETH_MAIN}" >/dev/null 2>&1 || true
    ip link del "${SERVER_VETH_MAIN}" >/dev/null 2>&1 || true
    ip link del "${CLIENT_VETH_NS}" >/dev/null 2>&1 || true
    ip link del "${SERVER_VETH_NS}" >/dev/null 2>&1 || true
}

create_namespaces() {
    step "Creating namespaces: ${CLIENT_NS}, ${SERVER_NS}"
    ip netns add "${CLIENT_NS}"
    ip netns add "${SERVER_NS}"
}

create_veth_pairs() {
    step "Creating veth pairs"
    ip link add "${CLIENT_VETH_NS}" type veth peer name "${CLIENT_VETH_MAIN}"
    ip link add "${SERVER_VETH_NS}" type veth peer name "${SERVER_VETH_MAIN}"
}

move_veths() {
    step "Moving namespace-side veth interfaces into namespaces"
    ip link set "${CLIENT_VETH_NS}" netns "${CLIENT_NS}"
    ip link set "${SERVER_VETH_NS}" netns "${SERVER_NS}"
}

configure_mtu() {
    step "Setting MTU=9000 on all veth interfaces and physical NICs"
    ip link set dev "${CLIENT_VETH_MAIN}" mtu 9000
    ip link set dev "${SERVER_VETH_MAIN}" mtu 9000
    ip link set dev "${LAN_IFACE}" mtu 9000
    ip link set dev "${FSO_IFACE}" mtu 9000
    ip netns exec "${CLIENT_NS}" ip link set dev "${CLIENT_VETH_NS}" mtu 9000
    ip netns exec "${SERVER_NS}" ip link set dev "${SERVER_VETH_NS}" mtu 9000
}

configure_addresses() {
    step "Assigning IP addresses"
    ip addr flush dev "${CLIENT_VETH_MAIN}" >/dev/null 2>&1 || true
    ip addr flush dev "${SERVER_VETH_MAIN}" >/dev/null 2>&1 || true
    ip addr add "${MAIN_CLIENT_IP}" dev "${CLIENT_VETH_MAIN}"
    ip addr add "${MAIN_SERVER_IP}" dev "${SERVER_VETH_MAIN}"

    ip netns exec "${CLIENT_NS}" ip addr flush dev "${CLIENT_VETH_NS}" >/dev/null 2>&1 || true
    ip netns exec "${SERVER_NS}" ip addr flush dev "${SERVER_VETH_NS}" >/dev/null 2>&1 || true
    ip netns exec "${CLIENT_NS}" ip addr add "${CLIENT_IP}" dev "${CLIENT_VETH_NS}"
    ip netns exec "${SERVER_NS}" ip addr add "${SERVER_IP}" dev "${SERVER_VETH_NS}"
}

bring_links_up() {
    step "Bringing interfaces up"
    ip link set dev "${CLIENT_VETH_MAIN}" up
    ip link set dev "${SERVER_VETH_MAIN}" up
    ip link set dev "${LAN_IFACE}" up
    ip link set dev "${FSO_IFACE}" up

    ip netns exec "${CLIENT_NS}" ip link set lo up
    ip netns exec "${CLIENT_NS}" ip link set dev "${CLIENT_VETH_NS}" up

    ip netns exec "${SERVER_NS}" ip link set lo up
    ip netns exec "${SERVER_NS}" ip link set dev "${SERVER_VETH_NS}" up
}

enable_forwarding() {
    step "Enabling IPv4 forwarding in main namespace"
    sysctl -w net.ipv4.ip_forward=1 >/dev/null
}

configure_routes() {
    step "Configuring routes"
    ip netns exec "${CLIENT_NS}" ip route replace default via "${CLIENT_GW}" dev "${CLIENT_VETH_NS}"
    ip netns exec "${SERVER_NS}" ip route replace default via "${SERVER_GW}" dev "${SERVER_VETH_NS}"

    ip route replace 10.10.0.0/24 dev "${CLIENT_VETH_MAIN}" scope link
    ip route replace 10.10.1.0/24 dev "${SERVER_VETH_MAIN}" scope link
}

verify_connectivity() {
    step "Verifying namespace gateway connectivity"
    ip netns exec "${CLIENT_NS}" ping -c 2 -W 1 10.10.0.2
    ip netns exec "${SERVER_NS}" ping -c 2 -W 1 10.10.1.2
}

main() {
    require_root
    require_project_root
    require_cmd ip
    require_cmd ping
    require_cmd sysctl

    require_iface "${LAN_IFACE}"
    require_iface "${FSO_IFACE}"

    cleanup_old_topology
    create_namespaces
    create_veth_pairs
    move_veths
    configure_mtu
    configure_addresses
    bring_links_up
    enable_forwarding
    configure_routes
    verify_connectivity

    success "FSO experiment topology ready."
}

main "$@"
