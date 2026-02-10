import { useEffect, useState } from 'react';
import {
  CheckCircle,
  XCircle,
  Copy,
  ExternalLink,
  ArrowLeft,
  AlertCircle,
} from 'lucide-react';
import {
  Card,
  CardHeader,
  CardTitle,
  CardContent,
  Badge,
  DifficultyBadge,
  AcceptanceBar,
  Button,
} from '@/components/ui';
import { lookupTransaction } from '@/services/api';
import type { BroadcastResponse } from '@/types/api';
import { POOL_NAMES } from '@/types/api';

function TxResultPage() {
  // Parse txid from URL path — works in both SPA and standalone
  const path = window.location.pathname;
  let txid: string | undefined;
  if (path.startsWith('/tx/')) {
    txid = path.slice(4);
  } else {
    const segment = path.slice(1);
    if (/^[0-9a-fA-F]{64}$/.test(segment)) {
      txid = segment;
    }
  }

  const [result, setResult] = useState<BroadcastResponse | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    if (!txid) return;

    const fetchResult = async () => {
      try {
        const data = await lookupTransaction(txid);
        setResult(data);
      } catch (err) {
        setError(err instanceof Error ? err.message : 'Failed to load transaction');
      } finally {
        setLoading(false);
      }
    };

    fetchResult();
  }, [txid]);

  const copyTxid = async () => {
    if (!txid) return;
    await navigator.clipboard.writeText(txid);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  if (loading) {
    return (
      <div className="flex-1 flex items-center justify-center">
        <div className="animate-spin w-8 h-8 border-2 border-[#f7931a] border-t-transparent rounded-full" />
      </div>
    );
  }

  if (error || !result) {
    return (
      <div className="flex-1 flex items-center justify-center">
        <Card className="max-w-md text-center">
          <CardContent>
            <AlertCircle className="w-12 h-12 text-[#ef4444] mx-auto mb-4" />
            <h2 className="text-xl font-semibold text-white mb-2">Transaction Not Found</h2>
            <p className="text-[#a0a0a0] mb-4">{error || 'Results expire after 1 hour'}</p>
            <a href="/">
              <Button variant="secondary">
                <ArrowLeft className="w-4 h-4 mr-2" />
                Back to Broadcast
              </Button>
            </a>
          </CardContent>
        </Card>
      </div>
    );
  }

  const { broadcast_results, classification, comparison, meta } = result;
  const acceptedCount = Object.values(broadcast_results).filter((r) => r.success).length;
  const totalCount = Object.keys(broadcast_results).length;

  return (
    <div className="flex-1 py-8">
      <div className="max-w-4xl mx-auto px-4 sm:px-6">
        {/* Header */}
        <div className="mb-8">
          <a
            href="/"
            className="inline-flex items-center text-sm text-[#a0a0a0] hover:text-white mb-4"
          >
            <ArrowLeft className="w-4 h-4 mr-1" />
            Back to Broadcast
          </a>

          <div className="flex items-start justify-between gap-4">
            <div>
              <h1 className="text-2xl font-bold text-white mb-2">Transaction Result</h1>
              <div className="flex items-center gap-2">
                <code className="text-sm text-[#a0a0a0] font-mono truncate max-w-xs sm:max-w-md">
                  {txid}
                </code>
                <button
                  onClick={copyTxid}
                  className="p-1 text-[#666] hover:text-white transition-colors"
                  title="Copy txid"
                >
                  <Copy className="w-4 h-4" />
                </button>
                {copied && <span className="text-xs text-[#22c55e]">Copied!</span>}
              </div>
            </div>

            {result.success ? (
              <Badge variant="success" className="flex items-center gap-1">
                <CheckCircle className="w-4 h-4" />
                Success
              </Badge>
            ) : (
              <Badge variant="error" className="flex items-center gap-1">
                <XCircle className="w-4 h-4" />
                Failed
              </Badge>
            )}
          </div>
        </div>

        <div className="space-y-6">
          {/* Endpoint Acceptance */}
          <Card variant="bordered">
            <CardHeader>
              <CardTitle>Endpoint Acceptance</CardTitle>
            </CardHeader>
            <CardContent>
              <AcceptanceBar accepted={acceptedCount} total={totalCount} className="mb-6" />

              <div className="space-y-3">
                {Object.entries(broadcast_results).map(([pool, poolResult]) => (
                  <div
                    key={pool}
                    className="flex items-center justify-between py-2 border-b border-[#252525] last:border-0"
                  >
                    <div className="flex items-center gap-3">
                      {poolResult.success ? (
                        <CheckCircle className="w-5 h-5 text-[#22c55e]" />
                      ) : (
                        <XCircle className="w-5 h-5 text-[#ef4444]" />
                      )}
                      <span className="font-medium text-white">
                        {POOL_NAMES[pool] || pool}
                      </span>
                    </div>
                    <div className="flex items-center gap-3 text-sm">
                      {poolResult.success ? (
                        <span className="text-[#a0a0a0]">{poolResult.time_ms}ms</span>
                      ) : (
                        <span className="text-[#ef4444] truncate max-w-xs">
                          {poolResult.error}
                        </span>
                      )}
                    </div>
                  </div>
                ))}
              </div>
            </CardContent>
          </Card>

          {/* Difficulty & Classification */}
          <Card variant="bordered">
            <CardHeader>
              <CardTitle>Classification</CardTitle>
            </CardHeader>
            <CardContent>
              <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 mb-6">
                <div>
                  <p className="text-sm text-[#666] mb-1">Difficulty</p>
                  <DifficultyBadge difficulty={classification.difficulty_class} />
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Fee Rate</p>
                  <p className="text-white font-medium">
                    {classification.fee_rate_sat_vb?.toFixed(2) || '?'} sat/vB
                  </p>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Size</p>
                  <p className="text-white font-medium">{classification.size.vbytes} vB</p>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Standard</p>
                  <Badge variant={classification.is_standard ? 'success' : 'warning'}>
                    {classification.is_standard ? 'Yes' : 'No'}
                  </Badge>
                </div>
              </div>

              {classification.non_standard_features.length > 0 && (
                <div>
                  <p className="text-sm text-[#666] mb-2">Non-Standard Features</p>
                  <div className="flex flex-wrap gap-2">
                    {classification.non_standard_features.map((feature) => (
                      <Badge key={feature} variant="warning">
                        {feature}
                      </Badge>
                    ))}
                  </div>
                </div>
              )}
            </CardContent>
          </Card>

          {/* Comparison */}
          <Card variant="bordered">
            <CardHeader>
              <CardTitle>Service Comparison</CardTitle>
            </CardHeader>
            <CardContent>
              <p className="text-sm text-[#a0a0a0] mb-4">{comparison.advantage}</p>

              <div className="space-y-2">
                {Object.entries(comparison.standard_services).map(([service, prediction]) => (
                  <div
                    key={service}
                    className="flex items-center justify-between py-2 border-b border-[#252525]"
                  >
                    <span className="text-[#a0a0a0]">{POOL_NAMES[service] || service}</span>
                    <div className="flex items-center gap-2">
                      {prediction.would_accept ? (
                        <CheckCircle className="w-4 h-4 text-[#22c55e]" />
                      ) : (
                        <XCircle className="w-4 h-4 text-[#ef4444]" />
                      )}
                      <span className="text-sm text-[#666]">{prediction.reason}</span>
                    </div>
                  </div>
                ))}

                <div className="flex items-center justify-between py-2 bg-[#f7931a10] -mx-6 px-6 rounded-b-lg">
                  <span className="font-medium text-[#f7931a]">RawRelay</span>
                  <div className="flex items-center gap-2">
                    {comparison.rawrelay_routing.path_found ? (
                      <CheckCircle className="w-4 h-4 text-[#22c55e]" />
                    ) : (
                      <XCircle className="w-4 h-4 text-[#ef4444]" />
                    )}
                    <span className="text-sm text-white">
                      {comparison.rawrelay_routing.path_found
                        ? `Found path via ${comparison.rawrelay_routing.accepted_by.join(', ')}`
                        : 'No path found'}
                    </span>
                  </div>
                </div>
              </div>

              {comparison.bypass_required.length > 0 && (
                <div className="mt-4 pt-4 border-t border-[#252525]">
                  <p className="text-sm text-[#666] mb-2">Policies Bypassed</p>
                  <div className="flex flex-wrap gap-2">
                    {comparison.bypass_required.map((policy) => (
                      <Badge key={policy} variant="info">
                        {policy}
                      </Badge>
                    ))}
                  </div>
                </div>
              )}
            </CardContent>
          </Card>

          {/* Timing */}
          <Card variant="bordered">
            <CardHeader>
              <CardTitle>Timing</CardTitle>
            </CardHeader>
            <CardContent>
              <div className="grid grid-cols-2 sm:grid-cols-4 gap-4">
                <div>
                  <p className="text-sm text-[#666] mb-1">Processed</p>
                  <p className="text-white text-sm">
                    {new Date(meta.processed_at).toLocaleString()}
                  </p>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Processing Time</p>
                  <p className="text-white font-medium">{meta.processing_time_ms}ms</p>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Endpoints Tried</p>
                  <p className="text-white font-medium">{meta.endpoints_attempted}</p>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Accepted</p>
                  <p className="text-white font-medium">{meta.endpoints_accepted}</p>
                </div>
              </div>
            </CardContent>
          </Card>

          {/* External Links */}
          <div className="flex justify-center gap-4">
            <a
              href={`https://mempool.space/tx/${txid}`}
              target="_blank"
              rel="noopener noreferrer"
              className="inline-flex items-center gap-2 text-sm text-[#a0a0a0] hover:text-white transition-colors"
            >
              <ExternalLink className="w-4 h-4" />
              View on mempool.space
            </a>
            <a
              href={`https://blockstream.info/tx/${txid}`}
              target="_blank"
              rel="noopener noreferrer"
              className="inline-flex items-center gap-2 text-sm text-[#a0a0a0] hover:text-white transition-colors"
            >
              <ExternalLink className="w-4 h-4" />
              View on Blockstream
            </a>
          </div>
        </div>
      </div>
    </div>
  );
}

export { TxResultPage };
