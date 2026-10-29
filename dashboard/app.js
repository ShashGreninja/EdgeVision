(() => {
  'use strict';
  const $ = id => document.getElementById(id), F = window.EdgeFlow;
  const params = new URLSearchParams(location.search);
  let moduleURL = params.get('module') || 'http://127.0.0.1:8081';
  let apiURL = params.get('api') || `${location.origin}/api/events`;
  let mode = params.get('mode') === 'live' ? 'live' : 'recorded';
  let telemetry = null, events = [], connected = false, lastTelemetry = 0, triggeredAt = null;
  let source = null, polling = null, replayTimer = null, recording = null, replayIndex = 0, playing = false, generation = 0;
  let lastGoodEvents = 0, history = [], activeView = 'demo';
  let connectionOpenedByUser = false;
  let eventCountBeforeTrigger = 0;
  const video = $('visit-video');
  const bytes = value => value >= 1e6 ? `${(value / 1e6).toFixed(1)} MB` : value >= 1e3 ? `${(value / 1e3).toFixed(1)} KB` : `${value} B`;
  const clock = value => `${String(Math.floor(value / 60)).padStart(2, '0')}:${String(Math.floor(value % 60)).padStart(2, '0')}`;
  const text = (tag, value, cls) => { const node = document.createElement(tag); node.textContent = value; if (cls) node.className = cls; return node; };
  const notice = value => { $('notice').textContent = value; $('notice').hidden = !value; };
  document.querySelectorAll('[data-destination]').forEach(a => { a.href = F.destination(a.dataset.destination, location.search); });

  function setView(view, focus = false) {
    activeView = ['demo', 'events', 'engineering'].includes(view) ? view : 'demo';
    document.querySelectorAll('[data-view]').forEach(button => {
      const selected = button.dataset.view === activeView;
      button.setAttribute('aria-selected', String(selected)); button.tabIndex = selected ? 0 : -1;
      if (selected && focus) button.focus();
    });
    for (const name of ['demo', 'events', 'engineering']) $(`view-${name}`).hidden = name !== activeView;
    historyURL();
  }
  function historyURL() {
    const query = new URLSearchParams(); query.set('mode', mode);
    if (params.has('api') || apiURL !== `${location.origin}/api/events`) query.set('api', apiURL);
    if (moduleURL !== 'http://127.0.0.1:8081') query.set('module', moduleURL);
    window.history.replaceState(null, '', `app.html?${query}#${activeView}`);
    document.querySelectorAll('[data-destination]').forEach(a => { a.href = F.destination(a.dataset.destination, '?' + query); });
  }
  document.querySelectorAll('[data-view]').forEach(button => {
    button.addEventListener('click', () => setView(button.dataset.view));
    button.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
      event.preventDefault(); const names = ['demo', 'events', 'engineering'];
      const index = names.indexOf(activeView);
      const next = event.key === 'Home' ? 0 : event.key === 'End' ? 2 : (index + (event.key === 'ArrowRight' ? 1 : 2)) % 3;
      setView(names[next], true);
    });
  });
  document.querySelectorAll('[data-open-view]').forEach(b => b.addEventListener('click', () => setView(b.dataset.openView, true)));

  function renderJourney() {
    let journey = F.journey({ connected, telemetry: telemetry || {}, triggeredAt, events, expectedEvent: 'detection' });
    if (mode === 'recorded') {
      if (!recording) journey = { stage: 'connect', title: 'Loading the recorded visit', detail: 'The recording includes real firmware telemetry and matching AWS results.' };
      else if (!triggeredAt) journey = { stage: 'ready', title: 'Watch a visit unfold', detail: 'Play a real simulator run and follow its event all the way to AWS.' };
      else if (journey.stage === 'stored') journey = { ...journey, title: 'A small event made it', detail: 'This recorded visit produced an event that AWS returned. No video traveled through the event pipeline.' };
    }
    $('journey-title').textContent = journey.title; $('journey-detail').textContent = journey.detail;
    const state = telemetry?.state, stored = journey.stage === 'stored';
    const detected = (telemetry?.events?.total || 0) > eventCountBeforeTrigger;
    const done = { connect: connected, awake: !!triggeredAt && (state === 'streaming' || detected || stored), detect: !!triggeredAt && (detected || stored), stored };
    if (mode === 'live' && stored) $('action-result').textContent = 'New detection confirmed by the backend.';
    else if (mode === 'live' && journey.stage === 'queued') $('action-result').textContent = 'Event buffered while the cloud link is offline.';
    for (const [name, complete] of Object.entries(done)) {
      $(`journey-${name}`).classList.toggle('active', complete);
      $(`journey-${name}`).setAttribute('aria-label', `${$(`journey-${name}`).querySelector('span').textContent}: ${complete ? 'observed' : 'waiting'}`);
    }
    const allowCommands = mode === 'live' && connected;
    $('inject-fault').disabled = !allowCommands; $('selftest').disabled = !allowCommands;
    if (mode === 'live') $('primary-action').disabled = !connected;
    $('connection-dot').classList.toggle('connected', connected);
    if (mode === 'live' && !connected && telemetry) $('sample-time').textContent = `Last received · ${clock(telemetry.t)}`;
    $('connection-summary').textContent = mode === 'recorded' ? 'Recorded data. No local setup required.' : connected ? (telemetry?.network?.online ? 'Firmware connected · cloud link online' : 'Firmware connected · cloud link offline') : 'Simulator not connected';
  }

  function renderTelemetry(tel) {
    telemetry = tel; if (!tel) { renderJourney(); return; }
    const net = tel.network, bw = tel.bandwidth;
    $('camera-state').textContent = tel.state === 'streaming' ? 'Checking the scene' : 'Asleep';
    $('captured').textContent = `${tel.camera.captured} frames`;
    $('uploaded').textContent = bytes(bw.uploaded_bytes); $('baseline').textContent = bytes(bw.streaming_baseline_bytes);
    const saving = F.savings(bw.uploaded_bytes, bw.streaming_baseline_bytes);
    $('saved-detail').textContent = saving === null ? 'The baseline starts when the camera wakes. It assumes 2 Mbit/s video.' : `${saving}% less payload than the assumed 2 Mbit/s video baseline. Transport overhead is excluded.`;
    $('sample-time').textContent = `${mode === 'recorded' ? 'Recording' : 'Live'} · ${clock(tel.t)}`;
    $('playback-time').textContent = clock(tel.t); $('uptime').textContent = `Uptime ${clock(tel.t)}`;
    $('heap').textContent = `${tel.heap.used_kb} / ${tel.heap.total_kb} KB`;
    $('heap-note').textContent = `Peak ${tel.heap.peak_kb} KB · tracked firmware heap only`;
    $('heap-bar').style.width = `${Math.min(100, tel.heap.used_kb / tel.heap.total_kb * 100)}%`;
    for (const [id, value] of Object.entries({ late: tel.heap.late_allocs, restarts: tel.npu.restarts, corrupt: tel.camera.corrupt, resets: tel.camera.sensor_resets, queued: net.queued, est: `${tel.governor.est_ms} ms`, admit: `${tel.governor.admitted} / ${tel.governor.dropped}`, 'budget-used': `${Math.round(Math.min(100, tel.governor.admitted * tel.governor.est_ms / 10))}%` })) $(id).textContent = value;
    $('scores').replaceChildren(...['person', 'vehicle', 'animal'].map(name => {
      const row = text('div', '', 'score-row'), track = text('div', '', 'score-track'), fill = text('div', '', 'score-fill');
      fill.style.width = `${Math.max(0, Math.min(1, tel.best[name])) * 100}%`; track.append(fill);
      row.append(text('span', name), track, text('b', tel.best[name].toFixed(2))); return row;
    }));
    const slotNames = { F: 'Free', W: 'DMA writing', R: 'Queued', P: 'Processing', X: 'Lent away' };
    $('slots').replaceChildren(...[...tel.pool].map((name, index) => { const slot = text('div', `Slot ${index + 1}`, 'slot'); slot.append(text('span', slotNames[name] || name)); return slot; }));
    $('history').replaceChildren(...[...tel.governor.history].map(value => { const cell = text('span', value === '.' ? '—' : value); cell.title = { P: 'Admitted for processing', M: 'Motion-gated', D: 'Dropped', '.': 'No frame' }[value]; return cell; }));
    if (history.length && tel.t < history.at(-1).t) history = [];
    if (!history.length || tel.t !== history.at(-1).t) history.push({ t: tel.t, full: tel.motion.forwarded, gate: tel.motion.gated, drop: tel.governor.dropped });
    history = history.slice(-60); drawChart(); renderJourney();
  }

  function renderEvents() {
    const filtered = F.filterEvents(events, $('event-filter').value);
    $('events-count').textContent = `${filtered.length} ${filtered.length === 1 ? 'event' : 'events'} in this view`;
    $('events-body').replaceChildren();
    if (!filtered.length) { const tr = document.createElement('tr'); const message = events.length ? 'No events match this filter. Choose All events to see the available results.' : mode === 'recorded' ? 'Play the recorded visit to reveal its stored events.' : 'No events yet. Trigger motion or send a test event.'; const td = text('td', message, 'empty-events'); td.colSpan = 5; tr.append(td); $('events-body').append(tr); }
    for (const event of filtered) {
      const tr = document.createElement('tr'), when = text('td', new Date(event.received_at).toLocaleTimeString());
      when.append(text('small', new Date(event.received_at).toLocaleDateString()));
      const confidence = /_detected$/.test(event.event) && typeof event.confidence === 'number' ? `${Math.round(event.confidence * 100)}%` : '—';
      const action = document.createElement('td'), button = text('button', 'Inspect', 'text-button');
      button.setAttribute('aria-label', `Inspect ${F.eventLabel(event.event)} event`);
      button.addEventListener('click', () => { $('event-json').textContent = JSON.stringify(event, null, 2); $('event-dialog').showModal(); }); action.append(button);
      tr.append(when, text('td', F.eventLabel(event.event)), text('td', confidence, 'num'), text('td', event.device || '—'), action); $('events-body').append(tr);
    }
    const latest = events[0], result = $('latest-result'); result.replaceChildren();
    if (!latest) {
      result.append(text('p', mode === 'recorded' ? 'Play the visit to see its first stored event.' : 'No stored event yet. Trigger motion to begin.', 'result-empty'));
      result.append(text('p', 'Look for the event type, its confidence, the captured frame, and the time the backend received it.', 'small muted'));
    }
    else {
      const title = text('div', '', 'result-title'); title.append(text('strong', F.eventLabel(latest.event)), text('span', /_detected$/.test(latest.event) && typeof latest.confidence === 'number' ? `${Math.round(latest.confidence * 100)}% confidence` : 'Connectivity check'));
      const details = text('dl', '', 'event-summary');
      for (const [label, value] of [['Device', latest.device || '—'], ['Frame', latest.seq ?? '—'], ['Received', new Date(latest.received_at).toLocaleTimeString()]]) { const item = document.createElement('div'); item.append(text('dt', label), text('dd', value)); details.append(item); }
      result.append(title, details);
    }
    renderJourney();
  }

  async function loadEvents() {
    if (mode !== 'live') return; const run = generation;
    try {
      const response = await fetch(F.eventsURL(apiURL, 50), { signal: AbortSignal.timeout(6000) });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const payload = await response.json();
      if (!Array.isArray(payload.events)) throw new Error('Unexpected response');
      if (generation !== run || mode !== 'live') return;
      events = payload.events.filter(e => e && typeof e === 'object' && Number.isFinite(e.received_at));
      lastGoodEvents = Date.now(); $('api-source').textContent = `Backend: ${new URL(apiURL).host}`;
      $('events-freshness').textContent = `Updated ${new Date(lastGoodEvents).toLocaleTimeString()} · live backend`;
      renderEvents();
    } catch { if (generation === run) { $('api-source').textContent = 'Events API unavailable. Check the URL in Connection settings.'; $('events-freshness').textContent = lastGoodEvents ? `Refresh failed. Last updated ${new Date(lastGoodEvents).toLocaleTimeString()}.` : 'Could not load the events API. Check Connection settings.'; } }
  }

  function connectTelemetry() {
    source?.close(); source = new EventSource(`${moduleURL}/api/telemetry/stream`);
    const run = generation;
    source.onmessage = message => {
      if (run !== generation || mode !== 'live') return;
      try { const tel = JSON.parse(message.data); if (!tel.camera || !tel.heap || !tel.network || !tel.governor || !tel.best || !tel.motion || !tel.npu || !tel.events || !tel.bandwidth || typeof tel.pool !== 'string') return;
        connected = true; lastTelemetry = Date.now();
        if (!connectionOpenedByUser) $('connection-panel').open = false;
        renderTelemetry(tel);
      } catch { notice('A telemetry message could not be read. Waiting for the next sample.'); }
    };
    source.onerror = () => { if (run === generation && mode === 'live') { connected = false; renderJourney(); } };
  }

  async function switchMode(next) {
    mode = next; generation++; source?.close(); clearInterval(polling); clearInterval(replayTimer); playing = false; video.pause();
    connected = false; triggeredAt = null; telemetry = null; events = []; history = []; lastTelemetry = 0; lastGoodEvents = 0; replayIndex = 0; notice('');
    connectionOpenedByUser = false;
    eventCountBeforeTrigger = 0;
    for (const id of ['camera-state', 'captured', 'uploaded', 'baseline', 'heap', 'late', 'restarts', 'corrupt', 'resets', 'queued', 'est', 'admit', 'budget-used']) $(id).textContent = '—';
    for (const id of ['slots', 'scores', 'history']) $(id).replaceChildren();
    $('heap-bar').style.width = '0%'; $('sample-time').textContent = 'Waiting for telemetry';
    $('saved-detail').textContent = 'The baseline assumes 2 Mbit/s video while the camera is awake.';
    drawChart();
    document.querySelectorAll('[data-mode]').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.mode === mode)));
    $('connection-panel').hidden = mode !== 'live'; $('connection-toggle').hidden = mode !== 'live'; $('replay-reset').hidden = mode === 'live';
    $('mode-label').textContent = mode === 'recorded' ? 'Recorded evaluation' : 'Live simulator';
    $('preview-label').textContent = mode === 'recorded' ? 'Evaluation footage · recorded' : 'Evaluation preview · telemetry is live';
    $('primary-action').textContent = mode === 'recorded' ? 'Play visit' : 'Trigger motion';
    $('primary-action').disabled = mode === 'live'; $('action-result').textContent = '';
    $('source-note').textContent = mode === 'recorded' ? 'Replay actual firmware telemetry and verified AWS results.' : 'Control your local simulator and follow new cloud events.';
    $('data-source').textContent = mode === 'recorded' ? 'Recorded simulator run · no live commands' : 'Live simulator telemetry · selected events backend';
    $('fault-help').textContent = mode === 'recorded' ? 'Connect the live simulator to inject faults. Recorded playback never sends device commands.' : 'Inject a temporary fault and observe its recovery counters or queued events.';
    $('events-freshness').textContent = mode === 'recorded' ? 'A captured run, not a live connection.' : 'Connecting to the events backend…';
    $('api-source').textContent = mode === 'recorded' ? 'Loading recorded AWS responses…' : 'Connecting to the events API…';
    $('module-url').value = moduleURL; $('events-url').value = apiURL; historyURL(); renderEvents();
    if (mode === 'live') { $('connection-panel').open = true; connectTelemetry(); loadEvents(); polling = setInterval(loadEvents, 3000); }
    else {
      const run = generation;
      try { if (!recording) { const response = await fetch('assets/recorded-run.json'); if (!response.ok) throw new Error(); recording = await response.json(); }
        if (run !== generation) return; connected = true; renderTelemetry(recording.samples[0]); $('api-source').textContent = `Recorded AWS response · ${new Date(recording.recorded_at).toLocaleDateString()}`;
      } catch { notice('The recorded visit could not be loaded. Refresh the page or use Live simulator.'); }
    }
  }

  function replaySample() {
    const tel = recording.samples[replayIndex];
    events = recording.events.filter(e => e.uptime_ms <= tel.t * 1000 && tel.network.published > 0);
    renderTelemetry(tel); renderEvents();
    if (Number.isFinite(video.duration) && Math.abs(video.currentTime - tel.t) > 1.5) video.currentTime = Math.min(tel.t, video.duration);
    if (replayIndex >= recording.samples.length - 1) { clearInterval(replayTimer); playing = false; video.pause(); $('primary-action').textContent = 'Replay visit'; $('action-result').textContent = 'Recorded visit complete. Inspect the events or explore Engineering.'; }
    else replayIndex++;
  }
  $('primary-action').addEventListener('click', async () => {
    if (mode === 'live') { await sendCommand('motion'); return; }
    if (!recording) return;
    if (playing) { clearInterval(replayTimer); playing = false; video.pause(); $('primary-action').textContent = 'Resume visit'; return; }
    if (replayIndex >= recording.samples.length - 1) { replayIndex = 0; events = []; history = []; }
    playing = true; triggeredAt = recording.recorded_at; $('primary-action').textContent = 'Pause visit'; $('action-result').textContent = 'Replaying a captured run. No device commands are being sent.';
    video.play().catch(() => {}); replaySample(); replayTimer = setInterval(replaySample, 1000);
  });
  $('replay-reset').addEventListener('click', () => { if (mode === 'recorded') { video.currentTime = 0; switchMode('recorded'); } });
  document.querySelectorAll('[data-mode]').forEach(b => b.addEventListener('click', () => switchMode(b.dataset.mode)));

  async function sendCommand(command) {
    if (mode !== 'live' || !connected) return;
    const resultId = command === 'motion' ? 'action-result' : 'fault-result';
    $(resultId).textContent = 'Sending to the local simulator…';
    const countAtSend = telemetry?.events?.total || 0;
    try {
      const path = command === 'motion' ? '/api/motion' : `/api/fault/${command}`;
      const response = await fetch(moduleURL + path, { method: 'POST', signal: AbortSignal.timeout(5000) });
      if (!response.ok) throw new Error();
      if (command === 'motion') { triggeredAt = Date.now(); eventCountBeforeTrigger = countAtSend; }
      $(resultId).textContent = command === 'motion' ? 'Motion sent. Waiting for a confirmed detection and cloud result.' : command === 'selftest' ? 'Test event requested. Check Events for the stored result.' : 'Fault injected. Watch the live recovery counters.';
      renderJourney();
    } catch { $(resultId).textContent = 'The command could not be sent. Check the simulator connection.'; }
  }
  $('inject-fault').addEventListener('click', () => sendCommand($('fault-select').value));
  $('selftest').addEventListener('click', () => sendCommand('selftest'));
  const faultText = { network_down: 'Watch the queue hold events until the network returns.', inference_hang: 'The watchdog should restart inference and reset the simulated accelerator.', camera_disconnect: 'The driver detects the lost sensor and retries with backoff.', frame_corrupt: 'A CRC check catches the corrupted frame and drops it.', mem_pressure: 'Observe the fixed frame pool under simulated memory pressure.', cpu_overload: 'The governor drops more frames to keep work within budget.' };
  $('fault-select').addEventListener('change', () => { $('fault-description').textContent = faultText[$('fault-select').value]; });
  $('connection-toggle').addEventListener('click', () => { connectionOpenedByUser = true; $('connection-panel').open = !$('connection-panel').open; if ($('connection-panel').open) $('connection-panel').scrollIntoView({ block: 'nearest' }); });
  $('connection-panel').querySelector('summary').addEventListener('click', () => { connectionOpenedByUser = true; });
  $('connection-form').addEventListener('submit', async event => {
    event.preventDefault();
    try {
      const module = new URL($('module-url').value), api = new URL($('events-url').value);
      if (![module, api].every(url => ['http:', 'https:'].includes(url.protocol) && !url.username && !url.password)) throw new Error();
      moduleURL = module.href.replace(/\/$/, ''); apiURL = api.href; params.set('api', apiURL);
      $('connection-result').textContent = 'Checking firmware and events API…';
      const checks = await Promise.allSettled([fetch(moduleURL + '/api/status', { signal: AbortSignal.timeout(5000) }).then(async r => { if (!r.ok) throw new Error(); const status = await r.json(); if (!status.firmware) throw new Error(); }), fetch(F.eventsURL(apiURL, 1), { signal: AbortSignal.timeout(5000) }).then(async r => { if (!r.ok || !Array.isArray((await r.json()).events)) throw new Error(); })]);
      $('connection-result').textContent = `${checks[0].status === 'fulfilled' ? 'Firmware connected' : 'Firmware unavailable'} · ${checks[1].status === 'fulfilled' ? 'events API responding' : 'events API unavailable'}`;
      connectTelemetry(); loadEvents(); historyURL();
    } catch { $('connection-result').textContent = 'Enter valid HTTP or HTTPS URLs without embedded credentials.'; }
  });
  $('copy-command').addEventListener('click', async () => { try { await navigator.clipboard.writeText($('launch-command').textContent); $('copy-command').textContent = 'Command copied'; } catch { $('copy-command').textContent = 'Select and copy the command above'; } });
  $('event-filter').addEventListener('change', renderEvents);
  $('refresh-events').addEventListener('click', () => mode === 'live' ? loadEvents() : renderEvents());
  $('table-toggle').addEventListener('click', () => { const open = $('table-scroll').hidden; $('table-scroll').hidden = !open; $('chart-wrap').hidden = open; $('table-toggle').setAttribute('aria-pressed', String(open)); $('table-toggle').textContent = open ? 'Show chart' : 'Show table'; });

  function drawChart() {
    const ns = 'http://www.w3.org/2000/svg', svg = $('chart'); svg.replaceChildren();
    const make = (tag, attrs, content) => { const el = document.createElementNS(ns, tag); Object.entries(attrs).forEach(([k, v]) => el.setAttribute(k, v)); if (content !== undefined) el.textContent = content; return el; };
    const max = Math.max(40, ...history.map(d => d.full + d.gate + d.drop));
    const y = value => 190 - value / max * 174;
    for (const value of [0, 10, 20, 30, 40]) { svg.append(make('line', { x1: 32, x2: 632, y1: y(value), y2: y(value), class: 'chart-grid' })); svg.append(make('text', { x: 24, y: y(value) + 4, 'text-anchor': 'end', class: 'chart-tick' }, value)); }
    const total = Math.max(15, history.length), step = 600 / total;
    history.forEach((sample, index) => { let base = 0;
      for (const [key, colour] of [['full', 'var(--series-full)'], ['gate', 'var(--series-gate)'], ['drop', 'var(--series-drop)']]) { const count = sample[key]; if (count) svg.append(make('rect', { x: 33 + index * step, y: y(base + count), width: Math.max(1, step - 3), height: Math.max(0, y(base) - y(base + count) - 1), fill: colour })); base += count; }
    });
    for (let i = 0; i < 4; i++) svg.append(make('text', { x: 32 + i * 200, y: 214, 'text-anchor': i === 3 ? 'end' : 'start', class: 'chart-tick' }, history.length ? `${Math.round(history[0].t + (history.at(-1).t - history[0].t) * i / 3)} s` : `${i * 20} s`));
    if (!history.length) svg.append(make('text', { x: 330, y: 100, 'text-anchor': 'middle', class: 'chart-tick' }, 'Waiting for telemetry'));
    $('data-body').replaceChildren(...history.slice().reverse().map(sample => { const tr = document.createElement('tr'); for (const value of [sample.t, sample.full, sample.gate, sample.drop]) tr.append(text('td', value, 'num')); return tr; }));
  }
  setInterval(() => { if (mode === 'live' && connected && Date.now() - lastTelemetry > 6000) { connected = false; renderJourney(); } }, 2000);
  setView(location.hash.slice(1) || 'demo'); drawChart(); switchMode(mode);
})();
