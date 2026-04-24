export interface GatewayConfig {
  lan_iface: string;
  fso_iface: string;
  k: number;
  m: number;
  depth: number;
  symbol_size: number;
  internal_symbol_crc: boolean;
  profile_name: string;
}

export interface ConfigPreset {
  id: string;
  name: string;
  description: string;
  config: Partial<GatewayConfig>;
}

export const CONFIG_PRESETS: ConfigPreset[] = [
  {
    id: "low-latency",
    name: "Low Latency",
    description: "Minimal block size for fastest end-to-end delivery",
    config: { k: 2, m: 1, depth: 2, symbol_size: 1500, profile_name: "LOW-LATENCY" },
  },
  {
    id: "lab-test",
    name: "Lab Test",
    description: "Balanced defaults for bench experimentation",
    config: { k: 8, m: 4, depth: 16, symbol_size: 800, profile_name: "LAB-TEST" },
  },
  {
    id: "high-throughput",
    name: "High Throughput",
    description: "Large blocks, minimal interleaving overhead",
    config: { k: 16, m: 4, depth: 8, symbol_size: 1500, profile_name: "HIGH-THROUGHPUT" },
  },
  {
    id: "max-resilience",
    name: "Max Resilience",
    description: "Heavy redundancy and deep interleaving for harsh channels",
    config: { k: 8, m: 8, depth: 32, symbol_size: 800, profile_name: "MAX-RESILIENCE" },
  },
  {
    id: "storm",
    name: "Storm Conditions",
    description: "Maximum robustness for severe weather / heavy fade",
    config: { k: 4, m: 8, depth: 32, symbol_size: 600, profile_name: "STORM" },
  },
];

export const CONFIG_BOUNDS = {
  k: { min: 1, max: 64, step: 1 },
  m: { min: 0, max: 64, step: 1 },
  depth: { min: 1, max: 128, step: 1 },
  symbol_size: { min: 64, max: 9000, step: 1 },
} as const;
