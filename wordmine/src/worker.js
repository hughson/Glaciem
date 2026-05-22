/* Wordmine -- Cloudflare Worker.
 *
 * Single-file game backend + claim queue. All authority is server-side:
 *   - the word is picked here, never sent to the client before the game ends
 *   - guesses are graded here
 *   - the rate limit and per-guess reward are computed here
 *   - claim eligibility lives in KV with TTL
 *   - payouts are written to a KV queue that a VPS-side daemon drains
 *
 * KV namespace layout:
 *   session:<id>         {word, started_at, guess_count, won, lost, claimed, reward}
 *                        TTL 30 min
 *   ratelimit:<ip>       'taken'   TTL 1 hour
 *   claim:<id>           {address, amount, status, tx_hash, error, created_at}
 *                        TTL 24h after final state
 *   faucet:balance       cached balance number   TTL 30s
 *
 * Static assets (HTML/JS/CSS) come from the `[assets]` binding (public/).
 */

import { WORDS } from './words.js';

// ---- helpers ---------------------------------------------------------------

const json = (body, status = 200, headers = {}) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { 'content-type': 'application/json', ...headers },
  });

const err = (msg, status = 400) => json({ error: msg }, status);

function newId(prefix) {
  const a = new Uint8Array(16);
  crypto.getRandomValues(a);
  return prefix + Array.from(a, (b) => b.toString(16).padStart(2, '0')).join('');
}

function clientIp(req) {
  return req.headers.get('cf-connecting-ip') ||
         req.headers.get('x-forwarded-for') ||
         'unknown';
}

// Grade a guess against the answer. Returns ['green'|'yellow'|'gray', ...].
// Standard Wordle scoring: green for correct position, yellow for correct
// letter wrong position (only marks each answer-letter once), gray otherwise.
function grade(guess, answer) {
  const marks = ['gray', 'gray', 'gray', 'gray', 'gray'];
  const counts = {};
  // first pass: greens, count remaining answer letters
  for (let i = 0; i < 5; i++) {
    if (guess[i] === answer[i]) marks[i] = 'green';
    else counts[answer[i]] = (counts[answer[i]] || 0) + 1;
  }
  // second pass: yellows (consuming counts)
  for (let i = 0; i < 5; i++) {
    if (marks[i] === 'green') continue;
    if (counts[guess[i]]) { marks[i] = 'yellow'; counts[guess[i]]--; }
  }
  return marks;
}

// Compute the dynamic max reward from current faucet balance.
function maxReward(balance) {
  return Math.min(1.0, Math.max(0, balance) / 1000);
}

// Verify Cloudflare Turnstile token.
//
// Behaviour by env:
//   secret == "TEST"   -> bypass with a LOUD warning in the log. Use this
//                         only for first-deploy iteration.
//   secret missing     -> fail closed (refuse to bypass on a misconfigured
//                         secret -- that's how bots drain faucets).
//   real secret        -> POST to challenges.cloudflare.com/siteverify
async function verifyTurnstile(token, secret, ip) {
  if (secret === 'TEST') {
    console.warn('[wordmine] TURNSTILE_SECRET="TEST" -- captcha is BYPASSED. '
               + 'Set a real secret via `wrangler secret put TURNSTILE_SECRET` '
               + 'before publishing the faucet.');
    return true;
  }
  if (!secret) {
    console.warn('[wordmine] TURNSTILE_SECRET is not set -- failing closed.');
    return false;
  }
  if (!token) return false;
  const form = new FormData();
  form.append('secret', secret);
  form.append('response', token);
  form.append('remoteip', ip);
  const r = await fetch(
    'https://challenges.cloudflare.com/turnstile/v0/siteverify',
    { method: 'POST', body: form });
  const j = await r.json();
  return !!j.success;
}

// ---- faucet balance helper -------------------------------------------------

async function getFaucetBalance(env) {
  // The VPS payout daemon writes faucet:balance to KV every ~30s. The
  // Worker only reads -- it never calls wallet-rpc directly, which keeps
  // the wallet RPC private on localhost on the VPS.
  //
  // Returns { balance, unlocked }:
  //   balance  = TOTAL faucet wallet balance (for display). Doesn't
  //              flash to 0 after a payout while change is locked.
  //   unlocked = currently SPENDABLE balance. Used to cap max_reward
  //              so we never promise a payout we can't actually transfer
  //              this instant.
  const rec = await env.WORDMINE.get('faucet:balance', { type: 'json' });
  if (!rec) return { balance: 0, unlocked: 0 };
  // If we haven't heard from the daemon in 10 min, treat as 0 (don't pay
  // out against a stale balance).
  if (Date.now() - rec.at > 600000) return { balance: 0, unlocked: 0 };
  // Backward-compat: pre-update daemon pushed only `balance` (which was
  // really the unlocked balance). Fall back to that if `unlocked` missing.
  const balance  = rec.balance ?? 0;
  const unlocked = rec.unlocked !== undefined ? rec.unlocked : balance;
  return { balance, unlocked };
}

// ---- routes ----------------------------------------------------------------

async function handleStart(req, env) {
  const ip = clientIp(req);
  // rate limit: store the start-timestamp so we can return the actual
  // remaining time rather than always claiming a full hour.
  const rlKey = 'ratelimit:' + ip;
  // TESTING BYPASS: setting env.DISABLE_RATELIMIT=1 in wrangler.toml lets
  // a single IP play unlimited games -- only useful while iterating on
  // the UI. Off by default; make sure it stays off in production.
  if (env.DISABLE_RATELIMIT === '1') {
    console.warn('[wordmine] DISABLE_RATELIMIT=1 -- per-IP cooldown is OFF');
  }
  const taken = env.DISABLE_RATELIMIT === '1' ? null : await env.WORDMINE.get(rlKey);
  if (taken) {
    // parseInt; legacy entries from before this fix stored the string
    // 'taken' instead of a timestamp -- treat those as expired.
    const startedAt = parseInt(taken, 10);
    if (!isNaN(startedAt)) {
      const remaining = Math.max(0, 3600 - Math.floor((Date.now() - startedAt) / 1000));
      if (remaining > 0) {
        return json({
          error: 'cooldown',
          retry_after_seconds: remaining,
        }, 429);
      }
    }
    // Fall through: legacy value or remaining <= 0 -> let them play.
  }
  // captcha
  const body = await req.json().catch(() => ({}));
  const ok = await verifyTurnstile(body.captcha, env.TURNSTILE_SECRET, ip);
  if (!ok) return err('captcha_failed', 403);

  // Pick a word + create session
  const word = WORDS[Math.floor(Math.random() * WORDS.length)];
  const id = newId('s_');
  const session = {
    word,
    started_at: Date.now(),
    guess_count: 0,
    won: false,
    lost: false,
    claimed: false,
    reward: 0,
  };
  await env.WORDMINE.put('session:' + id, JSON.stringify(session),
    { expirationTtl: 1800 });
  // Start the rate-limit TTL only on /start, so failed games still cost the slot.
  // Value stores the start timestamp so the cooldown page can show real remaining time.
  if (env.DISABLE_RATELIMIT !== '1') {
    await env.WORDMINE.put(rlKey, String(Date.now()), { expirationTtl: 3600 });
  }

  return json({ session: id });
}

async function handleGuess(req, env) {
  const body = await req.json().catch(() => ({}));
  const id = body.session;
  const guess = String(body.guess || '').toUpperCase();
  if (!/^[A-Z]{5}$/.test(guess)) return err('bad_guess');
  if (!id) return err('no_session', 400);

  const rec = await env.WORDMINE.get('session:' + id, { type: 'json' });
  if (!rec) return err('no_session', 410);
  if (rec.won || rec.lost) return err('already_finished', 410);
  // Note: we don't gate guesses against the curated WORDS list -- that list
  // is the solution pool. Real Wordle uses a separate (much larger) guess
  // dictionary; rather than ship 13k words, we accept any 5-letter A-Z
  // input. A solve still requires landing on a word that IS in WORDS, so
  // the game is well-defined.

  rec.guess_count += 1;
  const marks = grade(guess, rec.word);
  const won = marks.every((m) => m === 'green');
  const lost = !won && rec.guess_count >= 6;

  rec.won = won;
  rec.lost = lost;

  let reward = 0;
  if (won) {
    // Reward sizing is gated by UNLOCKED balance, not total: even if the
    // wallet shows 1100 GLAC total, if it's all locked behind a 10-block
    // confirmation wait, we'd promise a payout that the daemon can't yet
    // transfer. Use unlocked to be honest with the player.
    const { unlocked } = await getFaucetBalance(env);
    reward = maxReward(unlocked) / rec.guess_count;
    rec.reward = reward;
  }
  // TTL by state:
  //   won  -> 15 min, the /claim eligibility window
  //   lost -> 5 min, just enough to view the "you lost" screen
  //   in-progress -> 30 min, matches the original /start TTL so a slow
  //                  player isn't kicked between guesses (bug fix:
  //                  previously this was 60 seconds, which expired the
  //                  session if you thought for more than a minute).
  const ttl = won ? 900 : (lost ? 300 : 1800);
  await env.WORDMINE.put('session:' + id, JSON.stringify(rec),
    { expirationTtl: ttl });

  const out = { marks, guesses: rec.guess_count, won, lost };
  if (won) {
    out.reward = reward;
    out.claim_token = id;          // session id IS the claim token (server-validated)
  } else if (lost) {
    out.answer = rec.word;
  }
  return json(out);
}

async function handleClaim(req, env) {
  const body = await req.json().catch(() => ({}));
  const id = body.token;
  const address = String(body.address || '').trim();
  if (!id) return err('no_token');
  // Glaciem mainnet addresses are 97 chars (R + 96): the 2-byte varint
  // prefix (144 / 272 / 400) adds one byte over Monero's 1-byte prefix,
  // which base58-encodes to two extra chars (95 -> 97).
  if (!/^R[1-9A-HJ-NP-Za-km-z]{96}$/.test(address))
    return err('bad_address');

  const rec = await env.WORDMINE.get('session:' + id, { type: 'json' });
  if (!rec) return err('expired_or_unknown', 410);
  if (!rec.won) return err('not_won', 400);
  if (rec.claimed) return err('already_claimed', 409);

  // mark claimed
  rec.claimed = true;
  await env.WORDMINE.put('session:' + id, JSON.stringify(rec),
    { expirationTtl: 900 });

  // enqueue payout for the VPS daemon
  const claimId = newId('c_');
  const claim = {
    address,
    amount: rec.reward,
    amount_atomic: Math.floor(rec.reward * 1e12),
    status: 'pending',
    created_at: Date.now(),
  };
  await env.WORDMINE.put('claim:' + claimId, JSON.stringify(claim),
    { expirationTtl: 86400 });
  // Index for the payout daemon to find pending claims efficiently
  await env.WORDMINE.put('queue:' + Date.now() + ':' + claimId, claimId,
    { expirationTtl: 86400 });

  return json({ claim_id: claimId });
}

async function handleClaimStatus(req, env) {
  const url = new URL(req.url);
  const id = url.searchParams.get('id');
  if (!id) return err('no_id');
  const rec = await env.WORDMINE.get('claim:' + id, { type: 'json' });
  if (!rec) return err('unknown_claim', 404);
  return json(rec);
}

async function handleFaucet(req, env) {
  const { balance, unlocked } = await getFaucetBalance(env);
  return json({
    balance,                          // total -- for headline display
    unlocked,                         // currently spendable (for tooltip)
    address: env.FAUCET_ADDRESS || '',
    max_reward: maxReward(unlocked),  // honest about what we can pay now
  });
}

// ---- security headers ------------------------------------------------------

// Content-Security-Policy that allows exactly what the page actually needs:
//   - Cloudflare Turnstile JS + iframe at challenges.cloudflare.com
//   - everything else: same-origin only (noble crypto is now vendored
//     into lib/wallet.bundle.js, so no third-party CDN at runtime)
//   - inline styles (we ship one <style> block + a few style="" attributes)
const CSP = [
  "default-src 'self'",
  "script-src 'self' https://challenges.cloudflare.com",
  "script-src-elem 'self' https://challenges.cloudflare.com",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data:",
  "frame-src https://challenges.cloudflare.com",
  "connect-src 'self' https://challenges.cloudflare.com",
  "font-src 'self'",
  "base-uri 'self'",
  "form-action 'self'",
  "frame-ancestors 'none'",
].join('; ');

const SECURITY_HEADERS = {
  'content-security-policy':   CSP,
  'x-content-type-options':    'nosniff',
  'x-frame-options':           'DENY',
  'referrer-policy':           'strict-origin-when-cross-origin',
  'strict-transport-security': 'max-age=63072000; includeSubDomains; preload',
  'permissions-policy':        'accelerometer=(), camera=(), geolocation=(), gyroscope=(), microphone=(), payment=(), usb=()',
};

function withSecurityHeaders(resp) {
  const h = new Headers(resp.headers);
  for (const [k, v] of Object.entries(SECURITY_HEADERS)) h.set(k, v);
  return new Response(resp.body, { status: resp.status, headers: h });
}

// ---- dispatcher ------------------------------------------------------------

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    let resp;
    if (url.pathname === '/api/start' && req.method === 'POST')
      resp = await handleStart(req, env);
    else if (url.pathname === '/api/guess' && req.method === 'POST')
      resp = await handleGuess(req, env);
    else if (url.pathname === '/api/claim' && req.method === 'POST')
      resp = await handleClaim(req, env);
    else if (url.pathname === '/api/claim_status' && req.method === 'GET')
      resp = await handleClaimStatus(req, env);
    else if (url.pathname === '/api/faucet' && req.method === 'GET')
      resp = await handleFaucet(req, env);
    else
      resp = await env.ASSETS.fetch(req);
    return withSecurityHeaders(resp);
  },
};
