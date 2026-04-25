/**
 * Mirrors `struct config` in include/config.h. There are no extra UI-only
 * fields here — every parameter maps to a CLI flag the daemon accepts.
 */
export interface GatewayConfig {
  lan_iface: string;
  fso_iface: string;
  k: number;
  m: number;
  depth: number;
  symbol_size: number;
  internal_symbol_crc: boolean;
}

export interface ConfigPreset {
  id: string;
  name: string;
  description: string;
  config: Partial<GatewayConfig>;
}

/**
 * Presets reflect parameter sets actually used in the project:
 *
 *  - "phase8"        — fso_gw_runner Phase 8 default (k=2 m=1 depth=2 sym=750)
 *  - "hw-recommended"— README "Recommended FEC parameters for hardware testing"
 *  - "two-machine-test" — script default (GW_K=2 GW_M=1 GW_DEPTH=2 GW_SYMBOL_SIZE=750)
 *  - "high-redundancy"— heavy FEC for lossy channels (m≥k/2, deep interleave)
 *  - "minimal"        — smallest viable block (k=2 m=1)
 */
export const CONFIG_PRESETS: ConfigPreset[] = [
  {
    id: "phase8",
    name: "Phase 8 Default",
    description: "fso_gw_runner default (UDP-validated in Phase 8 hardware run)",
    config: { k: 2, m: 1, depth: 2, symbol_size: 750 },
  },
  {
    id: "hw-recommended",
    name: "HW Recommended",
    description: "README recommended set for hardware testing (50% overhead, MTU)",
    config: { k: 8, m: 4, depth: 2, symbol_size: 1500 },
  },
  {
    id: "two-machine-test",
    name: "Two-Machine Test",
    description: "scripts/two_machine_run_test.sh defaults",
    config: { k: 2, m: 1, depth: 2, symbol_size: 750 },
  },
  {
    id: "high-redundancy",
    name: "High Redundancy",
    description: "Heavy FEC + deep interleaving for lossy channels",
    config: { k: 8, m: 8, depth: 8, symbol_size: 1500 },
  },
  {
    id: "minimal",
    name: "Minimal",
    description: "Smallest viable block — useful for low-rate / control traffic",
    config: { k: 2, m: 1, depth: 2, symbol_size: 1500 },
  },
];

/**
 * Bounds taken from the actual C limits, not from arbitrary GUI caps:
 *   - k:           Wirehair requires K >= 2 (fec_wrapper.c:153).
 *                  Upper bound is MAX_SYMBOLS_PER_BLOCK = 256 (types.h:100).
 *                  Cross-parameter constraint K + M <= 256 is checked by the
 *                  daemon at interleaver_create() time — not modeled here.
 *   - m:           C allows M = 0 (no parity). Upper bound is also 256, but
 *                  effectively limited by K + M <= 256.
 *   - depth:       No explicit ceiling in C — memory grows linearly. 1024 is
 *                  a practical cap (≈18 MiB per matrix at K+M=12, sym=1500).
 *   - symbol_size: MAX_SYMBOL_DATA_SIZE = 9000 in types.h:39 (jumbo frame).
 */
export const CONFIG_BOUNDS = {
  k: { min: 2, max: 256, step: 1 },
  m: { min: 0, max: 256, step: 1 },
  depth: { min: 1, max: 1024, step: 1 },
  symbol_size: { min: 1, max: 9000, step: 1 },
} as const;
