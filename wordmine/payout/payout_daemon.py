#!/usr/bin/env python3
"""
Wordmine payout daemon -- runs on VM1, alongside the faucet rime-wallet-rpc.

Two responsibilities:

  1. Every 30 seconds, ask rime-wallet-rpc for the faucet balance and write
     it into Cloudflare KV at key `faucet:balance`. The Worker reads from
     there to show the user the current balance and to compute payout sizes.

  2. Every ~10 seconds, list all `queue:*` keys in KV; for each, fetch the
     corresponding `claim:<id>` record, send the GLAC via wallet-rpc
     `transfer`, then write the result (status / tx_hash / error) back to
     `claim:<id>` and delete the queue index. The frontend polls
     `/api/claim_status?id=...` to surface the tx hash to the user.

Configuration via env vars:

  CF_ACCOUNT_ID     Cloudflare account id (32-char hex)
  CF_KV_NAMESPACE   the WORDMINE namespace id
  CF_API_TOKEN      Cloudflare API token with "Workers KV Storage: Edit"
  WALLET_RPC_URL    e.g. http://127.0.0.1:28083/json_rpc
  WALLET_PASSWORD   wallet password (empty if none; we created with "")
  MIN_PAYOUT_ATOMIC minimum atomic units to pay out (default 1000 = 1e-9 GLAC)
"""

import json
import os
import sys
import time
import urllib.request
import urllib.error
import urllib.parse

# ---- config ---------------------------------------------------------------

CF_ACCOUNT_ID   = os.environ.get('CF_ACCOUNT_ID', '')
CF_KV_NAMESPACE = os.environ.get('CF_KV_NAMESPACE', '')
CF_API_TOKEN    = os.environ.get('CF_API_TOKEN', '')
WALLET_RPC_URL  = os.environ.get('WALLET_RPC_URL', 'http://127.0.0.1:28083/json_rpc')
WALLET_PASSWORD = os.environ.get('WALLET_PASSWORD', '')
MIN_PAYOUT      = int(os.environ.get('MIN_PAYOUT_ATOMIC', '1000'))

CF_API = 'https://api.cloudflare.com/client/v4'

# ---- HTTP helpers ---------------------------------------------------------

def http(method, url, body=None, headers=None, timeout=10):
    data = None
    h = dict(headers or {})
    if body is not None:
        if isinstance(body, (dict, list)):
            data = json.dumps(body).encode('utf-8')
            h.setdefault('Content-Type', 'application/json')
        else:
            data = body.encode('utf-8') if isinstance(body, str) else body
    req = urllib.request.Request(url, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode('utf-8', errors='replace')
            return r.status, raw
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', errors='replace')
    except Exception as e:
        return 0, str(e)

def kv_url(key):
    return (f'{CF_API}/accounts/{CF_ACCOUNT_ID}'
            f'/storage/kv/namespaces/{CF_KV_NAMESPACE}/values/'
            f'{urllib.parse.quote(key, safe="")}')

def kv_get(key):
    s, body = http('GET', kv_url(key),
                   headers={'Authorization': 'Bearer ' + CF_API_TOKEN})
    if s == 404: return None
    if s != 200:
        print(f'[kv_get] {key} -> {s} {body[:200]}', flush=True)
        return None
    try: return json.loads(body)
    except: return body

def kv_put(key, value, ttl=None):
    url = kv_url(key)
    if ttl: url += '?expiration_ttl=' + str(int(ttl))
    body = value if isinstance(value, str) else json.dumps(value)
    s, b = http('PUT', url,
                body=body,
                headers={'Authorization': 'Bearer ' + CF_API_TOKEN,
                         'Content-Type': 'application/json'})
    if s not in (200,):
        print(f'[kv_put] {key} -> {s} {b[:200]}', flush=True)

def kv_delete(key):
    s, b = http('DELETE', kv_url(key),
                headers={'Authorization': 'Bearer ' + CF_API_TOKEN})
    if s not in (200, 404):
        print(f'[kv_delete] {key} -> {s} {b[:200]}', flush=True)

def kv_list(prefix, limit=50):
    url = (f'{CF_API}/accounts/{CF_ACCOUNT_ID}'
           f'/storage/kv/namespaces/{CF_KV_NAMESPACE}/keys'
           f'?prefix={urllib.parse.quote(prefix, safe="")}&limit={limit}')
    s, b = http('GET', url,
                headers={'Authorization': 'Bearer ' + CF_API_TOKEN})
    if s != 200:
        print(f'[kv_list] {prefix} -> {s} {b[:200]}', flush=True)
        return []
    try: return [k['name'] for k in json.loads(b).get('result', [])]
    except: return []

# ---- wallet-rpc -----------------------------------------------------------

def wallet(method, params=None):
    s, b = http('POST', WALLET_RPC_URL, body={
        'jsonrpc': '2.0', 'id': '0', 'method': method, 'params': params or {},
    }, timeout=15)
    if s != 200:
        return None, f'http {s}: {b[:200]}'
    try:
        j = json.loads(b)
    except Exception as e:
        return None, f'bad json: {e}'
    if 'error' in j:
        return None, j['error'].get('message', 'rpc error')
    return j.get('result', {}), None

# ---- daemon loop ----------------------------------------------------------

def update_balance():
    res, err = wallet('get_balance', {'account_index': 0})
    if err:
        print(f'[balance] wallet rpc error: {err}', flush=True)
        return
    atomic = res.get('unlocked_balance', 0)
    bal = atomic / 1e12
    kv_put('faucet:balance', {'balance': bal, 'at': int(time.time() * 1000)},
           ttl=600)

def process_claims():
    keys = kv_list('queue:', limit=20)
    if not keys: return
    for qkey in keys:
        # qkey is "queue:<timestamp>:<claim_id>"; resolve to claim record
        claim_id = qkey.rsplit(':', 1)[-1]
        rec = kv_get('claim:' + claim_id)
        if not rec:
            kv_delete(qkey)
            continue
        if rec.get('status') != 'pending':
            kv_delete(qkey)
            continue
        amount = int(rec.get('amount_atomic', 0))
        if amount < MIN_PAYOUT:
            rec['status'] = 'failed'
            rec['error']  = 'below minimum payout'
            kv_put('claim:' + claim_id, rec, ttl=86400)
            kv_delete(qkey)
            continue
        address = rec.get('address', '')
        print(f'[pay] {claim_id} -> {amount} -> {address[:14]}…', flush=True)
        res, err = wallet('transfer', {
            'destinations': [{'amount': amount, 'address': address}],
            'account_index': 0,
            'priority': 0,
            'get_tx_key': True,
        })
        if err:
            rec['status'] = 'failed'
            rec['error']  = err
            kv_put('claim:' + claim_id, rec, ttl=86400)
            kv_delete(qkey)
            print(f'[pay] FAIL {claim_id}: {err}', flush=True)
            continue
        rec['status']  = 'paid'
        rec['tx_hash'] = res.get('tx_hash', '')
        rec['fee']     = res.get('fee', 0)
        kv_put('claim:' + claim_id, rec, ttl=86400)
        kv_delete(qkey)
        print(f'[pay] OK {claim_id} tx={rec["tx_hash"][:12]}', flush=True)

def main():
    if not (CF_ACCOUNT_ID and CF_KV_NAMESPACE and CF_API_TOKEN):
        print('error: set CF_ACCOUNT_ID, CF_KV_NAMESPACE, CF_API_TOKEN', file=sys.stderr)
        sys.exit(1)

    # open the wallet (no-op if already open)
    res, err = wallet('open_wallet', {'filename': 'wordmine', 'password': WALLET_PASSWORD})
    if err and 'already' not in err.lower():
        print(f'open_wallet warning: {err}', flush=True)

    last_balance = 0
    while True:
        try:
            now = time.time()
            if now - last_balance > 30:
                update_balance()
                last_balance = now
            process_claims()
        except KeyboardInterrupt:
            print('exiting', flush=True)
            return
        except Exception as e:
            print(f'loop error: {e}', flush=True)
        time.sleep(8)

if __name__ == '__main__':
    main()
