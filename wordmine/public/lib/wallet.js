/* Glaciem in-browser wallet -- key generation & restore.
 *
 * SECURITY POSTURE
 * ----------------
 * This produces a real Glaciem keypair entirely client-side. Keys never leave
 * the browser, are never written to localStorage, never POSTed anywhere. The
 * page caller is responsible for showing the seed to the user (write down on
 * paper) and dropping it from memory once they confirm.
 *
 * Crypto sources:
 *  - ed25519 scalar mult: @noble/curves (audited, MIT, pinned to 1.6.0)
 *  - keccak-256:          @noble/hashes (audited, MIT, pinned to 1.5.0)
 *  - randomness:          crypto.getRandomValues  (CSPRNG, never Math.random)
 *
 * Algorithm follows Monero's wallet exactly so the resulting seed restores
 * 1-for-1 in any Monero-fork CLI/GUI wallet (Glaciem's rime-wallet-cli):
 *   1. spend_secret   = sc_reduce32(rand 32 bytes)        // mod ed25519 order l
 *   2. view_secret    = sc_reduce32(keccak256(spend_secret))
 *   3. pub_spend      = spend_secret · G
 *   4. pub_view       = view_secret  · G
 *   5. address        = base58( varint(144) || pub_spend || pub_view
 *                                || keccak256(prev)[0..4] )
 *   6. mnemonic       = 24 words (encoding spend_secret) + 1 checksum word
 */

// Bare specifiers; esbuild resolves these from wordmine/node_modules and
// emits a single self-contained ESM file at lib/wallet.bundle.js.
import { ed25519 } from '@noble/curves/ed25519';
import { keccak_256 } from '@noble/hashes/sha3';
import { WORDS_EN, PREFIX_LEN_EN } from './wordlist-en.js';

// ---- constants -------------------------------------------------------------

// ed25519 group order
const L = 2n ** 252n + 27742317777372353535851937790883648493n;

// Glaciem mainnet address prefixes (from src/cryptonote_config.h).
// Standard (144) is the only one this module generates -- subaddresses and
// integrated addresses are valid claim targets too, but we don't create them
// here.
const PREFIX_STANDARD = 144;

// Monero base58 alphabet (NOT the same as Bitcoin base58 -- different order).
const B58_ALPHA = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
// Number of base58 chars produced for the trailing N-byte block (0..8).
const B58_BLOCK_SIZES = [0, 2, 3, 5, 6, 7, 9, 10, 11];

// ---- low-level helpers -----------------------------------------------------

function bytesToBigIntLE(b) {
  let n = 0n;
  for (let i = b.length - 1; i >= 0; i--) n = (n << 8n) | BigInt(b[i]);
  return n;
}

function bigIntToBytesLE(n, len = 32) {
  const b = new Uint8Array(len);
  for (let i = 0; i < len; i++) { b[i] = Number(n & 0xffn); n >>= 8n; }
  return b;
}

function bytesToHex(b) {
  return Array.from(b, x => x.toString(16).padStart(2, '0')).join('');
}

function hexToBytes(h) {
  if (h.length % 2) throw new Error('hex length must be even');
  const b = new Uint8Array(h.length / 2);
  for (let i = 0; i < b.length; i++) b[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
  return b;
}

function concatBytes(...arrs) {
  const total = arrs.reduce((s, a) => s + a.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const a of arrs) { out.set(a, off); off += a.length; }
  return out;
}

// Reduce 32 bytes (little-endian) modulo the ed25519 group order.
// Equivalent to Monero's sc_reduce32.
function scReduce32(b) {
  if (b.length !== 32) throw new Error('sc_reduce32 expects 32 bytes');
  return bigIntToBytesLE(bytesToBigIntLE(b) % L, 32);
}

// ---- ed25519 scalar multiplication -----------------------------------------

// pub = scalar · BASE, returned as 32-byte little-endian point encoding.
function scalarMultBase(scalarBytes) {
  const s = bytesToBigIntLE(scalarBytes);
  if (s === 0n) throw new Error('zero scalar');
  const point = ed25519.ExtendedPoint.BASE.multiply(s);
  return point.toRawBytes();
}

// ---- monero-style varint ---------------------------------------------------

function encodeVarint(n) {
  const out = [];
  while (n >= 0x80) { out.push((n & 0x7f) | 0x80); n = n >>> 7; }
  out.push(n & 0x7f);
  return Uint8Array.from(out);
}

// ---- monero-style base58 (block encoding) ----------------------------------

function encodeBlock(bytes, chars) {
  let n = 0n;
  for (const b of bytes) n = n * 256n + BigInt(b);
  const out = new Array(chars).fill(B58_ALPHA[0]);
  for (let i = chars - 1; i >= 0 && n > 0n; i--) {
    out[i] = B58_ALPHA[Number(n % 58n)];
    n /= 58n;
  }
  return out.join('');
}

function b58Encode(data) {
  let s = '';
  const fullBlocks = Math.floor(data.length / 8);
  const tail = data.length - fullBlocks * 8;
  for (let i = 0; i < fullBlocks; i++) {
    s += encodeBlock(data.slice(i * 8, i * 8 + 8), 11);
  }
  if (tail > 0) {
    s += encodeBlock(data.slice(fullBlocks * 8), B58_BLOCK_SIZES[tail]);
  }
  return s;
}

// ---- address builder -------------------------------------------------------

function buildAddress(prefix, pubSpend, pubView) {
  const head = concatBytes(encodeVarint(prefix), pubSpend, pubView);
  const checksum = keccak_256(head).slice(0, 4);
  return b58Encode(concatBytes(head, checksum));
}

// ---- mnemonic --------------------------------------------------------------

// CRC-32 (IEEE 802.3 polynomial, same as Monero's mnemonic checksum).
const CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32String(str) {
  let crc = 0xffffffff;
  for (let i = 0; i < str.length; i++) {
    crc = (crc >>> 8) ^ CRC32_TABLE[(crc ^ str.charCodeAt(i)) & 0xff];
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// Convert 32 secret bytes to 25 words (24 words + 1 checksum word).
function bytesToMnemonic(b, words = WORDS_EN, prefixLen = PREFIX_LEN_EN) {
  if (b.length !== 32) throw new Error('mnemonic expects 32 bytes');
  const n = BigInt(words.length);
  const out = [];
  for (let i = 0; i < 32; i += 4) {
    // 32-bit little-endian value
    const x = BigInt(b[i]) | (BigInt(b[i + 1]) << 8n)
            | (BigInt(b[i + 2]) << 16n) | (BigInt(b[i + 3]) << 24n);
    const w1 = x % n;
    const w2 = ((x / n) + w1) % n;
    const w3 = ((x / n / n) + w2) % n;
    out.push(words[Number(w1)], words[Number(w2)], words[Number(w3)]);
  }
  // 25th word is the checksum
  const joined = out.map(w => w.slice(0, prefixLen)).join('');
  const checksumIdx = crc32String(joined) % out.length;
  out.push(out[checksumIdx]);
  return out;
}

// Convert 25-word mnemonic back to 32 secret bytes (verifies checksum).
function mnemonicToBytes(mnemonic, words = WORDS_EN, prefixLen = PREFIX_LEN_EN) {
  if (mnemonic.length !== 25) throw new Error('expected 25 words');
  const wordIndex = new Map(words.map((w, i) => [w, i]));
  for (const w of mnemonic) {
    if (!wordIndex.has(w)) throw new Error(`word not in dictionary: ${w}`);
  }
  // verify checksum
  const w24 = mnemonic.slice(0, 24);
  const joined = w24.map(w => w.slice(0, prefixLen)).join('');
  const expectedIdx = crc32String(joined) % w24.length;
  if (mnemonic[24] !== w24[expectedIdx]) throw new Error('seed checksum failed');

  const n = BigInt(words.length);
  const b = new Uint8Array(32);
  for (let i = 0; i < 24; i += 3) {
    const w1 = BigInt(wordIndex.get(w24[i]));
    const w2 = BigInt(wordIndex.get(w24[i + 1]));
    const w3 = BigInt(wordIndex.get(w24[i + 2]));
    // x = w1 + n * ((n - w1 + w2) mod n) + n*n * ((n - w2 + w3) mod n)
    const x = w1
            + n * (((n - w1) + w2) % n)
            + n * n * (((n - w2) + w3) % n);
    const off = (i / 3) * 4;
    b[off]     = Number((x      ) & 0xffn);
    b[off + 1] = Number((x >>  8n) & 0xffn);
    b[off + 2] = Number((x >> 16n) & 0xffn);
    b[off + 3] = Number((x >> 24n) & 0xffn);
  }
  return b;
}

// ---- public API ------------------------------------------------------------

/** Generate a fresh Glaciem wallet. Returns { address, mnemonic, spendKeyHex, viewKeyHex }. */
export function generateWallet() {
  const r = new Uint8Array(32);
  crypto.getRandomValues(r);
  const spendSecret = scReduce32(r);
  return walletFromSpendSecret(spendSecret);
}

/** Restore from a 25-word seed. Throws if the seed is invalid. */
export function restoreFromMnemonic(words) {
  const spendSecret = mnemonicToBytes(words);
  return walletFromSpendSecret(spendSecret);
}

function walletFromSpendSecret(spendSecret) {
  const viewSecret = scReduce32(keccak_256(spendSecret));
  const pubSpend = scalarMultBase(spendSecret);
  const pubView  = scalarMultBase(viewSecret);
  const address  = buildAddress(PREFIX_STANDARD, pubSpend, pubView);
  const mnemonic = bytesToMnemonic(spendSecret);
  return {
    address,
    mnemonic,                                 // array of 25 strings
    spendKeyHex: bytesToHex(spendSecret),     // diagnostic only -- don't show users
    viewKeyHex:  bytesToHex(viewSecret),      // (the seed encodes the same info)
  };
}

// Exposed for unit tests / sanity checks; not used by the UI directly.
export const _internals = {
  scReduce32, encodeVarint, b58Encode, buildAddress,
  bytesToMnemonic, mnemonicToBytes, crc32String,
  bytesToHex, hexToBytes,
};
