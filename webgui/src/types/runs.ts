export interface RunSummary {
  id: number;
  name: string;
  startedAt: number;
  endedAt: number | null;
  notes: string | null;
  sampleCount: number;
  active: boolean;
}

export interface RunListResponse {
  active_run_id: number | null;
  runs: RunSummary[];
}

export interface RunSample {
  t: number;
  source: string;
  linkState: string;
  linkQualityPct: number | null;
  linkRssiDbm: number | null;
  linkSnrDb: number | null;
  linkBer: number | null;
  linkLatencyAvg: number | null;
  linkLatencyMax: number | null;
  txBps: number | null;
  rxBps: number | null;
  txPps: number | null;
  rxPps: number | null;
  blocksAttempted: number | null;
  blocksRecovered: number | null;
  blocksFailed: number | null;
  recoveredPackets: number | null;
  crcDrops: number | null;
  cpuPct: number | null;
  memoryPct: number | null;
}

export interface RunStats {
  t_min: number;
  t_max: number;
  n: number;
  tx_avg: number | null;
  tx_peak: number | null;
  rx_avg: number | null;
  rx_peak: number | null;
  quality_avg: number | null;
  quality_min: number | null;
  ber_avg: number | null;
  ber_peak: number | null;
  blocks_attempted: number | null;
  blocks_recovered: number | null;
  blocks_failed: number | null;
  crc_drops: number | null;
}

export interface RunDetailResponse {
  run: RunSummary;
  stats: RunStats | null;
}
