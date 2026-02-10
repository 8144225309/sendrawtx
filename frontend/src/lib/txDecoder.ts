/**
 * Pure JavaScript Bitcoin Transaction Decoder
 * No external dependencies - just byte parsing and math
 */

// ============================================================================
// Types
// ============================================================================

export interface DecodedInput {
  txid: string;
  vout: number;
  scriptSig: string;
  scriptSigAsm: string;
  sequence: number;
  isRbfEnabled: boolean;
  witness?: string[];
}

export interface DecodedOutput {
  value: number; // satoshis
  valueBtc: string;
  scriptPubKey: string;
  scriptType: ScriptType;
  address?: string;
  opReturnData?: {
    hex: string;
    ascii: string;
  };
}

export type ScriptType =
  | 'p2pkh'      // Pay to Public Key Hash (legacy)
  | 'p2sh'       // Pay to Script Hash
  | 'p2wpkh'     // Pay to Witness Public Key Hash (native segwit)
  | 'p2wsh'      // Pay to Witness Script Hash
  | 'p2tr'       // Pay to Taproot
  | 'op_return'  // Data output
  | 'p2pk'       // Pay to Public Key (very old)
  | 'multisig'   // Bare multisig
  | 'nonstandard'| 'unknown';

// ============================================================================
// Non-Standard Transaction Detection Types
// ============================================================================

export type NonStandardCategory =
  | 'size'           // Category 1: Transaction Size/Weight
  | 'version'        // Category 2: Transaction Version
  | 'sigops'         // Category 7: Signature Operations
  | 'witness'        // Category 8: Witness Limits
  | 'input'          // Category 9: Input Limits
  | 'output'         // Category 10: Output Limits
  | 'fee';           // Category 11: Fee Requirements

export interface NonStandardCheck {
  category: NonStandardCategory;
  code: string;           // Bitcoin Core rejection code
  severity: 'warning' | 'error';  // warning = likely rejected, error = definitely rejected
  message: string;
  details?: string;
  limit?: string;         // The standard limit
  actual?: string;        // What the TX has
}

export interface NonStandardReport {
  isStandard: boolean;
  checks: NonStandardCheck[];
  summary: string;        // e.g., "3 non-standard issues detected"
}

// Standard limits from Bitcoin Core policy.h
const LIMITS = {
  MIN_TX_SIZE: 82,                    // Minimum non-witness size
  MAX_STANDARD_TX_WEIGHT: 400000,     // 100 kvB
  MAX_STANDARD_VERSION: 2,
  MAX_OP_RETURN_RELAY: 83,            // OP_RETURN max size
  MAX_OP_RETURN_COUNT: 1,             // Only 1 OP_RETURN allowed
  MAX_SCRIPTSIG_SIZE: 1650,           // ~15-of-15 multisig
  MAX_STANDARD_P2WSH_SCRIPT_SIZE: 3600,
  MAX_STANDARD_P2WSH_STACK_ITEMS: 100,
  MAX_BARE_MULTISIG_PUBKEYS: 3,
  // Dust limits at 3000 sat/kvB
  DUST_P2PKH: 546,
  DUST_P2WPKH: 294,
  DUST_P2TR: 330,
  DUST_P2SH: 540,
  DUST_P2WSH: 330,
};

// ============================================================================
// Protocol Detection Types
// ============================================================================

// Ordinals inscription with full envelope parsing
export interface OrdinalsInscription {
  contentType: string;
  contentLength: number;
  content: Uint8Array;
  contentPreview: string; // ASCII or hex preview
  pointer?: number;
  parent?: string;
  metadata?: Record<string, unknown>;
  metaprotocol?: string;
}

// BRC-20 token operation
export interface BRC20Operation {
  op: 'deploy' | 'mint' | 'transfer';
  tick: string;
  amt?: string;
  max?: string;
  lim?: string;
  dec?: string;
}

// Runes protocol operation
export interface RunesTerms {
  cap?: bigint;
  amount?: bigint;
  height?: { start?: bigint; end?: bigint };
  offset?: { start?: bigint; end?: bigint };
}

export interface RunesOperation {
  type: 'etching' | 'mint' | 'transfer';
  runeName?: string;
  spacedName?: string;
  runeId?: { block: number; tx: number };
  symbol?: string;
  divisibility?: number;
  spacers?: number;
  premine?: bigint;
  terms?: RunesTerms;
  turbo?: boolean;
  supply?: bigint;
  cenotaph?: boolean;
  edicts: Array<{ id: { block: number; tx: number }; amount: bigint; output: number }>;
  flags?: number;
}

// CAT-21 token
export interface CAT21Token {
  type: 'genesis' | 'transfer';
  catId?: string;
}

// Atomicals detection (basic)
export interface AtomicalsOperation {
  type: 'nft' | 'ft' | 'realm' | 'container';
  atomicalId?: string;
  realmName?: string;
  containerName?: string;
}

export type DetectedProtocol =
  | { type: 'ordinals'; contentType?: string; contentLength?: number; inscription?: OrdinalsInscription }
  | { type: 'brc20'; operation: string; tick?: string; amount?: string; details?: BRC20Operation }
  | { type: 'runes'; data: string; details?: RunesOperation }
  | { type: 'cat21'; details?: CAT21Token }
  | { type: 'atomicals'; details?: AtomicalsOperation }
  | { type: 'stamps'; data: string }
  | { type: 'counterparty'; data: string }
  | { type: 'opreturn_unknown'; data: string };

export interface DecodedTransaction {
  txid: string;
  version: number;
  isSegwit: boolean;

  inputs: DecodedInput[];
  outputs: DecodedOutput[];

  locktime: number;
  locktimeType: 'block' | 'timestamp' | 'none';
  locktimeValue: string;

  // Size metrics
  size: number;        // Total bytes
  vsize: number;       // Virtual size
  weight: number;      // Weight units

  // Derived info
  totalOutputSats: number;
  totalOutputBtc: string;
  hasOpReturn: boolean;
  opReturnCount: number;
  isRbfSignaled: boolean;

  // Protocol detection
  detectedProtocols: DetectedProtocol[];

  // Non-standard detection
  nonStandardReport: NonStandardReport;

  // Warnings/notes
  warnings: string[];
}

// ============================================================================
// Hex/Byte Utilities
// ============================================================================

class ByteReader {
  private hex: string;
  private pos: number = 0;

  constructor(hex: string) {
    this.hex = hex.toLowerCase().replace(/\s/g, '');
  }

  get position(): number {
    return this.pos;
  }

  get remaining(): number {
    return (this.hex.length - this.pos) / 2;
  }

  get totalBytes(): number {
    return this.hex.length / 2;
  }

  readBytes(n: number): string {
    if (this.pos + n * 2 > this.hex.length) {
      throw new Error(`Not enough bytes: need ${n}, have ${this.remaining}`);
    }
    const result = this.hex.slice(this.pos, this.pos + n * 2);
    this.pos += n * 2;
    return result;
  }

  readUInt8(): number {
    return parseInt(this.readBytes(1), 16);
  }

  readUInt16LE(): number {
    const bytes = this.readBytes(2);
    return parseInt(bytes.slice(2, 4) + bytes.slice(0, 2), 16);
  }

  readUInt32LE(): number {
    const bytes = this.readBytes(4);
    return parseInt(
      bytes.slice(6, 8) + bytes.slice(4, 6) +
      bytes.slice(2, 4) + bytes.slice(0, 2),
      16
    );
  }

  readUInt64LE(): bigint {
    const bytes = this.readBytes(8);
    let result = 0n;
    for (let i = 7; i >= 0; i--) {
      result = result * 256n + BigInt(parseInt(bytes.slice(i * 2, i * 2 + 2), 16));
    }
    return result;
  }

  readVarInt(): number {
    const first = this.readUInt8();
    if (first < 0xfd) return first;
    if (first === 0xfd) return this.readUInt16LE();
    if (first === 0xfe) return this.readUInt32LE();
    // 0xff - 8 byte int (rarely used for counts)
    return Number(this.readUInt64LE());
  }

  readHash256(): string {
    // Read 32 bytes and reverse for display (Bitcoin shows txids reversed)
    const bytes = this.readBytes(32);
    return reverseHex(bytes);
  }

  peek(n: number): string {
    return this.hex.slice(this.pos, this.pos + n * 2);
  }
}

function reverseHex(hex: string): string {
  const bytes = hex.match(/.{2}/g) || [];
  return bytes.reverse().join('');
}

function hexToAscii(hex: string): string {
  let result = '';
  for (let i = 0; i < hex.length; i += 2) {
    const code = parseInt(hex.slice(i, i + 2), 16);
    // Only include printable ASCII characters
    if (code >= 32 && code < 127) {
      result += String.fromCharCode(code);
    } else {
      result += '.';
    }
  }
  return result;
}

function satsToBtc(sats: number): string {
  return (sats / 100_000_000).toFixed(8);
}

// ============================================================================
// Bech32/Bech32m Encoding (Pure math - no crypto needed)
// ============================================================================

const BECH32_CHARSET = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l';

const BECH32_GENERATOR = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3];

function bech32Polymod(values: number[]): number {
  let chk = 1;
  for (const v of values) {
    const top = chk >> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ v;
    for (let i = 0; i < 5; i++) {
      if ((top >> i) & 1) {
        chk ^= BECH32_GENERATOR[i];
      }
    }
  }
  return chk;
}

function bech32HrpExpand(hrp: string): number[] {
  const result: number[] = [];
  for (const c of hrp) {
    result.push(c.charCodeAt(0) >> 5);
  }
  result.push(0);
  for (const c of hrp) {
    result.push(c.charCodeAt(0) & 31);
  }
  return result;
}

function bech32CreateChecksum(hrp: string, data: number[], isBech32m: boolean): number[] {
  const values = bech32HrpExpand(hrp).concat(data);
  const CONST = isBech32m ? 0x2bc830a3 : 1;
  const polymod = bech32Polymod(values.concat([0, 0, 0, 0, 0, 0])) ^ CONST;
  const checksum: number[] = [];
  for (let i = 0; i < 6; i++) {
    checksum.push((polymod >> (5 * (5 - i))) & 31);
  }
  return checksum;
}

function convertBits(data: number[], fromBits: number, toBits: number, pad: boolean): number[] | null {
  let acc = 0;
  let bits = 0;
  const result: number[] = [];
  const maxv = (1 << toBits) - 1;

  for (const value of data) {
    if (value < 0 || value >> fromBits !== 0) return null;
    acc = (acc << fromBits) | value;
    bits += fromBits;
    while (bits >= toBits) {
      bits -= toBits;
      result.push((acc >> bits) & maxv);
    }
  }

  if (pad) {
    if (bits > 0) {
      result.push((acc << (toBits - bits)) & maxv);
    }
  } else if (bits >= fromBits || ((acc << (toBits - bits)) & maxv) !== 0) {
    return null;
  }

  return result;
}

function encodeBech32(hrp: string, version: number, program: number[], isBech32m: boolean): string {
  const data = [version].concat(convertBits(program, 8, 5, true) || []);
  const checksum = bech32CreateChecksum(hrp, data, isBech32m);
  let result = hrp + '1';
  for (const d of data.concat(checksum)) {
    result += BECH32_CHARSET[d];
  }
  return result;
}

function encodeSegwitAddress(hrp: string, version: number, program: string): string {
  const programBytes: number[] = [];
  for (let i = 0; i < program.length; i += 2) {
    programBytes.push(parseInt(program.slice(i, i + 2), 16));
  }
  // Bech32m for version 1+ (taproot), Bech32 for version 0
  const isBech32m = version > 0;
  return encodeBech32(hrp, version, programBytes, isBech32m);
}

// ============================================================================
// Base58Check Encoding (needs SHA256 from browser crypto)
// ============================================================================

const BASE58_ALPHABET = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';

function base58Encode(bytes: Uint8Array): string {
  // Count leading zeros
  let zeros = 0;
  for (const b of bytes) {
    if (b === 0) zeros++;
    else break;
  }

  // Convert to base58
  const digits: number[] = [0];
  for (const byte of bytes) {
    let carry = byte;
    for (let i = 0; i < digits.length; i++) {
      carry += digits[i] << 8;
      digits[i] = carry % 58;
      carry = Math.floor(carry / 58);
    }
    while (carry > 0) {
      digits.push(carry % 58);
      carry = Math.floor(carry / 58);
    }
  }

  // Build string
  let result = '';
  for (let i = 0; i < zeros; i++) result += '1';
  for (let i = digits.length - 1; i >= 0; i--) {
    result += BASE58_ALPHABET[digits[i]];
  }

  return result;
}

async function sha256(data: ArrayBuffer | Uint8Array): Promise<Uint8Array> {
  // Create a fresh ArrayBuffer to avoid SharedArrayBuffer issues
  let buffer: ArrayBuffer;
  if (data instanceof Uint8Array) {
    buffer = new ArrayBuffer(data.length);
    new Uint8Array(buffer).set(data);
  } else {
    buffer = data;
  }
  const hashBuffer = await crypto.subtle.digest('SHA-256', buffer);
  return new Uint8Array(hashBuffer);
}

async function doubleSha256(data: ArrayBuffer | Uint8Array): Promise<Uint8Array> {
  return sha256(await sha256(data));
}

async function encodeBase58Check(version: number, hash: string): Promise<string> {
  const hashBytes = new Uint8Array(hash.length / 2);
  for (let i = 0; i < hash.length; i += 2) {
    hashBytes[i / 2] = parseInt(hash.slice(i, i + 2), 16);
  }

  const payload = new Uint8Array(1 + hashBytes.length);
  payload[0] = version;
  payload.set(hashBytes, 1);

  const checksum = await doubleSha256(payload);

  const result = new Uint8Array(payload.length + 4);
  result.set(payload);
  result.set(checksum.slice(0, 4), payload.length);

  return base58Encode(result);
}

// ============================================================================
// Script Analysis
// ============================================================================

function identifyScriptType(scriptPubKey: string): { type: ScriptType; data?: string } {
  const len = scriptPubKey.length / 2;

  // OP_RETURN (0x6a)
  if (scriptPubKey.startsWith('6a')) {
    return { type: 'op_return', data: scriptPubKey.slice(2) };
  }

  // P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
  // 76 a9 14 <pubkeyhash:20> 88 ac
  if (len === 25 && scriptPubKey.startsWith('76a914') && scriptPubKey.endsWith('88ac')) {
    return { type: 'p2pkh', data: scriptPubKey.slice(6, 46) };
  }

  // P2SH: OP_HASH160 <20 bytes> OP_EQUAL
  // a9 14 <scripthash:20> 87
  if (len === 23 && scriptPubKey.startsWith('a914') && scriptPubKey.endsWith('87')) {
    return { type: 'p2sh', data: scriptPubKey.slice(4, 44) };
  }

  // P2WPKH: OP_0 <20 bytes>
  // 00 14 <pubkeyhash:20>
  if (len === 22 && scriptPubKey.startsWith('0014')) {
    return { type: 'p2wpkh', data: scriptPubKey.slice(4) };
  }

  // P2WSH: OP_0 <32 bytes>
  // 00 20 <scripthash:32>
  if (len === 34 && scriptPubKey.startsWith('0020')) {
    return { type: 'p2wsh', data: scriptPubKey.slice(4) };
  }

  // P2TR: OP_1 <32 bytes>
  // 51 20 <tweaked_pubkey:32>
  if (len === 34 && scriptPubKey.startsWith('5120')) {
    return { type: 'p2tr', data: scriptPubKey.slice(4) };
  }

  // P2PK: <pubkey> OP_CHECKSIG
  // Compressed: 21 <pubkey:33> ac
  // Uncompressed: 41 <pubkey:65> ac
  if ((len === 35 && scriptPubKey.startsWith('21') && scriptPubKey.endsWith('ac')) ||
      (len === 67 && scriptPubKey.startsWith('41') && scriptPubKey.endsWith('ac'))) {
    return { type: 'p2pk', data: scriptPubKey.slice(2, -2) };
  }

  // Check for bare multisig (starts with OP_1-OP_16, contains pubkeys, ends with OP_CHECKMULTISIG)
  const firstByte = parseInt(scriptPubKey.slice(0, 2), 16);
  if (firstByte >= 0x51 && firstByte <= 0x60 && scriptPubKey.endsWith('ae')) {
    return { type: 'multisig' };
  }

  return { type: 'unknown' };
}

function parseOpReturnData(data: string): { hex: string; ascii: string } {
  // Skip length prefix if present
  let payload = data;
  const firstByte = parseInt(data.slice(0, 2), 16);

  // Handle push opcodes
  if (firstByte <= 0x4b) {
    // Direct push
    payload = data.slice(2);
  } else if (firstByte === 0x4c) {
    // OP_PUSHDATA1
    payload = data.slice(4);
  } else if (firstByte === 0x4d) {
    // OP_PUSHDATA2
    payload = data.slice(6);
  } else if (firstByte === 0x4e) {
    // OP_PUSHDATA4
    payload = data.slice(10);
  }

  return {
    hex: payload,
    ascii: hexToAscii(payload)
  };
}

async function scriptToAddress(scriptType: ScriptType, data: string, isMainnet: boolean = true): Promise<string | undefined> {
  const hrp = isMainnet ? 'bc' : 'tb';

  switch (scriptType) {
    case 'p2wpkh':
      return encodeSegwitAddress(hrp, 0, data);

    case 'p2wsh':
      return encodeSegwitAddress(hrp, 0, data);

    case 'p2tr':
      return encodeSegwitAddress(hrp, 1, data);

    case 'p2pkh':
      // Version 0x00 for mainnet, 0x6f for testnet
      return encodeBase58Check(isMainnet ? 0x00 : 0x6f, data);

    case 'p2sh':
      // Version 0x05 for mainnet, 0xc4 for testnet
      return encodeBase58Check(isMainnet ? 0x05 : 0xc4, data);

    default:
      return undefined;
  }
}

function disassembleScript(script: string): string {
  const OPCODES: Record<number, string> = {
    0x00: 'OP_0', 0x4c: 'OP_PUSHDATA1', 0x4d: 'OP_PUSHDATA2', 0x4e: 'OP_PUSHDATA4',
    0x4f: 'OP_1NEGATE', 0x51: 'OP_1', 0x52: 'OP_2', 0x53: 'OP_3', 0x54: 'OP_4',
    0x55: 'OP_5', 0x56: 'OP_6', 0x57: 'OP_7', 0x58: 'OP_8', 0x59: 'OP_9',
    0x5a: 'OP_10', 0x5b: 'OP_11', 0x5c: 'OP_12', 0x5d: 'OP_13', 0x5e: 'OP_14',
    0x5f: 'OP_15', 0x60: 'OP_16',
    0x76: 'OP_DUP', 0x87: 'OP_EQUAL', 0x88: 'OP_EQUALVERIFY',
    0xa9: 'OP_HASH160', 0xac: 'OP_CHECKSIG', 0xae: 'OP_CHECKMULTISIG',
    0x6a: 'OP_RETURN', 0x75: 'OP_DROP',
  };

  const parts: string[] = [];
  let i = 0;

  while (i < script.length) {
    const opcode = parseInt(script.slice(i, i + 2), 16);
    i += 2;

    if (opcode === 0) {
      parts.push('OP_0');
    } else if (opcode >= 1 && opcode <= 0x4b) {
      // Direct push
      const data = script.slice(i, i + opcode * 2);
      i += opcode * 2;
      parts.push(data);
    } else if (opcode === 0x4c) {
      // OP_PUSHDATA1
      const len = parseInt(script.slice(i, i + 2), 16);
      i += 2;
      const data = script.slice(i, i + len * 2);
      i += len * 2;
      parts.push(data);
    } else if (OPCODES[opcode]) {
      parts.push(OPCODES[opcode]);
    } else {
      parts.push(`OP_UNKNOWN_${opcode.toString(16)}`);
    }
  }

  return parts.join(' ');
}

// ============================================================================
// LEB128 Varint Decoder (for Runes protocol)
// ============================================================================

/**
 * Decode an unsigned LEB128 varint from a byte array
 * Used by the Runes protocol for compact integer encoding
 */
function decodeLEB128(bytes: Uint8Array, offset: number): { value: bigint; bytesRead: number } {
  let result = 0n;
  let shift = 0n;
  let bytesRead = 0;

  while (offset + bytesRead < bytes.length) {
    const byte = bytes[offset + bytesRead];
    result |= BigInt(byte & 0x7f) << shift;
    bytesRead++;
    if ((byte & 0x80) === 0) break;
    shift += 7n;
  }

  return { value: result, bytesRead };
}

/**
 * Decode a sequence of LEB128 varints from a byte array
 */
function decodeAllLEB128(bytes: Uint8Array): bigint[] {
  const values: bigint[] = [];
  let offset = 0;

  while (offset < bytes.length) {
    const { value, bytesRead } = decodeLEB128(bytes, offset);
    values.push(value);
    offset += bytesRead;
    if (bytesRead === 0) break; // Safety check
  }

  return values;
}

// ============================================================================
// Ordinals Envelope Parser
// ============================================================================

/**
 * Parse a hex string to Uint8Array
 */
function hexToBytes(hex: string): Uint8Array {
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < hex.length; i += 2) {
    bytes[i / 2] = parseInt(hex.slice(i, i + 2), 16);
  }
  return bytes;
}

/**
 * Parse Ordinals envelope from witness data
 *
 * Envelope structure:
 * OP_FALSE (0x00)
 * OP_IF (0x63)
 * "ord" (0x036f7264)
 * [tag] [data]...
 *   Tag 1 = content-type
 *   Tag 3 = parent (TXID:INDEX)
 *   Tag 5 = pointer
 *   Tag 7 = metadata (CBOR)
 *   Tag 9 = metaprotocol
 *   Tag 0 = content (empty tag marks content start)
 * OP_ENDIF (0x68)
 */
function parseOrdinalsEnvelope(witness: string[]): OrdinalsInscription | null {
  if (!witness || witness.length === 0) return null;

  // Ordinals inscriptions are typically in the second-to-last witness item for taproot
  // Look through all witness items for the envelope
  for (const item of witness) {
    const itemLower = item.toLowerCase();

    // Look for the envelope marker: OP_FALSE OP_IF "ord"
    // Pattern: 0063 + 03 + 6f7264 (OP_0 OP_IF PUSH3 "ord")
    const envelopeStart = itemLower.indexOf('0063036f7264');
    if (envelopeStart === -1) continue;

    try {
      const bytes = hexToBytes(itemLower);
      const startOffset = envelopeStart / 2 + 6; // Skip OP_0 OP_IF PUSH3 "ord"

      let contentType = '';
      let content = new Uint8Array(0);
      let pointer: number | undefined;
      let parent: string | undefined;
      let metaprotocol: string | undefined;
      let metadata: Record<string, unknown> | undefined;

      let i = startOffset;

      // Parse tag-value pairs until we hit OP_ENDIF (0x68) or end
      while (i < bytes.length) {
        const opcode = bytes[i];

        // OP_ENDIF marks the end
        if (opcode === 0x68) break;

        // OP_0 marks content start (tag 0)
        if (opcode === 0x00) {
          i++;
          // Collect all remaining push data as content
          const contentParts: number[] = [];
          while (i < bytes.length && bytes[i] !== 0x68) {
            const pushLen = bytes[i];
            if (pushLen === 0x00) {
              i++;
              continue;
            }
            if (pushLen <= 0x4b) {
              // Direct push (1-75 bytes)
              i++;
              for (let j = 0; j < pushLen && i < bytes.length && bytes[i] !== 0x68; j++, i++) {
                contentParts.push(bytes[i]);
              }
            } else if (pushLen === 0x4c) {
              // OP_PUSHDATA1
              const len = bytes[i + 1] || 0;
              i += 2;
              for (let j = 0; j < len && i < bytes.length && bytes[i] !== 0x68; j++, i++) {
                contentParts.push(bytes[i]);
              }
            } else if (pushLen === 0x4d) {
              // OP_PUSHDATA2
              const len = (bytes[i + 1] || 0) | ((bytes[i + 2] || 0) << 8);
              i += 3;
              for (let j = 0; j < len && i < bytes.length && bytes[i] !== 0x68; j++, i++) {
                contentParts.push(bytes[i]);
              }
            } else {
              // Non-push opcode or OP_ENDIF - stop
              break;
            }
          }
          content = new Uint8Array(contentParts);
          continue;
        }

        // Read tag (small push)
        let tag = 0;
        if (opcode >= 0x01 && opcode <= 0x10) {
          // OP_1 to OP_16
          tag = opcode - 0x50;
          i++;
        } else if (opcode >= 0x51 && opcode <= 0x60) {
          // OP_1 to OP_16 (alternative encoding)
          tag = opcode - 0x50;
          i++;
        } else {
          i++;
          continue;
        }

        // Read data push
        if (i >= bytes.length) break;
        const dataLen = bytes[i];
        if (dataLen > 0x4b || i + 1 + dataLen > bytes.length) {
          i++;
          continue;
        }
        i++;
        const data = bytes.slice(i, i + dataLen);
        i += dataLen;

        // Process tag
        switch (tag) {
          case 1: // content-type
            contentType = new TextDecoder().decode(data);
            break;
          case 3: // parent
            if (data.length >= 32) {
              const parentTxid = Array.from(data.slice(0, 32))
                .reverse()
                .map(b => b.toString(16).padStart(2, '0'))
                .join('');
              const parentIndex = data.length > 32 ? data[32] : 0;
              parent = `${parentTxid}i${parentIndex}`;
            }
            break;
          case 5: // pointer
            if (data.length > 0) {
              pointer = data[0];
              for (let j = 1; j < data.length; j++) {
                pointer |= data[j] << (j * 8);
              }
            }
            break;
          case 7: // metadata (CBOR - just store raw for now)
            // Basic CBOR parsing could be added here
            break;
          case 9: // metaprotocol
            metaprotocol = new TextDecoder().decode(data);
            break;
        }
      }

      if (!contentType && content.length === 0) continue;

      // Generate content preview
      let contentPreview = '';
      if (contentType.startsWith('text/') || contentType === 'application/json') {
        // Text content - show as string
        try {
          const text = new TextDecoder().decode(content);
          contentPreview = text.length > 200 ? text.slice(0, 200) + '...' : text;
        } catch {
          contentPreview = Array.from(content.slice(0, 64))
            .map(b => b.toString(16).padStart(2, '0'))
            .join('');
        }
      } else {
        // Binary content - show hex preview
        contentPreview = Array.from(content.slice(0, 64))
          .map(b => b.toString(16).padStart(2, '0'))
          .join('');
        if (content.length > 64) contentPreview += '...';
      }

      return {
        contentType: contentType || 'application/octet-stream',
        contentLength: content.length,
        content,
        contentPreview,
        pointer,
        parent,
        metadata,
        metaprotocol,
      };
    } catch {
      continue;
    }
  }

  return null;
}

// ============================================================================
// BRC-20 Parser
// ============================================================================

/**
 * Parse and validate BRC-20 JSON operation
 */
function parseBRC20(json: string): BRC20Operation | null {
  try {
    const obj = JSON.parse(json);

    // Must have p = "brc-20"
    if (obj.p !== 'brc-20') return null;

    // Validate op
    const validOps = ['deploy', 'mint', 'transfer'];
    if (!validOps.includes(obj.op)) return null;

    // Validate tick (should be 4 bytes for standard, but some use more)
    if (!obj.tick || typeof obj.tick !== 'string' || obj.tick.length < 1) {
      return null;
    }

    const result: BRC20Operation = {
      op: obj.op as 'deploy' | 'mint' | 'transfer',
      tick: obj.tick,
    };

    // Validate and add optional fields
    if (obj.amt !== undefined) {
      // Amount should be a positive number string
      if (typeof obj.amt === 'string' && /^\d+(\.\d+)?$/.test(obj.amt)) {
        result.amt = obj.amt;
      }
    }

    if (obj.max !== undefined && typeof obj.max === 'string') {
      result.max = obj.max;
    }

    if (obj.lim !== undefined && typeof obj.lim === 'string') {
      result.lim = obj.lim;
    }

    if (obj.dec !== undefined) {
      result.dec = String(obj.dec);
    }

    return result;
  } catch {
    return null;
  }
}

/**
 * Extract BRC-20 data from inscription content
 */
function detectBrc20FromInscription(inscription: OrdinalsInscription): BRC20Operation | null {
  // BRC-20 must have JSON content type
  if (!inscription.contentType.includes('json') && !inscription.contentType.includes('text/plain')) {
    return null;
  }

  try {
    const text = new TextDecoder().decode(inscription.content);
    return parseBRC20(text);
  } catch {
    return null;
  }
}

// ============================================================================
// Runes Protocol Decoder
// ============================================================================

// Runes Tag constants (from mempool.space implementation)
const RuneTag = {
  Body: 0,
  Flags: 2,
  Rune: 4,
  Premine: 6,
  Cap: 8,
  Amount: 10,
  HeightStart: 12,
  HeightEnd: 14,
  OffsetStart: 16,
  OffsetEnd: 18,
  Mint: 20,
  Pointer: 22,
  Cenotaph: 126,
  // Odd tags
  Divisibility: 1,
  Spacers: 3,
  Symbol: 5,
  Nop: 127,
} as const;

// Runes Flag bits
const RuneFlag = {
  ETCHING: 1n,
  TERMS: 1n << 1n,
  TURBO: 1n << 2n,
  CENOTAPH: 1n << 127n,
};

// Runes alphabet for name encoding (A-Z)
const RUNES_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';

/**
 * Decode a Rune name from its numeric representation
 * Based on mempool.space implementation
 */
function decodeRuneName(rune: bigint): string {
  let name = '';
  rune += 1n; // Add 1 first (mempool.space approach)
  while (rune > 0n) {
    name = RUNES_ALPHABET[Number((rune - 1n) % 26n)] + name;
    rune = (rune - 1n) / 26n;
  }
  return name || 'A';
}

/**
 * Apply spacers to a rune name using bitwise flags
 * Based on mempool.space implementation
 */
function applySpacers(name: string, spacers: bigint): string {
  if (spacers === 0n) return name;

  let result = '';
  let spacerBits = spacers;
  for (let i = 0; i < name.length; i++) {
    result += name[i];
    if (spacerBits & 1n) {
      result += '•';
    }
    if (spacerBits > 0n) {
      spacerBits >>= 1n;
    }
  }
  return result;
}

/**
 * Parse tag-value pairs from LEB128 integers into a message structure
 * Based on mempool.space implementation
 */
function integersToMessage(integers: bigint[]): {
  fields: Map<number, bigint[]>;
  edicts: Array<{ id: { block: number; tx: number }; amount: bigint; output: number }>;
} {
  const fields = new Map<number, bigint[]>();
  const edicts: Array<{ id: { block: number; tx: number }; amount: bigint; output: number }> = [];
  let inBody = false;
  let i = 0;

  while (i < integers.length) {
    if (!inBody) {
      const tag = Number(integers[i++]);
      if (tag === RuneTag.Body) {
        inBody = true;
      } else if (i < integers.length) {
        const value = integers[i++];
        const existing = fields.get(tag);
        if (existing) {
          existing.push(value);
        } else {
          fields.set(tag, [value]);
        }
      }
    } else {
      // Edicts: block height, tx index, amount, output
      if (i + 3 < integers.length) {
        const height = integers[i++];
        const txIndex = integers[i++];
        const amount = integers[i++];
        const output = Number(integers[i++]);
        edicts.push({
          id: { block: Number(height), tx: Number(txIndex) },
          amount,
          output,
        });
      } else {
        break;
      }
    }
    // Safety limit
    if (edicts.length > 100) break;
  }

  return { fields, edicts };
}

/**
 * Parse Runes protocol payload from OP_RETURN data
 * Based on mempool.space implementation
 *
 * Structure:
 * OP_RETURN (0x6a)
 * OP_13 (0x5d) - Runes magic number
 * [payload as LEB128-encoded integers]
 */
function parseRunesPayload(opReturnData: Uint8Array): RunesOperation | null {
  try {
    // Decode all LEB128 values
    const integers = decodeAllLEB128(opReturnData);
    if (integers.length === 0) return null;

    // Parse into message structure
    const message = integersToMessage(integers);
    const fields = message.fields;

    // Get flags
    const flagsValue = fields.get(RuneTag.Flags)?.[0] ?? 0n;
    const hasEtching = (flagsValue & RuneFlag.ETCHING) > 0n;
    const hasTerms = (flagsValue & RuneFlag.TERMS) > 0n;
    const hasTurbo = (flagsValue & RuneFlag.TURBO) > 0n;
    const isCenotaph = fields.has(RuneTag.Cenotaph) || (flagsValue & RuneFlag.CENOTAPH) > 0n;

    const result: RunesOperation = {
      type: hasEtching ? 'etching' : 'transfer',
      edicts: message.edicts,
      flags: Number(flagsValue),
      cenotaph: isCenotaph || undefined,
    };

    // Parse etching if present
    if (hasEtching) {
      // Decode rune name
      const runeValue = fields.get(RuneTag.Rune)?.[0];
      if (runeValue !== undefined) {
        result.runeName = decodeRuneName(runeValue);
      }

      // Divisibility
      const divisibility = fields.get(RuneTag.Divisibility)?.[0];
      if (divisibility !== undefined) {
        result.divisibility = Number(divisibility);
      }

      // Premine
      const premine = fields.get(RuneTag.Premine)?.[0];
      if (premine !== undefined) {
        result.premine = premine;
      }

      // Symbol
      const symbolCode = fields.get(RuneTag.Symbol)?.[0];
      if (symbolCode !== undefined && symbolCode > 0n && symbolCode < 0x10FFFFn) {
        result.symbol = String.fromCodePoint(Number(symbolCode));
      } else {
        result.symbol = '¤'; // Default symbol
      }

      // Spacers
      const spacers = fields.get(RuneTag.Spacers)?.[0];
      if (spacers !== undefined) {
        result.spacers = Number(spacers);
        if (result.runeName) {
          result.spacedName = applySpacers(result.runeName, spacers);
        }
      } else {
        result.spacedName = result.runeName;
      }

      // Turbo mode
      result.turbo = hasTurbo;

      // Parse terms if present
      if (hasTerms) {
        result.terms = {};

        const cap = fields.get(RuneTag.Cap)?.[0];
        if (cap !== undefined) result.terms.cap = cap;

        const amount = fields.get(RuneTag.Amount)?.[0];
        if (amount !== undefined) result.terms.amount = amount;

        const heightStart = fields.get(RuneTag.HeightStart)?.[0];
        const heightEnd = fields.get(RuneTag.HeightEnd)?.[0];
        if (heightStart !== undefined || heightEnd !== undefined) {
          result.terms.height = { start: heightStart, end: heightEnd };
        }

        const offsetStart = fields.get(RuneTag.OffsetStart)?.[0];
        const offsetEnd = fields.get(RuneTag.OffsetEnd)?.[0];
        if (offsetStart !== undefined || offsetEnd !== undefined) {
          result.terms.offset = { start: offsetStart, end: offsetEnd };
        }
      }

      // Calculate supply: (cap * amount) + premine
      const termsCap = result.terms?.cap ?? 0n;
      const termsAmount = result.terms?.amount ?? 0n;
      const premineAmount = result.premine ?? 0n;
      result.supply = (termsCap * termsAmount) + premineAmount;
    }

    // Check for mint operation
    const mintField = fields.get(RuneTag.Mint);
    if (mintField && mintField.length >= 2) {
      result.runeId = { block: Number(mintField[0]), tx: Number(mintField[1]) };
      if (!hasEtching) {
        result.type = 'mint';
      }
    }

    // Determine final type
    if (hasEtching && result.runeName) {
      result.type = 'etching';
    } else if (result.runeId) {
      result.type = 'mint';
    } else if (result.edicts.length > 0) {
      result.type = 'transfer';
    }

    return result;
  } catch {
    return null;
  }
}

// ============================================================================
// CAT-21 Detection
// ============================================================================

/**
 * Detect CAT-21 from inscription content
 * CAT-21 uses Ordinals inscriptions with JSON content containing "p": "cat-21"
 */
function detectCAT21(inscription: OrdinalsInscription): CAT21Token | null {
  // CAT-21 uses JSON content type
  if (!inscription.contentType.includes('json') && !inscription.contentType.includes('text/plain')) {
    return null;
  }

  try {
    const text = new TextDecoder().decode(inscription.content);
    const obj = JSON.parse(text);

    // Check for CAT-21 protocol marker
    if (obj.p === 'cat-21' || obj.protocol === 'cat-21') {
      return {
        type: obj.op === 'mint' || obj.type === 'genesis' ? 'genesis' : 'transfer',
        catId: obj.id || obj.catId,
      };
    }

    return null;
  } catch {
    return null;
  }
}

// ============================================================================
// Atomicals Detection
// ============================================================================

/**
 * Detect Atomicals protocol from witness data
 * Atomicals use a specific envelope format with "atom" marker
 */
function detectAtomicals(witness: string[]): AtomicalsOperation | null {
  if (!witness || witness.length === 0) return null;

  for (const item of witness) {
    const itemLower = item.toLowerCase();

    // Look for "atom" marker (61746f6d in hex)
    if (itemLower.includes('61746f6d')) {
      // Basic detection - more detailed parsing would require full Atomicals spec
      const result: AtomicalsOperation = {
        type: 'nft',
      };

      // Check for realm ($) or container (#) markers in the content
      const ascii = hexToAscii(itemLower);
      if (ascii.includes('"realm"') || ascii.includes('$')) {
        result.type = 'realm';
        // Try to extract realm name
        const realmMatch = ascii.match(/"realm"\s*:\s*"([^"]+)"/);
        if (realmMatch) {
          result.realmName = realmMatch[1];
        }
      } else if (ascii.includes('"container"') || ascii.includes('#')) {
        result.type = 'container';
        const containerMatch = ascii.match(/"container"\s*:\s*"([^"]+)"/);
        if (containerMatch) {
          result.containerName = containerMatch[1];
        }
      } else if (ascii.includes('"ft"') || ascii.includes('"fungible"')) {
        result.type = 'ft';
      }

      return result;
    }
  }

  return null;
}

// ============================================================================
// Protocol Detection
// ============================================================================

/**
 * Detect protocols in OP_RETURN data
 */
function detectOpReturnProtocol(data: string): DetectedProtocol | null {
  const dataLower = data.toLowerCase();

  // Runes detection - two formats:
  // 1. New format (post-launch): OP_13 (0x5d) marker
  // 2. Old format: "R" (0x52) - note: push opcode already stripped by parseOpReturnData
  const isNewRunesFormat = dataLower.startsWith('5d');
  const isOldRunesFormat = dataLower.startsWith('52');

  if (isNewRunesFormat || isOldRunesFormat) {
    // Extract payload after marker (skip the 1-byte marker)
    let payloadHex = data.slice(2);

    // If there's a push opcode for the payload, skip it
    if (payloadHex.length >= 2) {
      const firstByte = parseInt(payloadHex.slice(0, 2), 16);
      if (firstByte <= 0x4b && firstByte > 0) {
        payloadHex = payloadHex.slice(2);
      } else if (firstByte === 0x4c) {
        payloadHex = payloadHex.slice(4);
      } else if (firstByte === 0x4d) {
        payloadHex = payloadHex.slice(6);
      }
    }

    // Parse the Runes payload
    const runesPayload = hexToBytes(payloadHex);
    const runesDetails = parseRunesPayload(runesPayload);

    return {
      type: 'runes',
      data,
      details: runesDetails || undefined,
    };
  }

  // Stamps: STAMP: prefix (5354414d503a in hex)
  const stampsMarker = '5354414d503a';
  if (dataLower.includes(stampsMarker)) {
    return { type: 'stamps', data };
  }

  // Counterparty: CNTRPRTY prefix (434e545250525459 in hex)
  const counterpartyMarker = '434e545250525459';
  if (dataLower.includes(counterpartyMarker)) {
    return { type: 'counterparty', data };
  }

  return null;
}

/**
 * Detect Ordinals inscription in witness data
 * Ordinals use taproot (P2TR) with envelope format:
 * OP_FALSE OP_IF ... "ord" ... content-type ... OP_0 ... data ... OP_ENDIF
 */
function detectOrdinalsInWitness(witness: string[]): DetectedProtocol | null {
  if (!witness || witness.length === 0) return null;

  // Check for "ord" marker in any witness item
  const ordMarker = '6f7264'; // "ord" in hex
  let hasOrdMarker = false;

  for (const item of witness) {
    if (item.toLowerCase().includes(ordMarker)) {
      hasOrdMarker = true;
      break;
    }
  }

  if (!hasOrdMarker) return null;

  // Use the full envelope parser
  const inscription = parseOrdinalsEnvelope(witness);

  if (inscription) {
    return {
      type: 'ordinals',
      contentType: inscription.contentType,
      contentLength: inscription.contentLength,
      inscription,
    };
  }

  // Fallback: basic detection without full parsing
  for (const item of witness) {
    const itemLower = item.toLowerCase();
    if (itemLower.includes(ordMarker)) {
      // Try to extract content-type manually
      let contentType: string | undefined;
      let contentLength: number | undefined;

      const ordPos = itemLower.indexOf(ordMarker);
      if (ordPos !== -1) {
        const afterOrd = itemLower.slice(ordPos + 6);

        // Format: 01 (OP_1) + push_len + content-type + 00 (OP_0) + push_len + data + 68 (OP_ENDIF)
        if (afterOrd.startsWith('01')) {
          const ctLen = parseInt(afterOrd.slice(2, 4), 16);
          if (ctLen > 0 && ctLen < 100) {
            const ctHex = afterOrd.slice(4, 4 + ctLen * 2);
            contentType = hexToAscii(ctHex).replace(/\./g, '');
          }
        }

        const dataStart = afterOrd.indexOf('00');
        if (dataStart !== -1) {
          const remaining = afterOrd.slice(dataStart);
          contentLength = Math.floor(remaining.length / 2) - 2;
        }
      }

      return { type: 'ordinals', contentType, contentLength };
    }
  }

  return null;
}

/**
 * Detect BRC-20 token operation in inscription data
 */
function detectBrc20(witnessOrOpReturn: string): DetectedProtocol | null {
  // BRC-20 inscriptions contain JSON: {"p":"brc-20","op":"...","tick":"...","amt":"..."}
  const ascii = hexToAscii(witnessOrOpReturn);

  // Look for BRC-20 JSON pattern
  if (ascii.includes('"p":"brc-20"') || ascii.includes('"p": "brc-20"')) {
    try {
      // Try to find and parse the JSON
      const jsonMatch = ascii.match(/\{[^{}]*"p"\s*:\s*"brc-20"[^{}]*\}/);
      if (jsonMatch) {
        const brc20Details = parseBRC20(jsonMatch[0]);
        if (brc20Details) {
          return {
            type: 'brc20',
            operation: brc20Details.op,
            tick: brc20Details.tick,
            amount: brc20Details.amt,
            details: brc20Details,
          };
        }
        // Fallback if parsing failed but pattern matched
        const parsed = JSON.parse(jsonMatch[0]);
        return {
          type: 'brc20',
          operation: parsed.op || 'unknown',
          tick: parsed.tick,
          amount: parsed.amt
        };
      }
    } catch {
      // JSON parse failed, but we still detected BRC-20 pattern
      return { type: 'brc20', operation: 'unknown' };
    }
  }

  return null;
}

/**
 * Detect all protocols in a transaction
 */
function detectProtocols(
  inputs: DecodedInput[],
  outputs: DecodedOutput[]
): DetectedProtocol[] {
  const protocols: DetectedProtocol[] = [];
  const seenTypes = new Set<string>();

  // Check OP_RETURN outputs
  for (const output of outputs) {
    if (output.opReturnData) {
      // Check for known OP_RETURN protocols
      const protocol = detectOpReturnProtocol(output.opReturnData.hex);
      if (protocol && !seenTypes.has(protocol.type)) {
        protocols.push(protocol);
        seenTypes.add(protocol.type);
      }
    }
  }

  // Check witness data for Ordinals/BRC-20/CAT-21/Atomicals
  for (const input of inputs) {
    if (input.witness && input.witness.length > 0) {
      // Check for Ordinals
      const ordinals = detectOrdinalsInWitness(input.witness);
      if (ordinals && !seenTypes.has('ordinals')) {
        protocols.push(ordinals);
        seenTypes.add('ordinals');

        // If we have a full inscription, check for BRC-20 and CAT-21
        if (ordinals.type === 'ordinals' && ordinals.inscription) {
          // Check for BRC-20
          const brc20Details = detectBrc20FromInscription(ordinals.inscription);
          if (brc20Details && !seenTypes.has('brc20')) {
            protocols.push({
              type: 'brc20',
              operation: brc20Details.op,
              tick: brc20Details.tick,
              amount: brc20Details.amt,
              details: brc20Details,
            });
            seenTypes.add('brc20');
          }

          // Check for CAT-21
          const cat21 = detectCAT21(ordinals.inscription);
          if (cat21 && !seenTypes.has('cat21')) {
            protocols.push({
              type: 'cat21',
              details: cat21,
            });
            seenTypes.add('cat21');
          }
        } else {
          // Fallback: check witness items directly for BRC-20
          for (const item of input.witness) {
            const brc20 = detectBrc20(item);
            if (brc20 && !seenTypes.has('brc20')) {
              protocols.push(brc20);
              seenTypes.add('brc20');
              break;
            }
          }
        }
      }

      // Check for Atomicals (uses different envelope)
      if (!seenTypes.has('atomicals')) {
        const atomicals = detectAtomicals(input.witness);
        if (atomicals) {
          protocols.push({
            type: 'atomicals',
            details: atomicals,
          });
          seenTypes.add('atomicals');
        }
      }
    }
  }

  return protocols;
}

// ============================================================================
// Non-Standard Transaction Detection
// ============================================================================

/**
 * Get dust limit for a given script type
 */
function getDustLimit(scriptType: ScriptType): number {
  switch (scriptType) {
    case 'p2pkh': return LIMITS.DUST_P2PKH;
    case 'p2wpkh': return LIMITS.DUST_P2WPKH;
    case 'p2tr': return LIMITS.DUST_P2TR;
    case 'p2sh': return LIMITS.DUST_P2SH;
    case 'p2wsh': return LIMITS.DUST_P2WSH;
    case 'op_return': return 0; // OP_RETURN must be 0
    default: return LIMITS.DUST_P2PKH; // Conservative default
  }
}

/**
 * Detect non-standard transaction issues
 * Based on Bitcoin Core policy.h and standardness rules
 */
function detectNonStandard(
  tx: {
    version: number;
    size: number;
    weight: number;
    inputs: DecodedInput[];
    outputs: DecodedOutput[];
    opReturnCount: number;
  }
): NonStandardReport {
  const checks: NonStandardCheck[] = [];

  // ─────────────────────────────────────────────────────────────────────────
  // Category 1: Transaction Size/Weight
  // ─────────────────────────────────────────────────────────────────────────

  if (tx.size < LIMITS.MIN_TX_SIZE) {
    checks.push({
      category: 'size',
      code: 'tx-size-small',
      severity: 'error',
      message: 'Transaction too small',
      details: 'Minimum non-witness size is 82 bytes (smallest valid P2WPKH)',
      limit: `≥${LIMITS.MIN_TX_SIZE} bytes`,
      actual: `${tx.size} bytes`,
    });
  }

  if (tx.weight > LIMITS.MAX_STANDARD_TX_WEIGHT) {
    checks.push({
      category: 'size',
      code: 'tx-size',
      severity: 'error',
      message: 'Transaction too large',
      details: 'Exceeds MAX_STANDARD_TX_WEIGHT (100 kvB)',
      limit: `≤${LIMITS.MAX_STANDARD_TX_WEIGHT} WU`,
      actual: `${tx.weight} WU`,
    });
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Category 2: Transaction Version
  // ─────────────────────────────────────────────────────────────────────────

  if (tx.version < 1 || tx.version > LIMITS.MAX_STANDARD_VERSION) {
    checks.push({
      category: 'version',
      code: 'version',
      severity: 'error',
      message: `Non-standard version: ${tx.version}`,
      details: 'Only version 1 and 2 are standard. Version 3 reserved for package relay.',
      limit: '1 or 2',
      actual: `${tx.version}`,
    });
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Category 9: Input Limits
  // ─────────────────────────────────────────────────────────────────────────

  // Check for coinbase transaction (non-relayable)
  const isCoinbase = tx.inputs.length === 1 &&
    tx.inputs[0].txid === '0000000000000000000000000000000000000000000000000000000000000000' &&
    tx.inputs[0].vout === 0xffffffff;

  if (isCoinbase) {
    checks.push({
      category: 'input',
      code: 'coinbase',
      severity: 'error',
      message: 'Coinbase transaction',
      details: 'Coinbase transactions cannot be relayed - they are only valid in blocks',
    });
  }

  for (let i = 0; i < tx.inputs.length; i++) {
    const input = tx.inputs[i];

    // ScriptSig size limit
    const scriptSigSize = input.scriptSig.length / 2;
    if (scriptSigSize > LIMITS.MAX_SCRIPTSIG_SIZE) {
      checks.push({
        category: 'input',
        code: 'bad-txns-nonstandard-inputs',
        severity: 'error',
        message: `Input #${i}: scriptSig too large`,
        details: 'ScriptSig exceeds maximum standard size (~15-of-15 multisig limit)',
        limit: `≤${LIMITS.MAX_SCRIPTSIG_SIZE} bytes`,
        actual: `${scriptSigSize} bytes`,
      });
    }

    // Check for non-push-only scriptSig (basic check)
    // Push-only means no opcodes other than data pushes
    if (input.scriptSig && input.scriptSig.length > 0) {
      const firstByte = parseInt(input.scriptSig.slice(0, 2), 16);
      // Opcodes 0x01-0x4e are pushes, 0x00 is OP_0 (push empty), >0x60 are non-push
      if (firstByte > 0x60 && firstByte !== 0x00) {
        checks.push({
          category: 'input',
          code: 'bad-txns-nonstandard-inputs',
          severity: 'warning',
          message: `Input #${i}: scriptSig may contain non-push opcodes`,
          details: 'ScriptSig must be push-only (no executable opcodes)',
        });
      }
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Category 8: Witness Limits
  // ─────────────────────────────────────────────────────────────────────────

  for (let i = 0; i < tx.inputs.length; i++) {
    const input = tx.inputs[i];
    if (input.witness && input.witness.length > 0) {
      // Stack item count
      if (input.witness.length > LIMITS.MAX_STANDARD_P2WSH_STACK_ITEMS) {
        checks.push({
          category: 'witness',
          code: 'bad-witness-nonstandard',
          severity: 'error',
          message: `Input #${i}: too many witness stack items`,
          limit: `≤${LIMITS.MAX_STANDARD_P2WSH_STACK_ITEMS}`,
          actual: `${input.witness.length}`,
        });
      }

      // Check each witness item size (80 bytes max for P2WSH)
      for (let j = 0; j < input.witness.length; j++) {
        const itemSize = input.witness[j].length / 2;
        if (itemSize > 80 && j < input.witness.length - 1) {
          // Last item can be the script which is allowed to be larger
          checks.push({
            category: 'witness',
            code: 'bad-witness-nonstandard',
            severity: 'warning',
            message: `Input #${i}: witness item #${j} exceeds 80 bytes`,
            details: 'P2WSH stack items (except script) limited to 80 bytes',
            limit: '≤80 bytes',
            actual: `${itemSize} bytes`,
          });
        }
      }

      // Check last witness item (script) size for P2WSH
      const lastItem = input.witness[input.witness.length - 1];
      const scriptSize = lastItem.length / 2;
      if (scriptSize > LIMITS.MAX_STANDARD_P2WSH_SCRIPT_SIZE) {
        checks.push({
          category: 'witness',
          code: 'bad-witness-nonstandard',
          severity: 'error',
          message: `Input #${i}: P2WSH script too large`,
          limit: `≤${LIMITS.MAX_STANDARD_P2WSH_SCRIPT_SIZE} bytes`,
          actual: `${scriptSize} bytes`,
        });
      }
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Category 10: Output Limits
  // ─────────────────────────────────────────────────────────────────────────

  // Multiple OP_RETURN outputs
  if (tx.opReturnCount > LIMITS.MAX_OP_RETURN_COUNT) {
    checks.push({
      category: 'output',
      code: 'multi-op-return',
      severity: 'error',
      message: 'Multiple OP_RETURN outputs',
      details: 'Only one OP_RETURN data carrier output is standard',
      limit: `≤${LIMITS.MAX_OP_RETURN_COUNT}`,
      actual: `${tx.opReturnCount}`,
    });
  }

  for (let i = 0; i < tx.outputs.length; i++) {
    const output = tx.outputs[i];

    // OP_RETURN size limit
    if (output.scriptType === 'op_return') {
      const opReturnSize = output.scriptPubKey.length / 2;
      if (opReturnSize > LIMITS.MAX_OP_RETURN_RELAY) {
        checks.push({
          category: 'output',
          code: 'scriptpubkey',
          severity: 'error',
          message: `Output #${i}: OP_RETURN too large`,
          details: 'OP_RETURN data exceeds relay limit',
          limit: `≤${LIMITS.MAX_OP_RETURN_RELAY} bytes`,
          actual: `${opReturnSize} bytes`,
        });
      }
      // OP_RETURN must have 0 value
      if (output.value > 0) {
        checks.push({
          category: 'output',
          code: 'dust',
          severity: 'error',
          message: `Output #${i}: OP_RETURN has non-zero value`,
          details: 'OP_RETURN outputs must be unspendable (0 sats)',
          limit: '0 sats',
          actual: `${output.value} sats`,
        });
      }
      continue;
    }

    // Unknown/non-standard output types
    if (output.scriptType === 'unknown' || output.scriptType === 'nonstandard') {
      checks.push({
        category: 'output',
        code: 'scriptpubkey',
        severity: 'error',
        message: `Output #${i}: non-standard script type`,
        details: 'Output script does not match any standard template',
      });
    }

    // Bare multisig check
    if (output.scriptType === 'multisig') {
      checks.push({
        category: 'output',
        code: 'bare-multisig',
        severity: 'warning',
        message: `Output #${i}: bare multisig output`,
        details: 'Bare multisig is non-standard by default (use P2SH-wrapped instead)',
      });
    }

    // Dust check (skip OP_RETURN)
    const dustLimit = getDustLimit(output.scriptType);
    if (output.value > 0 && output.value < dustLimit) {
      checks.push({
        category: 'output',
        code: 'dust',
        severity: 'error',
        message: `Output #${i}: dust output`,
        details: `Value below economic spend threshold for ${output.scriptType.toUpperCase()}`,
        limit: `≥${dustLimit} sats`,
        actual: `${output.value} sats`,
      });
    }
  }

  // Build summary
  const isStandard = checks.length === 0;
  const errorCount = checks.filter(c => c.severity === 'error').length;
  const warningCount = checks.filter(c => c.severity === 'warning').length;

  let summary: string;
  if (isStandard) {
    summary = 'Transaction passes all standard policy checks';
  } else if (errorCount > 0 && warningCount > 0) {
    summary = `${errorCount} policy violation${errorCount > 1 ? 's' : ''}, ${warningCount} warning${warningCount > 1 ? 's' : ''}`;
  } else if (errorCount > 0) {
    summary = `${errorCount} policy violation${errorCount > 1 ? 's' : ''} - will be rejected by default nodes`;
  } else {
    summary = `${warningCount} warning${warningCount > 1 ? 's' : ''} - may be rejected by some nodes`;
  }

  return { isStandard, checks, summary };
}

// ============================================================================
// Main Decoder
// ============================================================================

export async function decodeTransaction(hexInput: string): Promise<DecodedTransaction> {
  // Sanitize input to ensure accurate size calculation
  const hex = hexInput.replace(/\s+/g, '');
  const reader = new ByteReader(hex);
  const warnings: string[] = [];

  // Version (4 bytes)
  const version = reader.readUInt32LE();
  if (version !== 1 && version !== 2) {
    warnings.push(`Unusual version: ${version}`);
  }

  // Check for SegWit marker
  let isSegwit = false;
  const marker = reader.peek(1); // Peek 1 byte (2 hex chars)
  if (marker === '00') {
    reader.readUInt8(); // marker
    const flag = reader.readUInt8();
    if (flag !== 1) {
      throw new Error(`Invalid SegWit flag: ${flag}`);
    }
    isSegwit = true;
  }

  // Input count
  const inputCount = reader.readVarInt();
  if (inputCount === 0 && !isSegwit) {
    throw new Error('Transaction has no inputs');
  }

  // Inputs
  const inputs: DecodedInput[] = [];
  let isRbfSignaled = false;

  for (let i = 0; i < inputCount; i++) {
    const txid = reader.readHash256();
    const vout = reader.readUInt32LE();
    const scriptSigLen = reader.readVarInt();
    const scriptSig = reader.readBytes(scriptSigLen);
    const sequence = reader.readUInt32LE();

    const isRbfEnabled = sequence < 0xfffffffe;
    if (isRbfEnabled) isRbfSignaled = true;

    inputs.push({
      txid,
      vout,
      scriptSig,
      scriptSigAsm: scriptSig ? disassembleScript(scriptSig) : '(empty)',
      sequence,
      isRbfEnabled,
    });
  }

  // Output count
  const outputCount = reader.readVarInt();

  // Outputs
  const outputs: DecodedOutput[] = [];
  let totalOutputSats = 0;
  let hasOpReturn = false;
  let opReturnCount = 0;

  for (let i = 0; i < outputCount; i++) {
    const value = Number(reader.readUInt64LE());
    totalOutputSats += value;

    const scriptPubKeyLen = reader.readVarInt();
    const scriptPubKey = reader.readBytes(scriptPubKeyLen);

    const { type: scriptType, data } = identifyScriptType(scriptPubKey);

    const output: DecodedOutput = {
      value,
      valueBtc: satsToBtc(value),
      scriptPubKey,
      scriptType,
    };

    // Get address for standard types
    if (data && scriptType !== 'op_return') {
      output.address = await scriptToAddress(scriptType, data);
    }

    // Handle OP_RETURN
    if (scriptType === 'op_return') {
      hasOpReturn = true;
      opReturnCount++;
      if (data) {
        output.opReturnData = parseOpReturnData(data);
      }
    }

    outputs.push(output);
  }

  // Witness data (if SegWit)
  if (isSegwit) {
    for (let i = 0; i < inputCount; i++) {
      const witnessCount = reader.readVarInt();
      const witness: string[] = [];
      for (let j = 0; j < witnessCount; j++) {
        const itemLen = reader.readVarInt();
        witness.push(reader.readBytes(itemLen));
      }
      inputs[i].witness = witness;
    }
  }

  // Locktime (4 bytes)
  const locktime = reader.readUInt32LE();
  let locktimeType: 'block' | 'timestamp' | 'none' = 'none';
  let locktimeValue = 'None (immediately spendable)';

  if (locktime > 0) {
    if (locktime < 500_000_000) {
      locktimeType = 'block';
      locktimeValue = `Block ${locktime.toLocaleString()}`;
    } else {
      locktimeType = 'timestamp';
      const date = new Date(locktime * 1000);
      locktimeValue = date.toISOString();
    }
  }

  // Calculate sizes
  const size = hex.length / 2;
  let weight: number;
  let vsize: number;

  if (isSegwit) {
    // For SegWit, we need to calculate non-witness and witness sizes
    // This is an approximation - for exact, we'd need to track positions
    const baseSize = size - (isSegwit ? 2 : 0); // Subtract marker+flag
    // Rough witness discount calculation
    weight = baseSize * 3 + size;
    vsize = Math.ceil(weight / 4);
  } else {
    weight = size * 4;
    vsize = size;
  }

  // Calculate TXID
  const txid = await calculateTxid(hex, isSegwit);

  // Sanity checks
  if (reader.remaining > 0) {
    warnings.push(`${reader.remaining} bytes remaining after parsing`);
  }

  if (totalOutputSats > 21_000_000 * 100_000_000) {
    warnings.push('Total output exceeds 21M BTC!');
  }

  // Detect protocols (Ordinals, BRC-20, Runes, etc.)
  const detectedProtocols = detectProtocols(inputs, outputs);

  // Detect non-standard policy issues
  const nonStandardReport = detectNonStandard({
    version,
    size,
    weight,
    inputs,
    outputs,
    opReturnCount,
  });

  return {
    txid,
    version,
    isSegwit,
    inputs,
    outputs,
    locktime,
    locktimeType,
    locktimeValue,
    size,
    vsize,
    weight,
    totalOutputSats,
    totalOutputBtc: satsToBtc(totalOutputSats),
    hasOpReturn,
    opReturnCount,
    isRbfSignaled,
    detectedProtocols,
    nonStandardReport,
    warnings,
  };
}

async function calculateTxid(hex: string, isSegwit: boolean): Promise<string> {
  let txForHash = hex;

  if (isSegwit) {
    // For SegWit, TXID is calculated without witness data
    // This is a simplified approach - strip marker, flag, and witness
    const reader = new ByteReader(hex);

    // Version
    const version = reader.readBytes(4);
    reader.readBytes(2); // Skip marker+flag

    // Count inputs
    const inputCount = reader.readVarInt();
    const inputCountHex = inputCount < 0xfd ?
      inputCount.toString(16).padStart(2, '0') :
      'fd' + inputCount.toString(16).padStart(4, '0');

    let inputsHex = '';
    for (let i = 0; i < inputCount; i++) {
      inputsHex += reader.readBytes(32); // txid (unreversed in raw)
      inputsHex += reader.readBytes(4);  // vout
      const scriptLen = reader.readVarInt();
      const scriptLenHex = scriptLen < 0xfd ?
        scriptLen.toString(16).padStart(2, '0') :
        'fd' + scriptLen.toString(16).padStart(4, '0');
      inputsHex += scriptLenHex;
      inputsHex += reader.readBytes(scriptLen); // scriptSig
      inputsHex += reader.readBytes(4); // sequence
    }

    // Count outputs
    const outputCount = reader.readVarInt();
    const outputCountHex = outputCount < 0xfd ?
      outputCount.toString(16).padStart(2, '0') :
      'fd' + outputCount.toString(16).padStart(4, '0');

    let outputsHex = '';
    for (let i = 0; i < outputCount; i++) {
      outputsHex += reader.readBytes(8); // value
      const scriptLen = reader.readVarInt();
      const scriptLenHex = scriptLen < 0xfd ?
        scriptLen.toString(16).padStart(2, '0') :
        'fd' + scriptLen.toString(16).padStart(4, '0');
      outputsHex += scriptLenHex;
      outputsHex += reader.readBytes(scriptLen); // scriptPubKey
    }

    // Skip witness data, get locktime
    for (let i = 0; i < inputCount; i++) {
      const witnessCount = reader.readVarInt();
      for (let j = 0; j < witnessCount; j++) {
        const itemLen = reader.readVarInt();
        reader.readBytes(itemLen);
      }
    }

    const locktime = reader.readBytes(4);

    txForHash = version + inputCountHex + inputsHex + outputCountHex + outputsHex + locktime;
  }

  // Convert to bytes
  const bytes = new Uint8Array(txForHash.length / 2);
  for (let i = 0; i < txForHash.length; i += 2) {
    bytes[i / 2] = parseInt(txForHash.slice(i, i + 2), 16);
  }

  // Double SHA256
  const hash = await doubleSha256(bytes);

  // Reverse for display
  return Array.from(hash).reverse().map(b => b.toString(16).padStart(2, '0')).join('');
}

// ============================================================================
// Quick validation (synchronous, for form validation)
// ============================================================================

export function isValidTransactionHex(hex: string): { valid: boolean; error?: string } {
  if (!hex || hex.length === 0) {
    return { valid: false, error: 'Empty input' };
  }

  const cleaned = hex.trim().toLowerCase();

  if (!/^[0-9a-f]+$/.test(cleaned)) {
    return { valid: false, error: 'Invalid hex characters' };
  }

  if (cleaned.length % 2 !== 0) {
    return { valid: false, error: 'Odd number of hex characters' };
  }

  if (cleaned.length < 20) {
    return { valid: false, error: 'Too short to be a valid transaction' };
  }

  // Check version (first 4 bytes, little-endian)
  const version = parseInt(cleaned.slice(6, 8) + cleaned.slice(4, 6) + cleaned.slice(2, 4) + cleaned.slice(0, 2), 16);
  if (version !== 1 && version !== 2) {
    // Could still be valid but unusual
  }

  return { valid: true };
}

// ============================================================================
// PSBT (BIP-174) Decoder
// ============================================================================

export interface PSBTInput {
  index: number;
  witnessUtxo?: {
    value: number;
    valueBtc: string;
    scriptPubKey: string;
    scriptType: ScriptType;
    address?: string;
  };
  nonWitnessUtxo?: string; // Full previous tx hex
  partialSigs: Array<{ pubkey: string; signature: string }>;
  sighashType?: number;
  redeemScript?: string;
  witnessScript?: string;
  bip32Derivation: Array<{
    pubkey: string;
    masterFingerprint: string;
    path: string;
  }>;
  finalScriptSig?: string;
  finalScriptWitness?: string[];
  hasAllSignatures: boolean;
}

export interface PSBTOutput {
  index: number;
  redeemScript?: string;
  witnessScript?: string;
  bip32Derivation: Array<{
    pubkey: string;
    masterFingerprint: string;
    path: string;
  }>;
}

export interface DecodedPSBT {
  isPsbt: true;
  version: number;
  unsignedTx: DecodedTransaction;
  inputs: PSBTInput[];
  outputs: PSBTOutput[];
  globalXpubs: Array<{
    xpub: string;
    masterFingerprint: string;
    path: string;
  }>;
  unknownGlobals: Array<{ key: string; value: string }>;

  // Derived info
  totalInputValue: number;
  totalInputBtc: string;
  totalOutputValue: number;
  totalOutputBtc: string;
  fee?: number;
  feeBtc?: string;
  feeRate?: number; // sat/vB

  isFullySigned: boolean;
  signingProgress: string; // e.g., "2/3 inputs signed"
  warnings: string[];
}

// PSBT key types
const PSBT_GLOBAL_UNSIGNED_TX = 0x00;
const PSBT_GLOBAL_XPUB = 0x01;
const PSBT_GLOBAL_VERSION = 0xfb;

const PSBT_IN_NON_WITNESS_UTXO = 0x00;
const PSBT_IN_WITNESS_UTXO = 0x01;
const PSBT_IN_PARTIAL_SIG = 0x02;
const PSBT_IN_SIGHASH_TYPE = 0x03;
const PSBT_IN_REDEEM_SCRIPT = 0x04;
const PSBT_IN_WITNESS_SCRIPT = 0x05;
const PSBT_IN_BIP32_DERIVATION = 0x06;
const PSBT_IN_FINAL_SCRIPTSIG = 0x07;
const PSBT_IN_FINAL_SCRIPTWITNESS = 0x08;

const PSBT_OUT_REDEEM_SCRIPT = 0x00;
const PSBT_OUT_WITNESS_SCRIPT = 0x01;
const PSBT_OUT_BIP32_DERIVATION = 0x02;

function parseBip32Path(data: string): string {
  // First 4 bytes are master fingerprint, rest are path components (4 bytes each)
  const parts: string[] = ['m'];
  for (let i = 8; i < data.length; i += 8) {
    const index = parseInt(
      data.slice(i + 6, i + 8) + data.slice(i + 4, i + 6) +
      data.slice(i + 2, i + 4) + data.slice(i, i + 2),
      16
    );
    if (index >= 0x80000000) {
      parts.push((index - 0x80000000).toString() + "'");
    } else {
      parts.push(index.toString());
    }
  }
  return parts.join('/');
}

export function isPSBT(data: string): boolean {
  const cleaned = data.trim().toLowerCase();
  // Check for hex PSBT magic
  if (cleaned.startsWith('70736274ff')) {
    return true;
  }
  // Check for base64 PSBT (starts with "cHNidP8" which is base64 for "psbt\xff")
  if (data.trim().startsWith('cHNidP8')) {
    return true;
  }
  return false;
}

function base64ToHex(base64: string): string {
  const binary = atob(base64);
  let hex = '';
  for (let i = 0; i < binary.length; i++) {
    hex += binary.charCodeAt(i).toString(16).padStart(2, '0');
  }
  return hex;
}

export async function decodePSBT(dataInput: string): Promise<DecodedPSBT> {
  // Sanitize input - remove all whitespace
  const data = dataInput.replace(/\s+/g, '');
  let hex = data.toLowerCase();

  // Convert base64 to hex if needed
  if (data.startsWith('cHNidP8')) {
    hex = base64ToHex(data);
  }

  const reader = new ByteReader(hex);
  const warnings: string[] = [];

  // Check magic bytes (0x70736274ff = "psbt" + separator)
  const magic = reader.readBytes(5);
  if (magic !== '70736274ff') {
    throw new Error('Invalid PSBT magic bytes');
  }

  // Parse global section
  let unsignedTxHex: string | undefined;
  let psbtVersion = 0;
  const globalXpubs: DecodedPSBT['globalXpubs'] = [];
  const unknownGlobals: Array<{ key: string; value: string }> = [];

  while (true) {
    const keyLen = reader.readVarInt();
    if (keyLen === 0) break; // End of global section

    const key = reader.readBytes(keyLen);
    const keyType = parseInt(key.slice(0, 2), 16);
    const keyData = key.slice(2);

    const valueLen = reader.readVarInt();
    const value = reader.readBytes(valueLen);

    switch (keyType) {
      case PSBT_GLOBAL_UNSIGNED_TX:
        unsignedTxHex = value;
        break;
      case PSBT_GLOBAL_XPUB:
        globalXpubs.push({
          xpub: keyData,
          masterFingerprint: value.slice(0, 8),
          path: parseBip32Path(value),
        });
        break;
      case PSBT_GLOBAL_VERSION:
        psbtVersion = parseInt(value, 16);
        break;
      default:
        unknownGlobals.push({ key, value });
    }
  }

  if (!unsignedTxHex) {
    throw new Error('PSBT missing unsigned transaction');
  }

  // Decode the unsigned transaction
  const unsignedTx = await decodeTransaction(unsignedTxHex);

  // Parse input sections
  const psbtInputs: PSBTInput[] = [];
  let totalInputValue = 0;
  let inputsWithValue = 0;

  for (let i = 0; i < unsignedTx.inputs.length; i++) {
    const input: PSBTInput = {
      index: i,
      partialSigs: [],
      bip32Derivation: [],
      hasAllSignatures: false,
    };

    while (true) {
      const keyLen = reader.readVarInt();
      if (keyLen === 0) break;

      const key = reader.readBytes(keyLen);
      const keyType = parseInt(key.slice(0, 2), 16);
      const keyData = key.slice(2);

      const valueLen = reader.readVarInt();
      const value = reader.readBytes(valueLen);

      switch (keyType) {
        case PSBT_IN_NON_WITNESS_UTXO:
          input.nonWitnessUtxo = value;
          break;

        case PSBT_IN_WITNESS_UTXO: {
          // Parse witness UTXO: value (8 bytes) + scriptPubKey
          const utxoReader = new ByteReader(value);
          const utxoValue = Number(utxoReader.readUInt64LE());
          const scriptLen = utxoReader.readVarInt();
          const scriptPubKey = utxoReader.readBytes(scriptLen);
          const { type: scriptType, data: scriptData } = identifyScriptType(scriptPubKey);

          input.witnessUtxo = {
            value: utxoValue,
            valueBtc: satsToBtc(utxoValue),
            scriptPubKey,
            scriptType,
            address: scriptData ? await scriptToAddress(scriptType, scriptData) : undefined,
          };

          totalInputValue += utxoValue;
          inputsWithValue++;
          break;
        }

        case PSBT_IN_PARTIAL_SIG:
          input.partialSigs.push({
            pubkey: keyData,
            signature: value,
          });
          break;

        case PSBT_IN_SIGHASH_TYPE:
          input.sighashType = parseInt(value, 16);
          break;

        case PSBT_IN_REDEEM_SCRIPT:
          input.redeemScript = value;
          break;

        case PSBT_IN_WITNESS_SCRIPT:
          input.witnessScript = value;
          break;

        case PSBT_IN_BIP32_DERIVATION:
          input.bip32Derivation.push({
            pubkey: keyData,
            masterFingerprint: value.slice(0, 8),
            path: parseBip32Path(value),
          });
          break;

        case PSBT_IN_FINAL_SCRIPTSIG:
          input.finalScriptSig = value;
          input.hasAllSignatures = true;
          break;

        case PSBT_IN_FINAL_SCRIPTWITNESS: {
          // Parse witness stack
          const witnessReader = new ByteReader(value);
          const witnessCount = witnessReader.readVarInt();
          input.finalScriptWitness = [];
          for (let w = 0; w < witnessCount; w++) {
            const itemLen = witnessReader.readVarInt();
            input.finalScriptWitness.push(witnessReader.readBytes(itemLen));
          }
          input.hasAllSignatures = true;
          break;
        }
      }
    }

    // Check if has signatures (either finalized or partial)
    if (input.partialSigs.length > 0) {
      input.hasAllSignatures = true; // Simplified - real check would need to know required sigs
    }

    psbtInputs.push(input);
  }

  // Parse output sections
  const psbtOutputs: PSBTOutput[] = [];

  for (let i = 0; i < unsignedTx.outputs.length; i++) {
    const output: PSBTOutput = {
      index: i,
      bip32Derivation: [],
    };

    while (true) {
      const keyLen = reader.readVarInt();
      if (keyLen === 0) break;

      const key = reader.readBytes(keyLen);
      const keyType = parseInt(key.slice(0, 2), 16);
      const keyData = key.slice(2);

      const valueLen = reader.readVarInt();
      const value = reader.readBytes(valueLen);

      switch (keyType) {
        case PSBT_OUT_REDEEM_SCRIPT:
          output.redeemScript = value;
          break;
        case PSBT_OUT_WITNESS_SCRIPT:
          output.witnessScript = value;
          break;
        case PSBT_OUT_BIP32_DERIVATION:
          output.bip32Derivation.push({
            pubkey: keyData,
            masterFingerprint: value.slice(0, 8),
            path: parseBip32Path(value),
          });
          break;
      }
    }

    psbtOutputs.push(output);
  }

  // Calculate totals and fee
  const totalOutputValue = unsignedTx.totalOutputSats;
  let fee: number | undefined;
  let feeBtc: string | undefined;
  let feeRate: number | undefined;

  if (inputsWithValue === unsignedTx.inputs.length && totalInputValue > 0) {
    fee = totalInputValue - totalOutputValue;
    feeBtc = satsToBtc(fee);
    feeRate = Math.round(fee / unsignedTx.vsize * 100) / 100;

    if (fee < 0) {
      warnings.push('Negative fee! Outputs exceed inputs.');
    } else if (feeRate > 1000) {
      warnings.push(`Very high fee rate: ${feeRate} sat/vB`);
    } else if (feeRate < 1) {
      warnings.push(`Very low fee rate: ${feeRate} sat/vB - may not confirm`);
    } else if (feeRate < 5) {
      warnings.push(`Low fee rate: ${feeRate} sat/vB - may take a long time to confirm`);
    }
  }

  // Signing progress
  const signedInputs = psbtInputs.filter(i => i.hasAllSignatures).length;
  const signingProgress = `${signedInputs}/${unsignedTx.inputs.length} inputs signed`;
  const isFullySigned = signedInputs === unsignedTx.inputs.length;

  return {
    isPsbt: true,
    version: psbtVersion,
    unsignedTx,
    inputs: psbtInputs,
    outputs: psbtOutputs,
    globalXpubs,
    unknownGlobals,
    totalInputValue,
    totalInputBtc: satsToBtc(totalInputValue),
    totalOutputValue,
    totalOutputBtc: satsToBtc(totalOutputValue),
    fee,
    feeBtc,
    feeRate,
    isFullySigned,
    signingProgress,
    warnings,
  };
}

// ============================================================================
// Unified decode function - handles both raw TX and PSBT
// ============================================================================

export type DecodeResult =
  | { type: 'transaction'; data: DecodedTransaction }
  | { type: 'psbt'; data: DecodedPSBT };

/**
 * Sanitize input by removing all whitespace (spaces, tabs, newlines, etc.)
 * This allows users to paste hex with formatting
 */
export function sanitizeHexInput(input: string): string {
  return input.replace(/\s+/g, '');
}

export async function decodeAny(input: string): Promise<DecodeResult> {
  // Strip ALL whitespace to handle formatted/indented input
  const cleaned = sanitizeHexInput(input);

  if (isPSBT(cleaned)) {
    return { type: 'psbt', data: await decodePSBT(cleaned) };
  }

  return { type: 'transaction', data: await decodeTransaction(cleaned) };
}

// Helper to detect input type without full decode

export function detectInputType(input: string): 'transaction' | 'psbt' | 'invalid' {
  // Strip ALL whitespace (spaces, newlines, tabs, etc.)
  const cleaned = sanitizeHexInput(input);

  if (!cleaned) return 'invalid';

  // Check for PSBT (base64 or hex)
  if (isPSBT(cleaned)) return 'psbt';

  // Check for valid hex
  if (/^[0-9a-fA-F]+$/.test(cleaned) && cleaned.length >= 20) {
    return 'transaction';
  }

  return 'invalid';
}

// ============================================================================
// PSBT Finalization & Transaction Extraction
// ============================================================================

/**
 * Check if a PSBT can be finalized (all inputs have final scripts)
 */
export function canFinalizePSBT(psbt: DecodedPSBT): { canFinalize: boolean; reason?: string } {
  for (let i = 0; i < psbt.inputs.length; i++) {
    const input = psbt.inputs[i];

    // Check if input already has final scripts
    if (input.finalScriptSig || input.finalScriptWitness) {
      continue; // Already finalized
    }

    // Check if we have partial signatures
    if (input.partialSigs.length === 0) {
      return {
        canFinalize: false,
        reason: `Input #${i} has no signatures`
      };
    }

    // For now, we only support simple single-sig finalization
    // Multisig would need additional logic to determine if we have enough sigs
  }

  return { canFinalize: true };
}

/**
 * Helper to encode a varint
 */
function encodeVarInt(n: number): string {
  if (n < 0xfd) {
    return n.toString(16).padStart(2, '0');
  } else if (n <= 0xffff) {
    return 'fd' +
      (n & 0xff).toString(16).padStart(2, '0') +
      ((n >> 8) & 0xff).toString(16).padStart(2, '0');
  } else if (n <= 0xffffffff) {
    return 'fe' +
      (n & 0xff).toString(16).padStart(2, '0') +
      ((n >> 8) & 0xff).toString(16).padStart(2, '0') +
      ((n >> 16) & 0xff).toString(16).padStart(2, '0') +
      ((n >> 24) & 0xff).toString(16).padStart(2, '0');
  }
  throw new Error('VarInt too large');
}

/**
 * Extract raw transaction hex from a finalized PSBT
 *
 * This takes a PSBT where inputs have finalScriptSig and/or finalScriptWitness
 * and constructs the complete raw transaction that can be broadcast.
 */
export function extractTransactionFromPSBT(psbt: DecodedPSBT): {
  success: boolean;
  txHex?: string;
  error?: string
} {
  // First check if we can finalize
  const { canFinalize, reason } = canFinalizePSBT(psbt);
  if (!canFinalize) {
    return { success: false, error: reason || 'PSBT cannot be finalized' };
  }

  // Check if any input needs witness (determines if we use SegWit serialization)
  let needsWitness = false;
  for (const input of psbt.inputs) {
    if (input.finalScriptWitness && input.finalScriptWitness.length > 0) {
      needsWitness = true;
      break;
    }
    // Also check if witnessUtxo suggests SegWit
    if (input.witnessUtxo?.scriptType === 'p2wpkh' ||
        input.witnessUtxo?.scriptType === 'p2wsh' ||
        input.witnessUtxo?.scriptType === 'p2tr') {
      needsWitness = true;
      break;
    }
  }

  try {
    let txHex = '';

    // Version (4 bytes LE)
    const version = psbt.unsignedTx.version;
    txHex += (version & 0xff).toString(16).padStart(2, '0');
    txHex += ((version >> 8) & 0xff).toString(16).padStart(2, '0');
    txHex += ((version >> 16) & 0xff).toString(16).padStart(2, '0');
    txHex += ((version >> 24) & 0xff).toString(16).padStart(2, '0');

    // SegWit marker + flag
    if (needsWitness) {
      txHex += '00'; // marker
      txHex += '01'; // flag
    }

    // Input count
    txHex += encodeVarInt(psbt.unsignedTx.inputs.length);

    // Inputs
    for (let i = 0; i < psbt.unsignedTx.inputs.length; i++) {
      const txInput = psbt.unsignedTx.inputs[i];
      const psbtInput = psbt.inputs[i];

      // Previous output txid (32 bytes, reversed for internal representation)
      // The txid in DecodedInput is already in display format, we need to reverse it
      const txidBytes = txInput.txid.match(/.{2}/g)?.reverse().join('') || '';
      txHex += txidBytes;

      // Previous output index (4 bytes LE)
      const vout = txInput.vout;
      txHex += (vout & 0xff).toString(16).padStart(2, '0');
      txHex += ((vout >> 8) & 0xff).toString(16).padStart(2, '0');
      txHex += ((vout >> 16) & 0xff).toString(16).padStart(2, '0');
      txHex += ((vout >> 24) & 0xff).toString(16).padStart(2, '0');

      // ScriptSig
      let scriptSig = '';
      if (psbtInput.finalScriptSig) {
        scriptSig = psbtInput.finalScriptSig;
      } else if (!needsWitness && psbtInput.partialSigs.length > 0) {
        // For legacy P2PKH, construct scriptSig from signature + pubkey
        // <sig> <pubkey>
        const sig = psbtInput.partialSigs[0].signature;
        const pubkey = psbtInput.partialSigs[0].pubkey;
        const sigLen = sig.length / 2;
        const pubkeyLen = pubkey.length / 2;
        scriptSig = sigLen.toString(16).padStart(2, '0') + sig +
                    pubkeyLen.toString(16).padStart(2, '0') + pubkey;
      }

      txHex += encodeVarInt(scriptSig.length / 2);
      txHex += scriptSig;

      // Sequence (4 bytes LE)
      const seq = txInput.sequence;
      txHex += (seq & 0xff).toString(16).padStart(2, '0');
      txHex += ((seq >> 8) & 0xff).toString(16).padStart(2, '0');
      txHex += ((seq >> 16) & 0xff).toString(16).padStart(2, '0');
      txHex += ((seq >> 24) & 0xff).toString(16).padStart(2, '0');
    }

    // Output count
    txHex += encodeVarInt(psbt.unsignedTx.outputs.length);

    // Outputs
    for (const output of psbt.unsignedTx.outputs) {
      // Value (8 bytes LE)
      let value = output.value;
      for (let j = 0; j < 8; j++) {
        txHex += (value & 0xff).toString(16).padStart(2, '0');
        value = Math.floor(value / 256);
      }

      // ScriptPubKey
      txHex += encodeVarInt(output.scriptPubKey.length / 2);
      txHex += output.scriptPubKey;
    }

    // Witness data (if SegWit)
    if (needsWitness) {
      for (let i = 0; i < psbt.inputs.length; i++) {
        const psbtInput = psbt.inputs[i];

        if (psbtInput.finalScriptWitness && psbtInput.finalScriptWitness.length > 0) {
          // Use final witness directly
          txHex += encodeVarInt(psbtInput.finalScriptWitness.length);
          for (const item of psbtInput.finalScriptWitness) {
            txHex += encodeVarInt(item.length / 2);
            txHex += item;
          }
        } else if (psbtInput.partialSigs.length > 0) {
          // Construct witness from partial sigs (P2WPKH: <sig> <pubkey>)
          const sig = psbtInput.partialSigs[0].signature;
          const pubkey = psbtInput.partialSigs[0].pubkey;
          txHex += '02'; // 2 witness items
          txHex += encodeVarInt(sig.length / 2);
          txHex += sig;
          txHex += encodeVarInt(pubkey.length / 2);
          txHex += pubkey;
        } else {
          // Empty witness
          txHex += '00';
        }
      }
    }

    // Locktime (4 bytes LE)
    const locktime = psbt.unsignedTx.locktime;
    txHex += (locktime & 0xff).toString(16).padStart(2, '0');
    txHex += ((locktime >> 8) & 0xff).toString(16).padStart(2, '0');
    txHex += ((locktime >> 16) & 0xff).toString(16).padStart(2, '0');
    txHex += ((locktime >> 24) & 0xff).toString(16).padStart(2, '0');

    return { success: true, txHex };
  } catch (err) {
    return {
      success: false,
      error: err instanceof Error ? err.message : 'Failed to extract transaction'
    };
  }
}

// ============================================================================
// Helper Functions for Display
// ============================================================================

/**
 * Format sighash type for human-readable display
 */
export function formatSighashType(sighashType: number): string {
  const SIGHASH_ALL = 0x01;
  const SIGHASH_NONE = 0x02;
  const SIGHASH_SINGLE = 0x03;
  const SIGHASH_ANYONECANPAY = 0x80;

  const baseType = sighashType & 0x1f;
  const anyoneCanPay = (sighashType & SIGHASH_ANYONECANPAY) !== 0;

  let name: string;
  switch (baseType) {
    case SIGHASH_ALL:
      name = 'SIGHASH_ALL';
      break;
    case SIGHASH_NONE:
      name = 'SIGHASH_NONE';
      break;
    case SIGHASH_SINGLE:
      name = 'SIGHASH_SINGLE';
      break;
    default:
      name = `UNKNOWN(0x${baseType.toString(16)})`;
  }

  if (anyoneCanPay) {
    name += '|ANYONECANPAY';
  }

  return name;
}

// Re-export ByteReader for potential external use
export { ByteReader }
