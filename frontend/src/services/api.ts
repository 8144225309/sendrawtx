import type { BroadcastResponse, WorkerHealth } from '@/types/api';

class ApiError extends Error {
  status: number;

  constructor(status: number, message: string) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
  }
}

async function request<T>(url: string, options?: RequestInit): Promise<T> {
  const response = await fetch(url, {
    ...options,
    headers: {
      'Accept': 'application/json',
      ...options?.headers,
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: 'Request failed' }));
    throw new ApiError(response.status, error.error || `HTTP ${response.status}`);
  }

  return response.json();
}

export async function lookupTransaction(txid: string): Promise<BroadcastResponse> {
  return request<BroadcastResponse>(`/tx/${txid}`);
}

export async function getHealth(): Promise<WorkerHealth> {
  return request<WorkerHealth>('/health');
}
