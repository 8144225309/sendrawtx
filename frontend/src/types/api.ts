// API Response Types - matches C server endpoints

export interface BroadcastResult {
  success: boolean;
  txid?: string;
  error?: string;
  time_ms?: number;
}

export interface OpReturn {
  count: number;
  sizes: number[];
  total_bytes: number;
  max_size: number;
}

export type DifficultyClass =
  | 'trivial'
  | 'easy'
  | 'moderate'
  | 'hard'
  | 'extreme'
  | 'near_impossible';

export interface Classification {
  txid: string;
  size: { total_bytes: number; vbytes: number; is_segwit: boolean };
  fee_rate_sat_vb: number | null;
  is_standard: boolean;
  difficulty_class: DifficultyClass;
  difficulty_score: number;
  non_standard_features: string[];
  op_return: OpReturn;
}

export interface ServicePrediction {
  would_accept: boolean;
  reason: string;
}

export interface Comparison {
  standard_services: Record<string, ServicePrediction>;
  rawrelay_routing: {
    accepted_by: string[];
    rejected_by: string[];
    path_found: boolean;
  };
  bypass_required: string[];
  advantage: string;
}

export interface BroadcastMeta {
  processed_at: string;
  processing_time_ms: number;
  endpoints_attempted: number;
  endpoints_accepted: number;
}

export interface BroadcastResponse {
  success: boolean;
  txid: string;
  broadcast_results: Record<string, BroadcastResult>;
  classification: Classification;
  comparison: Comparison;
  meta: BroadcastMeta;
}

// C server health types
export interface SlotPool {
  used: number;
  max: number;
}

export interface WorkerHealth {
  status: string;
  worker_id: number;
  uptime_seconds: number;
  active_connections: number;
  slots: {
    normal: SlotPool;
    large: SlotPool;
    huge: SlotPool;
  };
  rate_limiter_entries: number;
  tls: {
    enabled: boolean;
    cert_expiry_days?: number;
  };
  resources: {
    open_fds: number;
    max_fds: number;
    usage_pct: number;
  };
}

// Pool display names
export const POOL_NAMES: Record<string, string> = {
  core: 'Bitcoin Core',
  knots: 'Bitcoin Knots',
  libre: 'Libre Relay',
  mempool_space: 'mempool.space',
  blockstream: 'Blockstream',
  blockcypher: 'BlockCypher',
};
