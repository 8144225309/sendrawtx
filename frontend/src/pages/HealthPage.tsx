import { useEffect, useState } from 'react';
import { CheckCircle, XCircle, RefreshCw, Server, Shield, Cpu, AlertTriangle } from 'lucide-react';
import { Card, CardHeader, CardTitle, CardContent, Badge, Button } from '@/components/ui';
import { getHealth } from '@/services/api';
import type { WorkerHealth } from '@/types/api';

function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

function HealthPage() {
  const [health, setHealth] = useState<WorkerHealth | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const fetchHealth = async () => {
    setLoading(true);
    setError(null);
    try {
      const data = await getHealth();
      setHealth(data);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to fetch health');
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    fetchHealth();
    const interval = setInterval(fetchHealth, 30000);
    return () => clearInterval(interval);
  }, []);

  return (
    <div className="flex-1 py-8">
      <div className="max-w-4xl mx-auto px-4 sm:px-6">
        <div className="flex items-center justify-between mb-8">
          <div>
            <h1 className="text-3xl font-bold text-white mb-2">System Status</h1>
            <p className="text-[#a0a0a0]">Monitor worker health and resource usage</p>
          </div>
          <Button variant="secondary" onClick={fetchHealth} disabled={loading}>
            <RefreshCw className={`w-4 h-4 mr-2 ${loading ? 'animate-spin' : ''}`} />
            Refresh
          </Button>
        </div>

        {error && (
          <Card variant="bordered" className="mb-6 border-[#ef4444]">
            <CardContent className="flex items-center gap-3 text-[#ef4444]">
              <XCircle className="w-5 h-5" />
              <span>{error}</span>
            </CardContent>
          </Card>
        )}

        {health && (
          <div className="space-y-6">
            {/* Overall Status */}
            <Card variant="bordered">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <Server className="w-5 h-5 text-[#f7931a]" />
                  Worker Status
                </CardTitle>
              </CardHeader>
              <CardContent>
                <div className="flex items-center gap-3 mb-6">
                  {health.status === 'ok' ? (
                    <>
                      <CheckCircle className="w-8 h-8 text-[#22c55e]" />
                      <div>
                        <p className="text-lg font-semibold text-white">Operational</p>
                        <p className="text-sm text-[#a0a0a0]">All systems running normally</p>
                      </div>
                    </>
                  ) : (
                    <>
                      <XCircle className="w-8 h-8 text-[#ef4444]" />
                      <div>
                        <p className="text-lg font-semibold text-white">Issues Detected</p>
                        <p className="text-sm text-[#a0a0a0]">Some systems may be degraded</p>
                      </div>
                    </>
                  )}
                </div>
                <div className="grid grid-cols-2 sm:grid-cols-4 gap-4">
                  <div>
                    <p className="text-sm text-[#666] mb-1">Worker ID</p>
                    <p className="text-xl font-bold text-white">{health.worker_id}</p>
                  </div>
                  <div>
                    <p className="text-sm text-[#666] mb-1">Uptime</p>
                    <p className="text-xl font-bold text-white">{formatUptime(health.uptime_seconds)}</p>
                  </div>
                  <div>
                    <p className="text-sm text-[#666] mb-1">Active Connections</p>
                    <p className="text-xl font-bold text-white">{health.active_connections}</p>
                  </div>
                  <div>
                    <p className="text-sm text-[#666] mb-1">Rate Limiter</p>
                    <p className="text-xl font-bold text-white">{health.rate_limiter_entries} entries</p>
                  </div>
                </div>
              </CardContent>
            </Card>

            {/* Slot Usage */}
            <Card variant="bordered">
              <CardHeader>
                <CardTitle>Slot Usage</CardTitle>
              </CardHeader>
              <CardContent>
                <div className="space-y-4">
                  {Object.entries(health.slots).map(([name, pool]) => {
                    const pct = pool.max > 0 ? Math.round((pool.used / pool.max) * 100) : 0;
                    return (
                      <div key={name}>
                        <div className="flex justify-between items-center mb-1">
                          <span className="text-sm font-medium text-white capitalize">{name}</span>
                          <span className="text-sm text-[#a0a0a0]">{pool.used}/{pool.max} ({pct}%)</span>
                        </div>
                        <div className="w-full bg-[#252525] rounded-full h-2">
                          <div
                            className={`h-2 rounded-full transition-all ${
                              pct >= 90 ? 'bg-[#ef4444]' : pct >= 70 ? 'bg-[#eab308]' : 'bg-[#22c55e]'
                            }`}
                            style={{ width: `${pct}%` }}
                          />
                        </div>
                      </div>
                    );
                  })}
                </div>
              </CardContent>
            </Card>

            {/* TLS Status */}
            <Card variant="bordered">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <Shield className="w-5 h-5 text-[#f7931a]" />
                  TLS Status
                </CardTitle>
              </CardHeader>
              <CardContent>
                <div className="flex items-center gap-4">
                  <div className="flex items-center gap-2">
                    {health.tls.enabled ? (
                      <CheckCircle className="w-5 h-5 text-[#22c55e]" />
                    ) : (
                      <XCircle className="w-5 h-5 text-[#ef4444]" />
                    )}
                    <span className="text-white font-medium">
                      {health.tls.enabled ? 'Enabled' : 'Disabled'}
                    </span>
                  </div>
                  {health.tls.enabled && health.tls.cert_expiry_days !== undefined && (
                    <div className="flex items-center gap-2">
                      {health.tls.cert_expiry_days < 14 ? (
                        <AlertTriangle className="w-4 h-4 text-[#eab308]" />
                      ) : (
                        <CheckCircle className="w-4 h-4 text-[#22c55e]" />
                      )}
                      <span className="text-[#a0a0a0]">
                        Certificate expires in{' '}
                        <Badge variant={health.tls.cert_expiry_days < 14 ? 'warning' : 'success'}>
                          {health.tls.cert_expiry_days} days
                        </Badge>
                      </span>
                    </div>
                  )}
                </div>
              </CardContent>
            </Card>

            {/* Resources */}
            <Card variant="bordered">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <Cpu className="w-5 h-5 text-[#f7931a]" />
                  Resources
                </CardTitle>
              </CardHeader>
              <CardContent>
                <div className="grid grid-cols-3 gap-4 mb-4">
                  <div>
                    <p className="text-sm text-[#666] mb-1">Open FDs</p>
                    <p className="text-xl font-bold text-white">{health.resources.open_fds}</p>
                  </div>
                  <div>
                    <p className="text-sm text-[#666] mb-1">Max FDs</p>
                    <p className="text-xl font-bold text-white">{health.resources.max_fds}</p>
                  </div>
                  <div>
                    <p className="text-sm text-[#666] mb-1">Usage</p>
                    <p className="text-xl font-bold text-white">{health.resources.usage_pct.toFixed(1)}%</p>
                  </div>
                </div>
                <div className="w-full bg-[#252525] rounded-full h-3">
                  <div
                    className={`h-3 rounded-full transition-all ${
                      health.resources.usage_pct >= 90 ? 'bg-[#ef4444]'
                        : health.resources.usage_pct >= 70 ? 'bg-[#eab308]'
                        : 'bg-[#22c55e]'
                    }`}
                    style={{ width: `${Math.min(health.resources.usage_pct, 100)}%` }}
                  />
                </div>
              </CardContent>
            </Card>
          </div>
        )}
      </div>
    </div>
  );
}

export { HealthPage };
