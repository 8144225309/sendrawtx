import { Card, CardHeader, CardTitle, CardContent, Badge } from '@/components/ui';
import { Code, Terminal, Zap } from 'lucide-react';

function ApiDocsPage() {
  const endpoints = [
    {
      method: 'GET',
      path: '/{raw_tx_hex}',
      description: 'Broadcast a raw transaction. The hex must be 164+ characters (a valid serialized Bitcoin transaction).',
      example: 'curl https://sendrawtx.com/0100000001...',
      notes: 'Returns broadcast.html for browsers, JSON with Accept: application/json header.',
    },
    {
      method: 'GET',
      path: '/tx/{txid}',
      description: 'Look up a cached broadcast result by transaction ID (64 hex characters).',
      example: 'curl -H "Accept: application/json" https://sendrawtx.com/tx/abc123...',
      notes: 'Returns result.html for browsers, JSON with Accept: application/json header.',
    },
    {
      method: 'GET',
      path: '/{txid}',
      description: 'Shorthand for /tx/{txid}. Exactly 64 hex characters routes to result lookup.',
      notes: 'The server distinguishes broadcasts (164+ hex) from lookups (64 hex) by length.',
    },
    {
      method: 'GET',
      path: '/health',
      description: 'Worker health check. Returns JSON with worker ID, uptime, slot usage, TLS status, and resource consumption.',
      example: 'curl https://sendrawtx.com/health',
    },
    {
      method: 'GET',
      path: '/metrics',
      description: 'Prometheus-format metrics for monitoring. Includes connection counts, slot usage, request rates.',
      example: 'curl https://sendrawtx.com/metrics',
    },
    {
      method: 'GET',
      path: '/ready',
      description: 'Readiness probe. Returns 200 when the worker is ready to accept connections.',
    },
    {
      method: 'GET',
      path: '/alive',
      description: 'Liveness probe. Returns 200 as long as the worker process is running.',
    },
  ];

  return (
    <div className="flex-1 py-8">
      <div className="max-w-4xl mx-auto px-4 sm:px-6">
        <div className="mb-8">
          <h1 className="text-3xl font-bold text-white mb-2">API Documentation</h1>
          <p className="text-[#a0a0a0]">
            REST API for broadcasting raw Bitcoin transactions via the C server
          </p>
        </div>

        {/* Quick Start */}
        <Card variant="bordered" className="mb-8">
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Zap className="w-5 h-5 text-[#f7931a]" />
              Quick Start
            </CardTitle>
          </CardHeader>
          <CardContent>
            <p className="text-[#a0a0a0] mb-4">
              Broadcast a transaction with a single curl command:
            </p>
            <div className="bg-[#0f0f0f] rounded-lg p-4 font-mono text-sm overflow-x-auto">
              <code className="text-[#22c55e]">curl</code>
              <code className="text-white"> </code>
              <code className="text-[#f7931a]">https://sendrawtx.com/0100000001...</code>
            </div>
            <p className="text-[#666] text-sm mt-3">
              For JSON response, add the Accept header:
            </p>
            <div className="bg-[#0f0f0f] rounded-lg p-4 font-mono text-sm overflow-x-auto mt-2">
              <code className="text-[#22c55e]">curl</code>
              <code className="text-white"> -H </code>
              <code className="text-[#eab308]">"Accept: application/json"</code>
              <code className="text-white"> </code>
              <code className="text-[#f7931a]">https://sendrawtx.com/0100000001...</code>
            </div>
          </CardContent>
        </Card>

        {/* Content Negotiation */}
        <Card variant="bordered" className="mb-8">
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Terminal className="w-5 h-5 text-[#f7931a]" />
              Content Negotiation
            </CardTitle>
          </CardHeader>
          <CardContent>
            <p className="text-[#a0a0a0] mb-4">
              Broadcast and result endpoints support content negotiation:
            </p>
            <div className="space-y-3">
              <div className="flex items-start gap-3">
                <Badge variant="info" className="mt-0.5">Browser</Badge>
                <span className="text-[#a0a0a0]">Returns HTML page with interactive UI</span>
              </div>
              <div className="flex items-start gap-3">
                <Badge variant="success" className="mt-0.5">JSON</Badge>
                <span className="text-[#a0a0a0]">
                  Send <code className="text-[#f7931a]">Accept: application/json</code> to get structured JSON response
                </span>
              </div>
            </div>
          </CardContent>
        </Card>

        {/* Endpoints */}
        <div className="space-y-6 mb-8">
          <h2 className="text-xl font-semibold text-white">Endpoints</h2>

          {endpoints.map((endpoint) => (
            <Card key={`${endpoint.method}-${endpoint.path}`} variant="bordered">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <Badge variant="info" size="sm">
                    {endpoint.method}
                  </Badge>
                  <code className="text-white font-mono">{endpoint.path}</code>
                </CardTitle>
              </CardHeader>
              <CardContent>
                <p className="text-[#a0a0a0] mb-4">{endpoint.description}</p>

                {endpoint.notes && (
                  <p className="text-sm text-[#666] mb-4">{endpoint.notes}</p>
                )}

                {endpoint.example && (
                  <div>
                    <p className="text-sm text-[#666] mb-2">Example:</p>
                    <div className="bg-[#0f0f0f] rounded-lg p-3 font-mono text-sm overflow-x-auto">
                      <code className="text-[#a0a0a0]">{endpoint.example}</code>
                    </div>
                  </div>
                )}
              </CardContent>
            </Card>
          ))}
        </div>

        {/* Health Response Example */}
        <Card variant="bordered">
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Code className="w-5 h-5 text-[#f7931a]" />
              Health Response Example
            </CardTitle>
          </CardHeader>
          <CardContent>
            <div className="bg-[#0f0f0f] rounded-lg p-4 font-mono text-sm overflow-x-auto">
              <pre className="text-[#a0a0a0]">
{`{
  "status": "ok",
  "worker_id": 1,
  "uptime_seconds": 3600,
  "active_connections": 5,
  "slots": {
    "normal": {"used": 3, "max": 1000},
    "large": {"used": 1, "max": 50},
    "huge": {"used": 0, "max": 10}
  },
  "rate_limiter_entries": 42,
  "tls": {
    "enabled": true,
    "cert_expiry_days": 60
  },
  "resources": {
    "open_fds": 128,
    "max_fds": 65536,
    "usage_pct": 0.2
  }
}`}
              </pre>
            </div>
          </CardContent>
        </Card>
      </div>
    </div>
  );
}

export { ApiDocsPage };
