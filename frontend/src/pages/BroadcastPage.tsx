import { useEffect, useState, useRef } from 'react';
import { Loader2, CheckCircle, XCircle, ExternalLink } from 'lucide-react';
import { Card, CardContent, Badge } from '@/components/ui';
import { TransactionPreview } from '@/components/TransactionPreview';
import { decodeAny } from '@/lib/txDecoder';
import { lookupTransaction } from '@/services/api';
import type { BroadcastResponse } from '@/types/api';

function BroadcastPage() {
  const [hex, setHex] = useState('');
  const [txid, setTxid] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [result, setResult] = useState<BroadcastResponse | null>(null);
  const [polling, setPolling] = useState(true);
  const [attempts, setAttempts] = useState(0);
  const pollRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Extract hex from URL path on mount
  useEffect(() => {
    const path = window.location.pathname.slice(1); // remove leading /
    if (!path || path.length < 164 || !/^[0-9a-fA-F]+$/.test(path)) {
      setError('Invalid transaction hex in URL');
      setPolling(false);
      return;
    }
    setHex(path);

    // Decode to get txid
    try {
      const decoded = decodeAny(path);
      if (decoded && 'txid' in decoded && decoded.txid) {
        setTxid(decoded.txid as string);
      } else {
        setError('Could not decode transaction');
        setPolling(false);
      }
    } catch {
      setError('Could not decode transaction');
      setPolling(false);
    }
  }, []);

  // Poll for result once we have a txid
  useEffect(() => {
    if (!txid || !polling) return;

    let attempt = 0;
    const maxAttempts = 15;
    const baseDelay = 500;

    const poll = async () => {
      attempt++;
      setAttempts(attempt);

      try {
        const data = await lookupTransaction(txid);
        setResult(data);
        setPolling(false);
        return;
      } catch {
        // Not ready yet, continue polling
      }

      if (attempt >= maxAttempts) {
        // Give up polling, redirect to result page
        setPolling(false);
        window.location.href = `/tx/${txid}`;
        return;
      }

      const delay = Math.min(baseDelay * Math.pow(1.5, attempt - 1), 5000);
      pollRef.current = setTimeout(poll, delay);
    };

    // Start polling after initial delay
    pollRef.current = setTimeout(poll, baseDelay);

    return () => {
      if (pollRef.current) clearTimeout(pollRef.current);
    };
  }, [txid, polling]);

  if (error) {
    return (
      <div className="flex-1 flex items-center justify-center py-12">
        <Card className="max-w-md text-center">
          <CardContent>
            <XCircle className="w-12 h-12 text-[#ef4444] mx-auto mb-4" />
            <h2 className="text-xl font-semibold text-white mb-2">Invalid Transaction</h2>
            <p className="text-[#a0a0a0] mb-4">{error}</p>
            <a href="/" className="inline-flex items-center gap-2 text-[#f7931a] hover:text-[#ffa940]">
              Back to Broadcast
            </a>
          </CardContent>
        </Card>
      </div>
    );
  }

  return (
    <div className="flex-1 py-8">
      <div className="max-w-4xl mx-auto px-4 sm:px-6">
        {/* Status Header */}
        <div className="mb-8 text-center">
          {polling ? (
            <>
              <Loader2 className="w-12 h-12 text-[#f7931a] mx-auto mb-4 animate-spin" />
              <h1 className="text-2xl font-bold text-white mb-2">Broadcasting Transaction</h1>
              <p className="text-[#a0a0a0]">
                Submitting to network endpoints... (attempt {attempts})
              </p>
            </>
          ) : result ? (
            <>
              {result.success ? (
                <CheckCircle className="w-12 h-12 text-[#22c55e] mx-auto mb-4" />
              ) : (
                <XCircle className="w-12 h-12 text-[#ef4444] mx-auto mb-4" />
              )}
              <h1 className="text-2xl font-bold text-white mb-2">
                {result.success ? 'Broadcast Successful' : 'Broadcast Failed'}
              </h1>
            </>
          ) : null}

          {txid && (
            <div className="mt-4">
              <Badge variant="default" className="font-mono text-xs">
                {txid}
              </Badge>
              <div className="mt-2 flex justify-center gap-4">
                <a
                  href={`/tx/${txid}`}
                  className="text-sm text-[#f7931a] hover:text-[#ffa940]"
                >
                  View full result
                </a>
                <a
                  href={`https://mempool.space/tx/${txid}`}
                  target="_blank"
                  rel="noopener noreferrer"
                  className="inline-flex items-center gap-1 text-sm text-[#a0a0a0] hover:text-white"
                >
                  <ExternalLink className="w-3 h-3" />
                  mempool.space
                </a>
              </div>
            </div>
          )}
        </div>

        {/* Transaction Preview */}
        {hex && (
          <div className="mb-8">
            <TransactionPreview rawTx={hex} />
          </div>
        )}

        {/* Result Summary (inline) */}
        {result && (
          <Card variant="bordered">
            <CardContent>
              <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 text-center">
                <div>
                  <p className="text-sm text-[#666] mb-1">Status</p>
                  <Badge variant={result.success ? 'success' : 'error'}>
                    {result.success ? 'Accepted' : 'Rejected'}
                  </Badge>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Endpoints Tried</p>
                  <p className="text-white font-medium">{result.meta.endpoints_attempted}</p>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Accepted By</p>
                  <p className="text-white font-medium">{result.meta.endpoints_accepted}</p>
                </div>
                <div>
                  <p className="text-sm text-[#666] mb-1">Time</p>
                  <p className="text-white font-medium">{result.meta.processing_time_ms}ms</p>
                </div>
              </div>
            </CardContent>
          </Card>
        )}
      </div>
    </div>
  );
}

export { BroadcastPage };
