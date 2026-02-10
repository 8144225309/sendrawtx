import { useState, useEffect, useMemo } from 'react';
import {
  decodeAny,
  detectInputType,
  formatSighashType,
  canFinalizePSBT,
  extractTransactionFromPSBT,
} from '../lib/txDecoder';
import type {
  DecodeResult,
  DecodedTransaction,
  DecodedPSBT,
  DecodedInput,
  DecodedOutput,
  PSBTInput,
  DetectedProtocol,
  NonStandardReport,
  OrdinalsInscription,
  BRC20Operation,
  RunesOperation,
} from '../lib/txDecoder';
import {
  AlertTriangle,
  CheckCircle2,
  Clock,
  ArrowRight,
  ArrowLeft,
  Hash,
  FileText,
  Loader2,
  Copy,
  ChevronDown,
  ChevronRight,
  Zap,
  Lock,
  Unlock,
  Key,
  HelpCircle,
  Download,
  Check,
  Tag,
  Gem,
  Coins,
  Stamp,
  Code2,
  ShieldAlert,
  XCircle,
  Info,
  Cat,
  Atom,
} from 'lucide-react';

interface TransactionPreviewProps {
  rawTx: string;
  onValidChange?: (isValid: boolean, canBroadcast: boolean) => void;
}

// Debounce hook
function useDebounce<T>(value: T, delay: number): T {
  const [debouncedValue, setDebouncedValue] = useState<T>(value);
  useEffect(() => {
    const handler = setTimeout(() => setDebouncedValue(value), delay);
    return () => clearTimeout(handler);
  }, [value, delay]);
  return debouncedValue;
}

// Copy to clipboard helper
function copyToClipboard(text: string) {
  navigator.clipboard.writeText(text);
}

// Truncate address/hash for display
function truncate(str: string, startChars = 8, endChars = 8): string {
  if (str.length <= startChars + endChars + 3) return str;
  return `${str.slice(0, startChars)}...${str.slice(-endChars)}`;
}

// Format script type for display
function formatScriptType(type: string): string {
  const names: Record<string, string> = {
    p2pkh: 'P2PKH (Legacy)',
    p2sh: 'P2SH',
    p2wpkh: 'P2WPKH (Native SegWit)',
    p2wsh: 'P2WSH',
    p2tr: 'P2TR (Taproot)',
    op_return: 'OP_RETURN',
    p2pk: 'P2PK (Legacy)',
    multisig: 'Bare Multisig',
    unknown: 'Unknown',
  };
  return names[type] || type.toUpperCase();
}

// Input component with potential lazy-loading
function InputRow({ input, index }: { input: DecodedInput; index: number }) {
  const [expanded, setExpanded] = useState(false);

  return (
    <div className="border border-[#333] rounded-lg p-3 bg-[#1a1a1a]">
      <div
        className="flex items-center gap-2 cursor-pointer"
        onClick={() => setExpanded(!expanded)}
      >
        {expanded ? (
          <ChevronDown className="w-4 h-4 text-[#666]" />
        ) : (
          <ChevronRight className="w-4 h-4 text-[#666]" />
        )}
        <span className="text-[#666] text-sm">#{index}</span>
        <code className="text-xs text-[#a0a0a0] font-mono flex-1">
          {truncate(input.txid, 12, 12)}:{input.vout}
        </code>
        <button
          onClick={(e) => {
            e.stopPropagation();
            copyToClipboard(`${input.txid}:${input.vout}`);
          }}
          className="text-[#666] hover:text-[#a0a0a0]"
        >
          <Copy className="w-3 h-3" />
        </button>
        {input.isRbfEnabled ? (
          <span className="text-xs px-1.5 py-0.5 rounded bg-[#f7931a]/20 text-[#f7931a]">
            RBF
          </span>
        ) : (
          <span className="text-xs px-1.5 py-0.5 rounded bg-[#333] text-[#666]">
            Final
          </span>
        )}
      </div>

      {expanded && (
        <div className="mt-3 pt-3 border-t border-[#333] space-y-2 text-xs">
          <div className="flex justify-between">
            <span className="text-[#666]">Previous TXID:</span>
            <code className="text-[#a0a0a0] font-mono">{input.txid}</code>
          </div>
          <div className="flex justify-between">
            <span className="text-[#666]">Output Index:</span>
            <span className="text-[#a0a0a0]">{input.vout}</span>
          </div>
          <div className="flex justify-between">
            <span className="text-[#666]">Sequence:</span>
            <code className="text-[#a0a0a0] font-mono">
              0x{input.sequence.toString(16).padStart(8, '0')}
            </code>
          </div>
          {input.scriptSig && (
            <div>
              <span className="text-[#666]">ScriptSig:</span>
              <code className="block mt-1 text-[#a0a0a0] font-mono break-all bg-[#252525] p-2 rounded text-[10px]">
                {input.scriptSigAsm}
              </code>
            </div>
          )}
          {input.witness && input.witness.length > 0 && (
            <div>
              <span className="text-[#666]">Witness ({input.witness.length} items):</span>
              <div className="mt-1 space-y-1">
                {input.witness.map((w, i) => (
                  <code
                    key={i}
                    className="block text-[#a0a0a0] font-mono break-all bg-[#252525] p-1 rounded text-[10px]"
                  >
                    {truncate(w, 20, 20)}
                  </code>
                ))}
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  );
}

// Output component
function OutputRow({ output, index }: { output: DecodedOutput; index: number }) {
  const [expanded, setExpanded] = useState(false);

  const isOpReturn = output.scriptType === 'op_return';

  return (
    <div
      className={`border rounded-lg p-3 ${
        isOpReturn
          ? 'border-[#3b82f6]/30 bg-[#3b82f6]/5'
          : 'border-[#333] bg-[#1a1a1a]'
      }`}
    >
      <div
        className="flex items-center gap-2 cursor-pointer"
        onClick={() => setExpanded(!expanded)}
      >
        {expanded ? (
          <ChevronDown className="w-4 h-4 text-[#666]" />
        ) : (
          <ChevronRight className="w-4 h-4 text-[#666]" />
        )}
        <span className="text-[#666] text-sm">#{index}</span>

        {isOpReturn ? (
          <span className="text-xs px-1.5 py-0.5 rounded bg-[#3b82f6]/20 text-[#3b82f6]">
            OP_RETURN
          </span>
        ) : (
          <code className="text-xs text-[#a0a0a0] font-mono flex-1 truncate">
            {output.address || 'Unknown address'}
          </code>
        )}

        <span className="text-sm font-medium text-white ml-auto">
          {isOpReturn ? (
            <span className="text-[#666]">
              {output.scriptPubKey.length / 2 - 1} bytes
            </span>
          ) : (
            <>
              {output.valueBtc} <span className="text-[#f7931a]">BTC</span>
            </>
          )}
        </span>
      </div>

      {expanded && (
        <div className="mt-3 pt-3 border-t border-[#333] space-y-2 text-xs">
          <div className="flex justify-between">
            <span className="text-[#666]">Type:</span>
            <span className="text-[#a0a0a0]">{formatScriptType(output.scriptType)}</span>
          </div>
          {output.address && (
            <div className="flex justify-between items-center gap-2">
              <span className="text-[#666]">Address:</span>
              <div className="flex items-center gap-1">
                <code className="text-[#a0a0a0] font-mono text-[10px]">
                  {output.address}
                </code>
                <button
                  onClick={(e) => {
                    e.stopPropagation();
                    copyToClipboard(output.address!);
                  }}
                  className="text-[#666] hover:text-[#a0a0a0]"
                >
                  <Copy className="w-3 h-3" />
                </button>
              </div>
            </div>
          )}
          {!isOpReturn && (
            <div className="flex justify-between">
              <span className="text-[#666]">Value (sats):</span>
              <span className="text-[#a0a0a0]">{output.value.toLocaleString()}</span>
            </div>
          )}
          {output.opReturnData && (
            <div>
              <span className="text-[#666]">OP_RETURN Data:</span>
              <code className="block mt-1 text-[#a0a0a0] font-mono break-all bg-[#252525] p-2 rounded text-[10px]">
                {output.opReturnData.hex}
              </code>
              {output.opReturnData.ascii && (
                <div className="mt-1 text-[#3b82f6]">
                  ASCII: "{output.opReturnData.ascii}"
                </div>
              )}
            </div>
          )}
          <div>
            <span className="text-[#666]">ScriptPubKey:</span>
            <code className="block mt-1 text-[#a0a0a0] font-mono break-all bg-[#252525] p-2 rounded text-[10px]">
              {output.scriptPubKey}
            </code>
          </div>
        </div>
      )}
    </div>
  );
}

// PSBT Input metadata row
function PSBTInputMetadata({ input, index }: { input: PSBTInput; index: number }) {
  const [expanded, setExpanded] = useState(false);

  const hasMetadata = input.sighashType !== undefined ||
    input.redeemScript ||
    input.witnessScript ||
    input.witnessUtxo ||
    input.partialSigs.length > 0;

  if (!hasMetadata) return null;

  return (
    <div className="border border-[#333] rounded-lg p-3 bg-[#1a1a1a]">
      <div
        className="flex items-center gap-2 cursor-pointer"
        onClick={() => setExpanded(!expanded)}
      >
        {expanded ? (
          <ChevronDown className="w-4 h-4 text-[#666]" />
        ) : (
          <ChevronRight className="w-4 h-4 text-[#666]" />
        )}
        <Key className="w-4 h-4 text-[#666]" />
        <span className="text-sm text-[#a0a0a0]">Input #{index} Metadata</span>

        {/* Status badges */}
        <div className="flex gap-1 ml-auto">
          {input.hasAllSignatures && (
            <span className="text-xs px-1.5 py-0.5 rounded bg-[#22c55e]/20 text-[#22c55e]">
              Signed
            </span>
          )}
          {input.witnessUtxo && (
            <span className="text-xs px-1.5 py-0.5 rounded bg-[#3b82f6]/20 text-[#3b82f6]">
              {input.witnessUtxo.valueBtc} BTC
            </span>
          )}
        </div>
      </div>

      {expanded && (
        <div className="mt-3 pt-3 border-t border-[#333] space-y-3 text-xs">
          {/* Witness UTXO info */}
          {input.witnessUtxo && (
            <div className="bg-[#252525] p-2 rounded">
              <div className="text-[#666] mb-1">Witness UTXO:</div>
              <div className="flex justify-between">
                <span className="text-[#666]">Value:</span>
                <span className="text-[#a0a0a0]">{input.witnessUtxo.value.toLocaleString()} sats</span>
              </div>
              {input.witnessUtxo.address && (
                <div className="flex justify-between mt-1">
                  <span className="text-[#666]">Address:</span>
                  <code className="text-[#a0a0a0] font-mono text-[10px]">{input.witnessUtxo.address}</code>
                </div>
              )}
            </div>
          )}

          {/* Sighash type */}
          {input.sighashType !== undefined && (
            <div className="flex justify-between">
              <span className="text-[#666]">Sighash Type:</span>
              <code className="text-[#a0a0a0] font-mono">
                {formatSighashType(input.sighashType)}
              </code>
            </div>
          )}

          {/* Partial signatures */}
          {input.partialSigs.length > 0 && (
            <div>
              <span className="text-[#666]">Partial Signatures ({input.partialSigs.length}):</span>
              <div className="mt-1 space-y-1">
                {input.partialSigs.map((sig, i) => (
                  <div key={i} className="bg-[#252525] p-2 rounded">
                    <div className="text-[#666] text-[10px]">Pubkey:</div>
                    <code className="text-[#a0a0a0] font-mono text-[10px] break-all">{sig.pubkey}</code>
                  </div>
                ))}
              </div>
            </div>
          )}

          {/* Redeem script */}
          {input.redeemScript && (
            <div>
              <span className="text-[#666]">Redeem Script:</span>
              <code className="block mt-1 text-[#a0a0a0] font-mono break-all bg-[#252525] p-2 rounded text-[10px]">
                {input.redeemScript}
              </code>
            </div>
          )}

          {/* Witness script */}
          {input.witnessScript && (
            <div>
              <span className="text-[#666]">Witness Script:</span>
              <code className="block mt-1 text-[#a0a0a0] font-mono break-all bg-[#252525] p-2 rounded text-[10px]">
                {input.witnessScript}
              </code>
            </div>
          )}

          {/* BIP32 derivation */}
          {input.bip32Derivation.length > 0 && (
            <div>
              <span className="text-[#666]">BIP32 Paths:</span>
              <div className="mt-1 space-y-1">
                {input.bip32Derivation.map((d, i) => (
                  <div key={i} className="bg-[#252525] p-2 rounded font-mono">
                    <span className="text-[#a0a0a0]">{d.path}</span>
                    <span className="text-[#666] ml-2 text-[10px]">(fp: {d.masterFingerprint})</span>
                  </div>
                ))}
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  );
}

// Warning banner
function WarningBanner({ warnings }: { warnings: string[] }) {
  if (warnings.length === 0) return null;

  return (
    <div className="bg-[#eab308]/10 border border-[#eab308]/30 rounded-lg p-3">
      <div className="flex items-start gap-2">
        <AlertTriangle className="w-4 h-4 text-[#eab308] mt-0.5 flex-shrink-0" />
        <div className="text-sm text-[#eab308]">
          {warnings.map((w, i) => (
            <div key={i}>{w}</div>
          ))}
        </div>
      </div>
    </div>
  );
}

// Protocol display helper
function getProtocolInfo(protocol: DetectedProtocol): {
  icon: React.ReactNode;
  label: string;
  color: string;
  description: string;
} {
  switch (protocol.type) {
    case 'ordinals': {
      const inscription = protocol.inscription;
      let description = 'Ordinals Inscription detected';
      if (inscription) {
        const sizeStr = inscription.contentLength > 1024
          ? `${(inscription.contentLength / 1024).toFixed(1)} KB`
          : `${inscription.contentLength} bytes`;
        description = `${inscription.contentType} (${sizeStr})`;
        if (inscription.parent) {
          description += ' [Child]';
        }
      } else if (protocol.contentType) {
        description = `Inscription (${protocol.contentType}${protocol.contentLength ? `, ~${protocol.contentLength} bytes` : ''})`;
      }
      return {
        icon: <Gem className="w-3 h-3" />,
        label: 'Ordinals',
        color: '#f97316', // Orange
        description,
      };
    }
    case 'brc20': {
      const details = protocol.details;
      let description = `BRC-20 ${protocol.operation}`;
      if (details) {
        description = `${details.op.toUpperCase()} ${details.tick}`;
        if (details.amt) {
          description += ` • ${details.amt}`;
        }
        if (details.op === 'deploy') {
          if (details.max) description += ` • Max: ${details.max}`;
          if (details.lim) description += ` • Lim: ${details.lim}`;
        }
      } else if (protocol.tick) {
        description = `${protocol.operation.toUpperCase()} ${protocol.tick}${protocol.amount ? ` (${protocol.amount})` : ''}`;
      }
      return {
        icon: <Coins className="w-3 h-3" />,
        label: 'BRC-20',
        color: '#eab308', // Yellow
        description,
      };
    }
    case 'runes': {
      const details = protocol.details;
      let description = 'Runes protocol detected';
      if (details) {
        if (details.cenotaph) {
          description = 'Cenotaph (Invalid Runestone)';
        } else if (details.type === 'etching' && (details.spacedName || details.runeName)) {
          description = `Etching: ${details.spacedName || details.runeName}`;
          if (details.symbol) description += ` (${details.symbol})`;
        } else if (details.type === 'mint' && details.runeId) {
          description = `Mint: Rune ${details.runeId.block}:${details.runeId.tx}`;
        } else if (details.type === 'transfer' && details.edicts.length > 0) {
          description = `Transfer: ${details.edicts.length} edict${details.edicts.length > 1 ? 's' : ''}`;
        } else if (details.type === 'mint') {
          description = 'Mint operation';
        }
      }
      return {
        icon: <Tag className="w-3 h-3" />,
        label: details?.cenotaph ? 'Runes (Invalid)' : 'Runes',
        color: details?.cenotaph ? '#ef4444' : '#8b5cf6', // Red if cenotaph, purple otherwise
        description,
      };
    }
    case 'cat21': {
      const details = protocol.details;
      let description = 'CAT-21 token detected';
      if (details) {
        description = details.type === 'genesis' ? 'CAT-21 Genesis' : 'CAT-21 Transfer';
        if (details.catId) description += ` • ID: ${details.catId}`;
      }
      return {
        icon: <Cat className="w-3 h-3" />,
        label: 'CAT-21',
        color: '#10b981', // Green
        description,
      };
    }
    case 'atomicals': {
      const details = protocol.details;
      let description = 'Atomicals detected';
      if (details) {
        switch (details.type) {
          case 'realm':
            description = `Realm: ${details.realmName || 'unknown'}`;
            break;
          case 'container':
            description = `Container: ${details.containerName || 'unknown'}`;
            break;
          case 'ft':
            description = 'Fungible Token';
            break;
          case 'nft':
            description = 'NFT';
            break;
        }
      }
      return {
        icon: <Atom className="w-3 h-3" />,
        label: 'Atomicals',
        color: '#0ea5e9', // Sky blue
        description,
      };
    }
    case 'stamps':
      return {
        icon: <Stamp className="w-3 h-3" />,
        label: 'Stamps',
        color: '#ec4899', // Pink
        description: 'Bitcoin Stamps data detected',
      };
    case 'counterparty':
      return {
        icon: <Code2 className="w-3 h-3" />,
        label: 'Counterparty',
        color: '#06b6d4', // Cyan
        description: 'Counterparty protocol data detected',
      };
    default:
      return {
        icon: <Tag className="w-3 h-3" />,
        label: 'Unknown',
        color: '#666',
        description: 'Unknown protocol data',
      };
  }
}

// Detailed protocol display components
function OrdinalsDetails({ inscription }: { inscription: OrdinalsInscription }) {
  return (
    <div className="space-y-2 text-xs">
      <div className="flex justify-between">
        <span className="text-[#666]">Content-Type:</span>
        <span className="text-[#a0a0a0]">{inscription.contentType}</span>
      </div>
      <div className="flex justify-between">
        <span className="text-[#666]">Size:</span>
        <span className="text-[#a0a0a0]">
          {inscription.contentLength > 1024
            ? `${(inscription.contentLength / 1024).toFixed(2)} KB`
            : `${inscription.contentLength} bytes`}
        </span>
      </div>
      {inscription.parent && (
        <div className="flex justify-between">
          <span className="text-[#666]">Parent:</span>
          <code className="text-[#f97316] font-mono text-[10px]">{inscription.parent}</code>
        </div>
      )}
      {inscription.pointer !== undefined && (
        <div className="flex justify-between">
          <span className="text-[#666]">Pointer:</span>
          <span className="text-[#a0a0a0]">{inscription.pointer}</span>
        </div>
      )}
      {inscription.metaprotocol && (
        <div className="flex justify-between">
          <span className="text-[#666]">Metaprotocol:</span>
          <span className="text-[#a0a0a0]">{inscription.metaprotocol}</span>
        </div>
      )}
      {inscription.contentPreview && (
        <div>
          <span className="text-[#666]">Preview:</span>
          <code className="block mt-1 text-[#a0a0a0] font-mono break-all bg-[#252525] p-2 rounded text-[10px] max-h-24 overflow-auto">
            {inscription.contentPreview}
          </code>
        </div>
      )}
    </div>
  );
}

function BRC20Details({ details }: { details: BRC20Operation }) {
  return (
    <div className="space-y-2 text-xs">
      <div className="flex justify-between">
        <span className="text-[#666]">Operation:</span>
        <span className="text-[#eab308] font-medium uppercase">{details.op}</span>
      </div>
      <div className="flex justify-between">
        <span className="text-[#666]">Ticker:</span>
        <span className="text-[#a0a0a0] font-mono">{details.tick}</span>
      </div>
      {details.amt && (
        <div className="flex justify-between">
          <span className="text-[#666]">Amount:</span>
          <span className="text-[#a0a0a0]">{details.amt}</span>
        </div>
      )}
      {details.max && (
        <div className="flex justify-between">
          <span className="text-[#666]">Max Supply:</span>
          <span className="text-[#a0a0a0]">{details.max}</span>
        </div>
      )}
      {details.lim && (
        <div className="flex justify-between">
          <span className="text-[#666]">Mint Limit:</span>
          <span className="text-[#a0a0a0]">{details.lim}</span>
        </div>
      )}
      {details.dec && (
        <div className="flex justify-between">
          <span className="text-[#666]">Decimals:</span>
          <span className="text-[#a0a0a0]">{details.dec}</span>
        </div>
      )}
    </div>
  );
}

function RunesDetails({ details }: { details: RunesOperation }) {
  return (
    <div className="space-y-2 text-xs">
      <div className="flex justify-between">
        <span className="text-[#666]">Type:</span>
        <span className="text-[#8b5cf6] font-medium capitalize">{details.type}</span>
      </div>
      {details.cenotaph && (
        <div className="flex justify-between">
          <span className="text-[#666]">Status:</span>
          <span className="text-[#ef4444] font-medium">Cenotaph (Invalid)</span>
        </div>
      )}
      {(details.spacedName || details.runeName) && (
        <div className="flex justify-between">
          <span className="text-[#666]">Rune Name:</span>
          <span className="text-[#a0a0a0] font-mono">{details.spacedName || details.runeName}</span>
        </div>
      )}
      {details.runeId && (
        <div className="flex justify-between">
          <span className="text-[#666]">Rune ID:</span>
          <span className="text-[#a0a0a0] font-mono">{details.runeId.block}:{details.runeId.tx}</span>
        </div>
      )}
      {details.symbol && (
        <div className="flex justify-between">
          <span className="text-[#666]">Symbol:</span>
          <span className="text-[#a0a0a0]">{details.symbol}</span>
        </div>
      )}
      {details.divisibility !== undefined && (
        <div className="flex justify-between">
          <span className="text-[#666]">Divisibility:</span>
          <span className="text-[#a0a0a0]">{details.divisibility}</span>
        </div>
      )}
      {details.turbo && (
        <div className="flex justify-between">
          <span className="text-[#666]">Turbo:</span>
          <span className="text-[#10b981]">Enabled</span>
        </div>
      )}
      {details.premine !== undefined && details.premine > 0n && (
        <div className="flex justify-between">
          <span className="text-[#666]">Premine:</span>
          <span className="text-[#a0a0a0]">{details.premine.toString()}</span>
        </div>
      )}
      {details.supply !== undefined && details.supply > 0n && (
        <div className="flex justify-between">
          <span className="text-[#666]">Total Supply:</span>
          <span className="text-[#a0a0a0]">{details.supply.toString()}</span>
        </div>
      )}
      {details.terms && (
        <div className="mt-2 pt-2 border-t border-[#333]">
          <span className="text-[#666] font-medium">Mint Terms:</span>
          <div className="mt-1 space-y-1 pl-2">
            {details.terms.cap !== undefined && (
              <div className="flex justify-between">
                <span className="text-[#555]">Cap:</span>
                <span className="text-[#a0a0a0]">{details.terms.cap.toString()}</span>
              </div>
            )}
            {details.terms.amount !== undefined && (
              <div className="flex justify-between">
                <span className="text-[#555]">Amount per Mint:</span>
                <span className="text-[#a0a0a0]">{details.terms.amount.toString()}</span>
              </div>
            )}
            {details.terms.height && (
              <div className="flex justify-between">
                <span className="text-[#555]">Height Range:</span>
                <span className="text-[#a0a0a0]">
                  {details.terms.height.start?.toString() || '0'} - {details.terms.height.end?.toString() || '∞'}
                </span>
              </div>
            )}
            {details.terms.offset && (
              <div className="flex justify-between">
                <span className="text-[#555]">Offset Range:</span>
                <span className="text-[#a0a0a0]">
                  {details.terms.offset.start?.toString() || '0'} - {details.terms.offset.end?.toString() || '∞'}
                </span>
              </div>
            )}
          </div>
        </div>
      )}
      {details.edicts.length > 0 && (
        <div className="mt-2 pt-2 border-t border-[#333]">
          <span className="text-[#666]">Edicts ({details.edicts.length}):</span>
          <div className="mt-1 space-y-1 max-h-32 overflow-auto">
            {details.edicts.slice(0, 10).map((edict, i) => (
              <div key={i} className="bg-[#252525] p-1.5 rounded font-mono text-[10px]">
                <span className="text-[#666]">#{i} </span>
                <span className="text-[#a0a0a0]">
                  Rune {edict.id.block}:{edict.id.tx} → Output {edict.output}
                </span>
                <span className="text-[#8b5cf6] ml-1">({edict.amount.toString()})</span>
              </div>
            ))}
            {details.edicts.length > 10 && (
              <div className="text-[#666] text-center">
                ...and {details.edicts.length - 10} more edicts
              </div>
            )}
          </div>
        </div>
      )}
    </div>
  );
}

// Detected protocols display
function ProtocolBadges({ protocols }: { protocols: DetectedProtocol[] }) {
  const [expanded, setExpanded] = useState(false);

  if (!protocols || protocols.length === 0) return null;

  return (
    <div className="bg-[#1a1a1a] border border-[#333] rounded-lg p-3">
      <div
        className="flex items-center gap-2 cursor-pointer"
        onClick={() => setExpanded(!expanded)}
      >
        {expanded ? (
          <ChevronDown className="w-4 h-4 text-[#666]" />
        ) : (
          <ChevronRight className="w-4 h-4 text-[#666]" />
        )}
        <span className="text-sm font-medium text-[#a0a0a0]">
          Detected Protocols ({protocols.length})
        </span>
        <div className="flex gap-1.5 ml-2 flex-wrap">
          {protocols.map((protocol, i) => {
            const info = getProtocolInfo(protocol);
            return (
              <span
                key={i}
                className="text-xs px-2 py-0.5 rounded flex items-center gap-1"
                style={{ backgroundColor: `${info.color}20`, color: info.color }}
              >
                {info.icon}
                {info.label}
              </span>
            );
          })}
        </div>
      </div>

      {expanded && (
        <div className="mt-3 pt-3 border-t border-[#333] space-y-3">
          {protocols.map((protocol, i) => {
            const info = getProtocolInfo(protocol);
            return (
              <div
                key={i}
                className="p-3 rounded-lg"
                style={{ backgroundColor: `${info.color}10`, border: `1px solid ${info.color}30` }}
              >
                <div className="flex items-center gap-2 mb-2">
                  <span style={{ color: info.color }}>{info.icon}</span>
                  <div className="text-sm font-medium" style={{ color: info.color }}>
                    {info.label}
                  </div>
                </div>
                <div className="text-xs text-[#a0a0a0] mb-2">{info.description}</div>

                {/* Detailed protocol info */}
                {protocol.type === 'ordinals' && protocol.inscription && (
                  <OrdinalsDetails inscription={protocol.inscription} />
                )}
                {protocol.type === 'brc20' && protocol.details && (
                  <BRC20Details details={protocol.details} />
                )}
                {protocol.type === 'runes' && protocol.details && (
                  <RunesDetails details={protocol.details} />
                )}
                {protocol.type === 'cat21' && protocol.details && (
                  <div className="space-y-2 text-xs">
                    <div className="flex justify-between">
                      <span className="text-[#666]">Type:</span>
                      <span className="text-[#10b981] font-medium capitalize">
                        {protocol.details.type}
                      </span>
                    </div>
                    {protocol.details.catId && (
                      <div className="flex justify-between">
                        <span className="text-[#666]">CAT ID:</span>
                        <code className="text-[#a0a0a0] font-mono text-[10px]">
                          {protocol.details.catId}
                        </code>
                      </div>
                    )}
                  </div>
                )}
                {protocol.type === 'atomicals' && protocol.details && (
                  <div className="space-y-2 text-xs">
                    <div className="flex justify-between">
                      <span className="text-[#666]">Type:</span>
                      <span className="text-[#0ea5e9] font-medium uppercase">
                        {protocol.details.type}
                      </span>
                    </div>
                    {protocol.details.realmName && (
                      <div className="flex justify-between">
                        <span className="text-[#666]">Realm:</span>
                        <span className="text-[#a0a0a0]">{protocol.details.realmName}</span>
                      </div>
                    )}
                    {protocol.details.containerName && (
                      <div className="flex justify-between">
                        <span className="text-[#666]">Container:</span>
                        <span className="text-[#a0a0a0]">{protocol.details.containerName}</span>
                      </div>
                    )}
                  </div>
                )}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}

// Non-standard transaction report display
function NonStandardReportDisplay({ report }: { report: NonStandardReport }) {
  const [expanded, setExpanded] = useState(false);

  // Don't show anything if transaction is standard
  if (report.isStandard) return null;

  const errorCount = report.checks.filter(c => c.severity === 'error').length;
  const warningCount = report.checks.filter(c => c.severity === 'warning').length;

  // Category display names
  const categoryNames: Record<string, string> = {
    size: 'Size/Weight',
    version: 'Version',
    sigops: 'Sigops',
    witness: 'Witness',
    input: 'Input',
    output: 'Output',
    fee: 'Fee',
  };

  return (
    <div className="bg-[#ef4444]/10 border border-[#ef4444]/30 rounded-lg p-4">
      <div
        className="flex items-center gap-3 cursor-pointer"
        onClick={() => setExpanded(!expanded)}
      >
        {expanded ? (
          <ChevronDown className="w-4 h-4 text-[#ef4444]" />
        ) : (
          <ChevronRight className="w-4 h-4 text-[#ef4444]" />
        )}
        <ShieldAlert className="w-5 h-5 text-[#ef4444]" />
        <div className="flex-1">
          <div className="font-medium text-[#ef4444]">Non-Standard Transaction</div>
          <div className="text-xs text-[#ef4444]/70">{report.summary}</div>
        </div>
        <div className="flex gap-2">
          {errorCount > 0 && (
            <span className="text-xs px-2 py-0.5 rounded bg-[#ef4444]/20 text-[#ef4444] flex items-center gap-1">
              <XCircle className="w-3 h-3" />
              {errorCount} error{errorCount > 1 ? 's' : ''}
            </span>
          )}
          {warningCount > 0 && (
            <span className="text-xs px-2 py-0.5 rounded bg-[#eab308]/20 text-[#eab308] flex items-center gap-1">
              <AlertTriangle className="w-3 h-3" />
              {warningCount} warning{warningCount > 1 ? 's' : ''}
            </span>
          )}
        </div>
      </div>

      {expanded && (
        <div className="mt-4 pt-4 border-t border-[#ef4444]/20 space-y-3">
          {report.checks.map((check, i) => (
            <div
              key={i}
              className={`p-3 rounded-lg ${
                check.severity === 'error'
                  ? 'bg-[#ef4444]/10 border border-[#ef4444]/20'
                  : 'bg-[#eab308]/10 border border-[#eab308]/20'
              }`}
            >
              <div className="flex items-start gap-2">
                {check.severity === 'error' ? (
                  <XCircle className="w-4 h-4 text-[#ef4444] mt-0.5 flex-shrink-0" />
                ) : (
                  <AlertTriangle className="w-4 h-4 text-[#eab308] mt-0.5 flex-shrink-0" />
                )}
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 flex-wrap">
                    <span className={`text-sm font-medium ${
                      check.severity === 'error' ? 'text-[#ef4444]' : 'text-[#eab308]'
                    }`}>
                      {check.message}
                    </span>
                    <span className="text-xs px-1.5 py-0.5 rounded bg-[#333] text-[#a0a0a0]">
                      {categoryNames[check.category] || check.category}
                    </span>
                    <code className="text-xs px-1.5 py-0.5 rounded bg-[#1a1a1a] text-[#666] font-mono">
                      {check.code}
                    </code>
                  </div>
                  {check.details && (
                    <div className="text-xs text-[#a0a0a0] mt-1">{check.details}</div>
                  )}
                  {(check.limit || check.actual) && (
                    <div className="flex gap-4 mt-2 text-xs">
                      {check.limit && (
                        <span className="text-[#666]">
                          Limit: <span className="text-[#a0a0a0]">{check.limit}</span>
                        </span>
                      )}
                      {check.actual && (
                        <span className="text-[#666]">
                          Actual: <span className={check.severity === 'error' ? 'text-[#ef4444]' : 'text-[#eab308]'}>
                            {check.actual}
                          </span>
                        </span>
                      )}
                    </div>
                  )}
                </div>
              </div>
            </div>
          ))}

          <div className="mt-3 p-3 bg-[#1a1a1a] rounded-lg">
            <div className="flex items-start gap-2 text-xs text-[#a0a0a0]">
              <Info className="w-4 h-4 text-[#666] mt-0.5 flex-shrink-0" />
              <div>
                <strong className="text-[#a0a0a0]">What does this mean?</strong>
                <p className="mt-1">
                  Non-standard transactions are rejected by default Bitcoin Core mempool policy,
                  but they&apos;re valid by consensus. They must reach miners directly (via mining pools
                  that accept non-standard TXs like F2Pool, Luxor, or MaraPool).
                </p>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

// Transaction preview content
function TransactionPreviewContent({ tx }: { tx: DecodedTransaction }) {
  return (
    <div className="space-y-4">
      {/* Warnings */}
      <WarningBanner warnings={tx.warnings} />

      {/* Non-Standard Report (only shows if non-standard) */}
      <NonStandardReportDisplay report={tx.nonStandardReport} />

      {/* Detected Protocols */}
      <ProtocolBadges protocols={tx.detectedProtocols} />

      {/* Basic Info Grid */}
      <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
        <div className="bg-[#1a1a1a] rounded-lg p-3">
          <div className="text-xs text-[#666] mb-1">Version</div>
          <div className="text-sm font-medium">{tx.version}</div>
        </div>
        <div className="bg-[#1a1a1a] rounded-lg p-3">
          <div className="text-xs text-[#666] mb-1">Size</div>
          <div className="text-sm font-medium">{tx.size} bytes</div>
        </div>
        <div className="bg-[#1a1a1a] rounded-lg p-3">
          <div className="text-xs text-[#666] mb-1">Virtual Size</div>
          <div className="text-sm font-medium">{tx.vsize} vB</div>
        </div>
        <div className="bg-[#1a1a1a] rounded-lg p-3">
          <div className="text-xs text-[#666] mb-1">Weight</div>
          <div className="text-sm font-medium">{tx.weight} WU</div>
        </div>
      </div>

      {/* Status badges */}
      <div className="flex flex-wrap gap-2">
        {tx.isSegwit && (
          <span className="text-xs px-2 py-1 rounded bg-[#22c55e]/20 text-[#22c55e]">
            SegWit
          </span>
        )}
        {tx.isRbfSignaled && (
          <span className="text-xs px-2 py-1 rounded bg-[#f7931a]/20 text-[#f7931a] flex items-center gap-1">
            <Zap className="w-3 h-3" /> RBF Enabled
          </span>
        )}
        {tx.hasOpReturn && (
          <span className="text-xs px-2 py-1 rounded bg-[#3b82f6]/20 text-[#3b82f6]">
            {tx.opReturnCount} OP_RETURN
          </span>
        )}
        {tx.locktimeType !== 'none' && (
          <span className="text-xs px-2 py-1 rounded bg-[#666]/20 text-[#a0a0a0] flex items-center gap-1">
            <Clock className="w-3 h-3" /> {tx.locktimeValue}
          </span>
        )}
      </div>

      {/* TXID */}
      <div className="bg-[#1a1a1a] rounded-lg p-3">
        <div className="flex items-center justify-between">
          <div className="text-xs text-[#666] mb-1 flex items-center gap-1">
            <Hash className="w-3 h-3" /> Transaction ID (TXID)
          </div>
          <button
            onClick={() => copyToClipboard(tx.txid)}
            className="text-[#666] hover:text-[#a0a0a0]"
          >
            <Copy className="w-3 h-3" />
          </button>
        </div>
        <code className="text-xs font-mono text-[#f7931a] break-all">{tx.txid}</code>
      </div>

      {/* Inputs */}
      <div>
        <div className="flex items-center gap-2 mb-2">
          <ArrowRight className="w-4 h-4 text-[#ef4444]" />
          <span className="text-sm font-medium">Inputs ({tx.inputs.length})</span>
        </div>
        <div className="space-y-2">
          {tx.inputs.map((input, i) => (
            <InputRow key={i} input={input} index={i} />
          ))}
        </div>
      </div>

      {/* Outputs */}
      <div>
        <div className="flex items-center justify-between mb-2">
          <div className="flex items-center gap-2">
            <ArrowLeft className="w-4 h-4 text-[#22c55e]" />
            <span className="text-sm font-medium">Outputs ({tx.outputs.length})</span>
          </div>
          <span className="text-sm text-[#a0a0a0]">
            Total: {tx.totalOutputBtc} <span className="text-[#f7931a]">BTC</span>
          </span>
        </div>
        <div className="space-y-2">
          {tx.outputs.map((output, i) => (
            <OutputRow key={i} output={output} index={i} />
          ))}
        </div>
      </div>
    </div>
  );
}

// PSBT preview content
function PSBTPreviewContent({ psbt }: { psbt: DecodedPSBT }) {
  const [extractedTx, setExtractedTx] = useState<string | null>(null);
  const [extractError, setExtractError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  const finalizationStatus = useMemo(() => canFinalizePSBT(psbt), [psbt]);

  const handleExtract = () => {
    const result = extractTransactionFromPSBT(psbt);
    if (result.success && result.txHex) {
      setExtractedTx(result.txHex);
      setExtractError(null);
    } else {
      setExtractError(result.error || 'Failed to extract transaction');
      setExtractedTx(null);
    }
  };

  const handleCopyExtracted = () => {
    if (extractedTx) {
      navigator.clipboard.writeText(extractedTx);
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    }
  };

  return (
    <div className="space-y-4">
      {/* Warnings */}
      <WarningBanner warnings={psbt.warnings} />

      {/* PSBT Status Banner */}
      <div
        className={`rounded-lg p-4 ${
          psbt.isFullySigned
            ? 'bg-[#22c55e]/10 border border-[#22c55e]/30'
            : 'bg-[#f7931a]/10 border border-[#f7931a]/30'
        }`}
      >
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-3">
            {psbt.isFullySigned ? (
              <>
                <Unlock className="w-5 h-5 text-[#22c55e]" />
                <div>
                  <div className="font-medium text-[#22c55e]">Fully Signed</div>
                  <div className="text-xs text-[#22c55e]/70">
                    Ready to finalize and broadcast
                  </div>
                </div>
              </>
            ) : (
              <>
                <Lock className="w-5 h-5 text-[#f7931a]" />
                <div>
                  <div className="font-medium text-[#f7931a]">
                    Partially Signed Bitcoin Transaction
                  </div>
                  <div className="text-xs text-[#f7931a]/70">{psbt.signingProgress}</div>
                </div>
              </>
            )}
          </div>

          {/* Extract Transaction Button */}
          {finalizationStatus.canFinalize && !extractedTx && (
            <button
              onClick={handleExtract}
              className="flex items-center gap-2 px-3 py-1.5 rounded bg-[#22c55e] hover:bg-[#22c55e]/80 text-black text-sm font-medium transition-colors"
            >
              <Download className="w-4 h-4" />
              Extract Raw TX
            </button>
          )}
        </div>
      </div>

      {/* Extracted Transaction Display */}
      {extractedTx && (
        <div className="bg-[#22c55e]/10 border border-[#22c55e]/30 rounded-lg p-4">
          <div className="flex items-center justify-between mb-2">
            <div className="text-sm font-medium text-[#22c55e] flex items-center gap-2">
              <Check className="w-4 h-4" />
              Raw Transaction Extracted
            </div>
            <button
              onClick={handleCopyExtracted}
              className="flex items-center gap-1 px-2 py-1 rounded bg-[#22c55e]/20 hover:bg-[#22c55e]/30 text-[#22c55e] text-xs font-medium transition-colors"
            >
              {copied ? (
                <>
                  <Check className="w-3 h-3" />
                  Copied!
                </>
              ) : (
                <>
                  <Copy className="w-3 h-3" />
                  Copy TX Hex
                </>
              )}
            </button>
          </div>
          <code className="block text-[10px] font-mono text-[#a0a0a0] break-all bg-[#1a1a1a] p-3 rounded max-h-32 overflow-auto">
            {extractedTx}
          </code>
          <p className="text-xs text-[#22c55e]/70 mt-2">
            This raw transaction can now be broadcast to the Bitcoin network.
          </p>
        </div>
      )}

      {/* Extraction Error */}
      {extractError && (
        <div className="bg-[#ef4444]/10 border border-[#ef4444]/30 rounded-lg p-3">
          <div className="flex items-start gap-2">
            <AlertTriangle className="w-4 h-4 text-[#ef4444] mt-0.5 flex-shrink-0" />
            <div className="text-sm text-[#ef4444]">{extractError}</div>
          </div>
        </div>
      )}

      {/* Cannot Finalize Message */}
      {!finalizationStatus.canFinalize && (
        <div className="bg-[#666]/10 border border-[#666]/30 rounded-lg p-3">
          <div className="flex items-start gap-2">
            <Lock className="w-4 h-4 text-[#666] mt-0.5 flex-shrink-0" />
            <div className="text-sm text-[#a0a0a0]">
              Cannot extract transaction: {finalizationStatus.reason}
            </div>
          </div>
        </div>
      )}

      {/* Fee Info (if available from PSBT) */}
      {psbt.fee !== undefined && (
        <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
          <div className="bg-[#1a1a1a] rounded-lg p-3">
            <div className="text-xs text-[#666] mb-1">Total Input</div>
            <div className="text-sm font-medium">
              {psbt.totalInputBtc} <span className="text-[#f7931a]">BTC</span>
            </div>
          </div>
          <div className="bg-[#1a1a1a] rounded-lg p-3">
            <div className="text-xs text-[#666] mb-1">Total Output</div>
            <div className="text-sm font-medium">
              {psbt.totalOutputBtc} <span className="text-[#f7931a]">BTC</span>
            </div>
          </div>
          <div className="bg-[#1a1a1a] rounded-lg p-3">
            <div className="text-xs text-[#666] mb-1">Fee</div>
            <div className="text-sm font-medium">
              {psbt.feeBtc} <span className="text-[#f7931a]">BTC</span>
            </div>
          </div>
          <div className="bg-[#1a1a1a] rounded-lg p-3">
            <div className="text-xs text-[#666] mb-1">Fee Rate</div>
            <div className="text-sm font-medium">{psbt.feeRate} sat/vB</div>
          </div>
        </div>
      )}

      {/* Show the underlying transaction */}
      <div className="border-t border-[#333] pt-4">
        <div className="text-sm font-medium mb-3 flex items-center gap-2">
          <FileText className="w-4 h-4 text-[#666]" />
          Unsigned Transaction
        </div>
        <TransactionPreviewContent tx={psbt.unsignedTx} />
      </div>

      {/* PSBT Input Details */}
      {psbt.inputs.some((i) =>
        i.sighashType !== undefined || i.redeemScript || i.witnessScript ||
        i.witnessUtxo || i.partialSigs.length > 0
      ) && (
        <div className="border-t border-[#333] pt-4">
          <div className="text-sm font-medium mb-3 flex items-center gap-2">
            <Key className="w-4 h-4 text-[#666]" />
            PSBT Input Details
          </div>
          <div className="space-y-2">
            {psbt.inputs.map((input, i) => (
              <PSBTInputMetadata key={i} input={input} index={i} />
            ))}
          </div>
        </div>
      )}

      {/* Global XPubs */}
      {psbt.globalXpubs.length > 0 && (
        <div className="border-t border-[#333] pt-4">
          <div className="text-sm font-medium mb-3 flex items-center gap-2">
            <Key className="w-4 h-4 text-[#666]" />
            Global XPubs ({psbt.globalXpubs.length})
          </div>
          <div className="space-y-2 text-xs">
            {psbt.globalXpubs.map((xpub, i) => (
              <div key={i} className="bg-[#1a1a1a] rounded p-2">
                <div className="flex justify-between mb-1">
                  <span className="text-[#666]">Path:</span>
                  <code className="text-[#a0a0a0] font-mono">{xpub.path}</code>
                </div>
                <div className="flex justify-between">
                  <span className="text-[#666]">Fingerprint:</span>
                  <code className="text-[#a0a0a0] font-mono">{xpub.masterFingerprint}</code>
                </div>
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Unknown Global Fields */}
      {psbt.unknownGlobals.length > 0 && (
        <div className="border-t border-[#333] pt-4">
          <div className="text-sm font-medium mb-3 flex items-center gap-2">
            <HelpCircle className="w-4 h-4 text-[#eab308]" />
            Unknown Global Fields ({psbt.unknownGlobals.length})
          </div>
          <div className="space-y-2 text-xs">
            {psbt.unknownGlobals.map((field, i) => (
              <div key={i} className="bg-[#1a1a1a] rounded p-2">
                <div className="mb-1">
                  <span className="text-[#666]">Key: </span>
                  <code className="text-[#eab308] font-mono">{field.key}</code>
                </div>
                <div>
                  <span className="text-[#666]">Value: </span>
                  <code className="text-[#a0a0a0] font-mono break-all text-[10px]">{field.value}</code>
                </div>
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}

// Main component
export function TransactionPreview({ rawTx, onValidChange }: TransactionPreviewProps) {
  const debouncedRawTx = useDebounce(rawTx, 300);
  const [decodeResult, setDecodeResult] = useState<DecodeResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [isDecoding, setIsDecoding] = useState(false);

  // Quick input type detection (synchronous)
  const inputType = useMemo(() => detectInputType(rawTx), [rawTx]);

  // Decode when input changes
  useEffect(() => {
    if (!debouncedRawTx.trim()) {
      setDecodeResult(null);
      setError(null);
      onValidChange?.(false, false);
      return;
    }

    if (inputType === 'invalid') {
      setError('Invalid input - must be hex transaction or PSBT');
      setDecodeResult(null);
      onValidChange?.(false, false);
      return;
    }

    setIsDecoding(true);
    setError(null);

    decodeAny(debouncedRawTx)
      .then((result) => {
        setDecodeResult(result);
        setError(null);
        // Can broadcast if it's a raw TX (not PSBT, unless PSBT is fully signed and we support finalization)
        const canBroadcast = result.type === 'transaction' ||
          (result.type === 'psbt' && result.data.isFullySigned);
        onValidChange?.(true, canBroadcast);
      })
      .catch((err) => {
        setError(err.message || 'Failed to decode transaction');
        setDecodeResult(null);
        onValidChange?.(false, false);
      })
      .finally(() => {
        setIsDecoding(false);
      });
  }, [debouncedRawTx, inputType, onValidChange]);

  // Nothing to show
  if (!rawTx.trim()) {
    return null;
  }

  // Loading state
  if (isDecoding) {
    return (
      <div className="bg-[#252525] border border-[#333] rounded-lg p-6">
        <div className="flex items-center justify-center gap-3 text-[#a0a0a0]">
          <Loader2 className="w-5 h-5 animate-spin" />
          <span>Decoding transaction...</span>
        </div>
      </div>
    );
  }

  // Error state
  if (error) {
    return (
      <div className="bg-[#ef4444]/10 border border-[#ef4444]/30 rounded-lg p-4">
        <div className="flex items-start gap-2">
          <AlertTriangle className="w-4 h-4 text-[#ef4444] mt-0.5 flex-shrink-0" />
          <div className="text-sm text-[#ef4444]">{error}</div>
        </div>
      </div>
    );
  }

  // Success state
  if (decodeResult) {
    return (
      <div className="bg-[#252525] border border-[#333] rounded-lg p-4">
        <div className="flex items-center justify-between mb-4 pb-3 border-b border-[#333]">
          <div className="flex items-center gap-2">
            <CheckCircle2 className="w-4 h-4 text-[#22c55e]" />
            <span className="text-sm font-medium">
              {decodeResult.type === 'psbt' ? 'PSBT' : 'Transaction'} Decoded
            </span>
          </div>
          <span className="text-xs px-2 py-1 rounded bg-[#333] text-[#a0a0a0]">
            {decodeResult.type === 'psbt' ? 'PSBT v' + decodeResult.data.version : 'Raw TX'}
          </span>
        </div>

        {decodeResult.type === 'psbt' ? (
          <PSBTPreviewContent psbt={decodeResult.data} />
        ) : (
          <TransactionPreviewContent tx={decodeResult.data} />
        )}
      </div>
    );
  }

  return null;
}

export default TransactionPreview;
