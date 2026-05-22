/* Wordmine -- frontend game logic.
 *
 * Talks to the /api/* endpoints on the same Worker. All authority lives on
 * the server: the word is server-side, the grading is server-side, the
 * timer/rate-limit is server-side. The browser is a dumb renderer + input
 * device, by design (so users can't cheat by reading source). */

(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const els = {
    start:        $('start'),
    game:         $('game'),
    board:        $('board'),
    kb:           $('kb'),
    win:          $('win'),
    loss:         $('loss'),
    cooldown:     $('cooldown'),
    donate:       $('donate-card'),
    captchaWrap:  $('captcha-wrap'),
    playBtn:      $('play-btn'),
    faucetBal:    $('faucet-balance'),
    faucetAddr:   $('faucet-address'),
    maxReward:    $('max-reward'),
    minReward:    $('min-reward'),
    winGuesses:   $('win-guesses'),
    winReward:    $('win-reward'),
    claimAddress: $('claim-address'),
    claimBtn:     $('claim-btn'),
    claimStatus:  $('claim-status'),
    lossWord:     $('loss-word'),
    cooldownTime: $('cooldown-time'),
    copyAddress:  $('copy-address'),
    toast:        $('toast'),
    // wallet UI
    walletPanel:    $('wallet'),
    walletEmpty:    $('wallet-empty'),
    walletSaved:    $('wallet-saved'),
    walletSavedAddr:$('wallet-saved-addr'),
    walletChangeBtn:$('wallet-change-btn'),
    genWalletBtn:   $('gen-wallet-btn'),
    restoreWalletBtn:$('restore-wallet-btn'),
    walletShow:     $('wallet-show'),
    walletShowAddr: $('wallet-show-addr'),
    walletShowSeed: $('wallet-show-seed'),
    seedCopyBtn:    $('seed-copy-btn'),
    seedConfirmCb:  $('seed-confirm-cb'),
    seedContinue:   $('seed-continue'),
    walletRestore:  $('wallet-restore'),
    restoreInput:   $('restore-input'),
    restoreErr:     $('restore-err'),
    restoreGoBtn:   $('restore-go-btn'),
    restoreCancelBtn:$('restore-cancel-btn'),
  };

  let sessionId   = null;
  let currentRow  = 0;
  let currentCol  = 0;
  let guesses     = [];           // [{word, marks}]
  let kbState     = {};           // letter -> 'green' | 'yellow' | 'gray'
  let captchaToken = null;
  let gameOver    = false;

  // ----- helpers ----------------------------------------------------------

  function toast(msg, ms = 1800) {
    els.toast.textContent = msg;
    els.toast.classList.add('show');
    clearTimeout(toast._t);
    toast._t = setTimeout(() => els.toast.classList.remove('show'), ms);
  }

  function api(path, body) {
    return fetch('/api' + path, {
      method: body ? 'POST' : 'GET',
      headers: body ? { 'Content-Type': 'application/json' } : {},
      body: body ? JSON.stringify(body) : undefined,
    }).then((r) => r.json().then((j) => ({ status: r.status, body: j })));
  }

  function formatGlac(n) {
    if (n == null || isNaN(n)) return '— GLAC';
    if (n === 0) return '0 GLAC';
    return n.toFixed(n < 0.01 ? 6 : (n < 1 ? 4 : 3)) + ' GLAC';
  }

  // ----- board ------------------------------------------------------------

  function buildBoard() {
    els.board.innerHTML = '';
    for (let r = 0; r < 6; r++) {
      const row = document.createElement('div');
      row.className = 'row';
      for (let c = 0; c < 5; c++) {
        const cell = document.createElement('div');
        cell.className = 'cell';
        cell.id = `c${r}${c}`;
        row.appendChild(cell);
      }
      els.board.appendChild(row);
    }
  }

  function setCell(r, c, ch, cls) {
    const el = $(`c${r}${c}`);
    if (!el) return;
    el.textContent = ch || '';
    el.className = 'cell' + (cls ? ' ' + cls : '');
  }

  function flipRow(r, marks, letters) {
    for (let c = 0; c < 5; c++) {
      setTimeout(() => {
        const el = $(`c${r}${c}`);
        el.classList.add('flip');
        setTimeout(() => {
          el.className = 'cell ' + marks[c];
          el.textContent = letters[c];
        }, 175);
      }, c * 140);
    }
  }

  function shakeRow(r) {
    for (let c = 0; c < 5; c++) {
      const el = $(`c${r}${c}`);
      el.classList.add('shake');
      setTimeout(() => el.classList.remove('shake'), 500);
    }
  }

  // ----- keyboard ---------------------------------------------------------

  const KEYS = [
    ['Q','W','E','R','T','Y','U','I','O','P'],
    ['A','S','D','F','G','H','J','K','L'],
    ['ENT','Z','X','C','V','B','N','M','BACK'],
  ];

  function buildKeyboard() {
    els.kb.innerHTML = '';
    KEYS.forEach((tokens) => {
      const row = document.createElement('div');
      row.className = 'kbrow';
      tokens.forEach((t) => {
        const key = document.createElement('div');
        const isWide = t === 'ENT' || t === 'BACK';
        key.className = 'key' + (isWide ? ' wide' : '');
        key.dataset.k = t;
        key.textContent = t === 'BACK' ? '⌫' : (t === 'ENT' ? 'ENTER' : t);
        key.addEventListener('click', () => pressKey(t));
        row.appendChild(key);
      });
      els.kb.appendChild(row);
    });
  }

  function paintKeyboard() {
    document.querySelectorAll('.key').forEach((k) => {
      const t = k.dataset.k;
      if (t === 'ENT' || t === 'BACK') return;
      k.classList.remove('green', 'yellow', 'gray');
      if (kbState[t]) k.classList.add(kbState[t]);
    });
  }

  function pressKey(t) {
    if (gameOver) return;
    if (t === 'ENT') return submitRow();
    if (t === 'BACK') return backspace();
    if (currentCol < 5 && /^[A-Z]$/.test(t)) {
      setCell(currentRow, currentCol, t, 'typing');
      currentCol++;
    }
  }

  function backspace() {
    if (currentCol === 0) return;
    currentCol--;
    setCell(currentRow, currentCol, '', '');
  }

  function getCurrentGuess() {
    let w = '';
    for (let c = 0; c < 5; c++) w += ($(`c${currentRow}${c}`).textContent || '');
    return w;
  }

  async function submitRow() {
    if (currentCol < 5) { shakeRow(currentRow); toast('Need 5 letters'); return; }
    const guess = getCurrentGuess();
    const res = await api('/guess', { session: sessionId, guess });
    if (res.status !== 200) {
      const err = (res.body && res.body.error) || 'guess rejected';
      if (err === 'not_in_wordlist') { shakeRow(currentRow); toast('Not in word list'); return; }
      if (err === 'no_session')      { toast('Session expired — refreshing…'); setTimeout(()=>location.reload(), 1500); return; }
      shakeRow(currentRow); toast(err); return;
    }
    const marks = res.body.marks;      // ['green'|'yellow'|'gray', ...]
    const letters = guess.split('');
    flipRow(currentRow, marks, letters);

    // update keyboard state (only upgrade, never downgrade)
    const order = { gray: 0, yellow: 1, green: 2 };
    for (let c = 0; c < 5; c++) {
      const L = letters[c];
      const cur = kbState[L];
      if (!cur || order[marks[c]] > order[cur]) kbState[L] = marks[c];
    }
    setTimeout(paintKeyboard, 5 * 140 + 200);

    guesses.push({ word: guess, marks });
    currentRow++;
    currentCol = 0;

    if (res.body.won) {
      gameOver = true;
      setTimeout(() => showWin(res.body), 5 * 140 + 400);
    } else if (res.body.lost) {
      gameOver = true;
      setTimeout(() => showLoss(res.body.answer), 5 * 140 + 400);
    }
  }

  // ----- physical keyboard input ------------------------------------------

  document.addEventListener('keydown', (e) => {
    if (els.game.hidden) return;
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    if (e.key === 'Enter')      { e.preventDefault(); pressKey('ENT'); }
    else if (e.key === 'Backspace') { e.preventDefault(); pressKey('BACK'); }
    else if (/^[a-zA-Z]$/.test(e.key)) { pressKey(e.key.toUpperCase()); }
  });

  // ----- start screen -----------------------------------------------------

  function showStart() {
    els.start.hidden = false;
    els.game.hidden = true;
    els.win.hidden = true;
    els.loss.hidden = true;
    els.cooldown.hidden = true;
    if (els.walletPanel) els.walletPanel.hidden = false;
    if (els.walletShow)  els.walletShow.hidden = true;
    if (els.walletRestore) els.walletRestore.hidden = true;
  }
  function showGame() {
    els.start.hidden = true;
    els.game.hidden = false;
    if (els.walletPanel) els.walletPanel.hidden = true;
  }
  function showWin(data) {
    els.game.hidden = true;
    els.win.hidden = false;
    els.winGuesses.textContent = data.guesses + (data.guesses === 1 ? ' GUESS' : ' GUESSES');
    els.winReward.textContent = formatGlac(data.reward);
    // pre-fill address if remembered
    const saved = localStorage.getItem('wordmine.address') || '';
    if (saved) els.claimAddress.value = saved;
    // store claim token for /claim
    els.claimBtn.dataset.token = data.claim_token;
    els.claimBtn.dataset.reward = data.reward;
  }
  function showLoss(answer) {
    els.game.hidden = true;
    els.loss.hidden = false;
    els.lossWord.textContent = answer;
  }
  function showCooldown(secsLeft) {
    els.start.hidden = true;
    els.cooldown.hidden = false;
    const upd = () => {
      const s = Math.max(0, Math.floor(secsLeft));
      const mm = Math.floor(s / 60), ss = s % 60;
      els.cooldownTime.textContent = `${mm}m ${ss}s`;
      secsLeft -= 1;
      if (secsLeft >= 0) setTimeout(upd, 1000);
      else setTimeout(() => location.reload(), 1000);
    };
    upd();
  }

  // ----- start a game -----------------------------------------------------

  async function startGame() {
    els.playBtn.disabled = true;
    els.playBtn.textContent = 'STARTING…';
    const res = await api('/start', { captcha: captchaToken });
    if (res.status === 429) {
      // ?? not || -- 0 is a valid value meaning "cooldown already over"
      const secs = res.body.retry_after_seconds;
      showCooldown(typeof secs === 'number' ? secs : 3600);
      return;
    }
    if (res.status !== 200) {
      toast(res.body.error || 'failed to start');
      els.playBtn.disabled = false;
      els.playBtn.textContent = 'PLAY';
      return;
    }
    sessionId = res.body.session;
    showGame();
  }

  // ----- claim payout -----------------------------------------------------

  async function submitClaim() {
    const addr = els.claimAddress.value.trim();
    if (!/^R[1-9A-HJ-NP-Za-km-z]{96}$/.test(addr)) {
      els.claimStatus.style.color = 'var(--red)';
      els.claimStatus.textContent = 'Doesn\'t look like a valid Glaciem address (starts with R, 97 chars).';
      return;
    }
    localStorage.setItem('wordmine.address', addr);
    els.claimBtn.disabled = true;
    els.claimBtn.textContent = 'SUBMITTING…';
    els.claimStatus.style.color = 'var(--dim)';
    els.claimStatus.textContent = 'Queued. Payout daemon picks it up within ~10s.';
    const res = await api('/claim', {
      token: els.claimBtn.dataset.token, address: addr,
    });
    if (res.status !== 200) {
      els.claimBtn.disabled = false;
      els.claimBtn.textContent = 'CLAIM';
      els.claimStatus.style.color = 'var(--red)';
      els.claimStatus.textContent = res.body.error || 'claim failed';
      return;
    }
    els.claimBtn.textContent = 'CLAIMED';
    pollClaimStatus(res.body.claim_id);
  }

  async function pollClaimStatus(claimId) {
    let tries = 0;
    const tick = async () => {
      tries++;
      const r = await api('/claim_status?id=' + encodeURIComponent(claimId));
      if (r.status === 200 && r.body.status === 'paid') {
        els.claimStatus.style.color = 'var(--green)';
        els.claimStatus.innerHTML =
          'Paid. <span class="mono">tx ' + (r.body.tx_hash || '').slice(0, 12) + '…</span>';
        return;
      }
      if (r.body && r.body.status === 'failed') {
        els.claimStatus.style.color = 'var(--red)';
        els.claimStatus.textContent = 'Payout failed: ' + (r.body.error || 'unknown');
        els.claimBtn.disabled = false;
        els.claimBtn.textContent = 'RETRY';
        return;
      }
      if (tries < 30) setTimeout(tick, 5000);
      else {
        els.claimStatus.style.color = 'var(--dim)';
        els.claimStatus.textContent = 'Payout still pending after 2.5 min — check the address later.';
      }
    };
    tick();
  }

  // ----- faucet status (balance + max-reward math) -----------------------

  async function refreshFaucet() {
    const r = await api('/faucet');
    if (r.status !== 200) return;
    const bal = r.body.balance;
    const addr = r.body.address;
    const max = Math.min(1.0, bal / 1000);
    const min = max / 6;
    els.faucetBal.textContent = formatGlac(bal);
    els.maxReward.textContent = formatGlac(max);
    els.minReward.textContent = formatGlac(min);
    if (addr) els.faucetAddr.textContent = addr;
  }

  // ----- captcha (Cloudflare Turnstile) -----------------------------------

  window.onTurnstileLoad = function () {
    if (typeof turnstile === 'undefined') return;
    turnstile.render(els.captchaWrap, {
      // glacwordmine Turnstile widget (public site key; the secret lives
      // on the Worker via `wrangler secret put TURNSTILE_SECRET`).
      sitekey: '0x4AAAAAADUJKvz_iS7-vogJ',
      theme: 'dark',
      callback: (t) => {
        captchaToken = t;
        els.playBtn.disabled = false;
        els.playBtn.textContent = 'PLAY';
      },
      'expired-callback': () => {
        captchaToken = null;
        els.playBtn.disabled = true;
        els.playBtn.textContent = 'CAPTCHA EXPIRED';
      },
    });
  };

  // Poll for Turnstile script load (it's async)
  const turnstileWaiter = setInterval(() => {
    if (typeof turnstile !== 'undefined') {
      clearInterval(turnstileWaiter);
      window.onTurnstileLoad();
    }
  }, 200);

  // ----- wallet UI --------------------------------------------------------

  // Lazy-loaded wallet module (loads ~44 KB of vendored noble crypto only
  // when the user actually clicks Generate / Restore -- not on every page
  // hit). Same-origin, no third-party CDN at runtime.
  let walletMod = null;
  async function loadWalletMod() {
    if (walletMod) return walletMod;
    try {
      walletMod = await import('./lib/wallet.bundle.js');
      return walletMod;
    } catch (e) {
      toast('Couldn\'t load wallet code — check your network');
      throw e;
    }
  }

  function shortAddr(a) {
    if (!a) return '';
    return a.slice(0, 12) + '…' + a.slice(-8);
  }

  function renderSavedWallet() {
    const a = localStorage.getItem('wordmine.address');
    if (a && /^R[1-9A-HJ-NP-Za-km-z]{96}$/.test(a)) {
      els.walletEmpty.hidden = true;
      els.walletSaved.hidden = false;
      els.walletSavedAddr.textContent = a;
      els.claimAddress.value = a;
    } else {
      els.walletEmpty.hidden = false;
      els.walletSaved.hidden = true;
    }
    els.walletPanel.hidden = false;
  }

  function hideAllWalletPanels() {
    els.walletShow.hidden = true;
    els.walletRestore.hidden = true;
    // Drop the seed words from the DOM so they don't linger anywhere
    els.walletShowSeed.innerHTML = '';
    els.walletShowAddr.textContent = '';
    els.seedConfirmCb.checked = false;
    els.seedContinue.disabled = true;
  }

  async function generateNewWallet() {
    const btn = els.genWalletBtn;
    btn.disabled = true;
    btn.textContent = 'GENERATING…';
    let w;
    try {
      const { generateWallet } = await loadWalletMod();
      w = generateWallet();
    } catch (e) {
      btn.disabled = false;
      btn.textContent = 'GENERATE NEW WALLET';
      return;
    }
    btn.disabled = false;
    btn.textContent = 'GENERATE NEW WALLET';

    showSeed(w);
  }

  function showSeed(w) {
    els.walletShowAddr.textContent = w.address;
    els.walletShowSeed.innerHTML = '';
    w.mnemonic.forEach((word, i) => {
      // Number prefix renders via a CSS ::before pseudo-element from
      // data-n so a select-all on the grid grabs only the words.
      const wrap = document.createElement('div');
      wrap.className = 'word';
      wrap.dataset.n = String(i + 1);
      const wspan = document.createElement('span');
      wspan.className = 'w';
      wspan.textContent = word;
      wrap.appendChild(wspan);
      els.walletShowSeed.appendChild(wrap);
    });
    // Stash the seed as a plain string on the copy button so the
    // Copy-all handler can grab it without traversing the DOM.
    els.seedCopyBtn.dataset.seed = w.mnemonic.join(' ');

    // Hide the wallet panel + start; show the seed-display panel
    els.walletPanel.hidden = true;
    els.start.hidden = true;
    els.walletRestore.hidden = true;
    els.walletShow.hidden = false;

    // Stash the address (NOT the seed) on the continue button
    els.seedContinue.dataset.addr = w.address;
  }

  els.genWalletBtn.addEventListener('click', generateNewWallet);

  els.seedCopyBtn.addEventListener('click', () => {
    const seed = els.seedCopyBtn.dataset.seed || '';
    if (!seed) return;
    navigator.clipboard.writeText(seed).then(
      () => toast('25 words copied — paste into wallet restore'),
      () => toast('Copy failed — select the words manually')
    );
  });

  els.seedConfirmCb.addEventListener('change', () => {
    els.seedContinue.disabled = !els.seedConfirmCb.checked;
  });

  els.seedContinue.addEventListener('click', () => {
    const a = els.seedContinue.dataset.addr;
    if (a) localStorage.setItem('wordmine.address', a);
    hideAllWalletPanels();
    renderSavedWallet();
    els.start.hidden = false;
    toast('Address saved');
  });

  els.walletChangeBtn.addEventListener('click', () => {
    if (!confirm('Replacing the saved address. Make sure you have your current seed first.')) return;
    localStorage.removeItem('wordmine.address');
    els.claimAddress.value = '';
    renderSavedWallet();
  });

  els.restoreWalletBtn.addEventListener('click', () => {
    els.walletPanel.hidden = true;
    els.start.hidden = true;
    els.walletRestore.hidden = false;
    els.restoreInput.value = '';
    els.restoreErr.textContent = '';
  });

  els.restoreCancelBtn.addEventListener('click', () => {
    els.walletRestore.hidden = true;
    els.start.hidden = false;
    renderSavedWallet();
  });

  els.restoreGoBtn.addEventListener('click', async () => {
    els.restoreErr.textContent = '';
    const raw = els.restoreInput.value.trim().toLowerCase();
    const words = raw.split(/\s+/).filter(Boolean);
    if (words.length !== 25) {
      els.restoreErr.textContent = `Need exactly 25 words (got ${words.length}).`;
      return;
    }
    let w;
    try {
      const { restoreFromMnemonic } = await loadWalletMod();
      w = restoreFromMnemonic(words);
    } catch (e) {
      els.restoreErr.textContent = e.message || 'Invalid seed.';
      return;
    }
    // Restore is a "you already know the seed" flow -- don't re-display it.
    // Just save the address and continue.
    localStorage.setItem('wordmine.address', w.address);
    els.walletRestore.hidden = true;
    els.start.hidden = false;
    renderSavedWallet();
    toast('Wallet restored — address saved');
  });

  // ----- bootstrap --------------------------------------------------------

  buildBoard();
  buildKeyboard();
  showStart();
  renderSavedWallet();
  refreshFaucet();
  setInterval(refreshFaucet, 30000);

  els.playBtn.addEventListener('click', startGame);
  els.claimBtn.addEventListener('click', submitClaim);
  els.copyAddress.addEventListener('click', () => {
    const a = els.faucetAddr.textContent.trim();
    if (a && a !== 'loading…') {
      navigator.clipboard.writeText(a).then(() => toast('Address copied'));
    }
  });

  // Pre-fill remembered address on the win panel
  const saved = localStorage.getItem('wordmine.address');
  if (saved) els.claimAddress.value = saved;

  // CSP-friendly replacement for inline onclick="location.reload()" --
  // CLOSE, PLAY AGAIN LATER, REFRESH buttons all carry `data-reload`.
  document.querySelectorAll('[data-reload]').forEach((b) => {
    b.addEventListener('click', () => location.reload());
  });
})();
