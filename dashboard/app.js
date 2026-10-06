// EdgeVision dashboard.
//
// Live telemetry comes from the Wi-Fi module (Server-Sent Events); the event
// history comes from the cloud API: the local stub by default, or the AWS
// HTTP API with ?api=<EventsApiUrl>. ?module= overrides the module address.

(() => {
  "use strict";

  const params = new URLSearchParams(location.search);
  const MODULE = params.get("module") || "http://127.0.0.1:8081";
  const API = params.get("api") || `${location.origin}/api/events`;
  const WINDOW = 60;

  const $ = (id) => document.getElementById(id);
  // Round down: anything uploaded must never display as "100% saved".
  const pctDown = (v, digits) => `${(Math.floor(v * 10 ** digits) / 10 ** digits).toFixed(digits)}%`;
  const fmtBytes = (b) => (b >= 1e6 ? `${(b / 1e6).toFixed(1)} MB` : b >= 1e3 ? `${(b / 1e3).toFixed(1)} KB` : `${b} B`);
  const fmtClock = (s) => `${String(Math.floor(s / 60)).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`;
  const ago = (ms) => {
    const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
    return s < 60 ? `${s} s ago` : s < 3600 ? `${Math.floor(s / 60)} min ago` : `${Math.floor(s / 3600)} h ago`;
  };
  const label = (event) => event.replace(/_detected$/, "").replace(/_/g, " ");

  const history = []; // { t, full, drop, gate }
  let lastEvent = null;

  // ---- Live telemetry ---------------------------------------------------------

  function setPill(pill, text, cls) {
    pill.classList.remove("ok", "bad", "warn");
    if (cls) pill.classList.add(cls);
    pill.querySelector("span:last-child").textContent = text;
  }

  function render(tel) {
    $("banner").hidden = true;
    $("device").textContent = `Device ${tel.device} · front door`;
    $("uptime").textContent = `uptime ${fmtClock(tel.t)}`;
    setPill($("state-pill"), tel.state === "streaming" ? "Camera streaming" : "Camera asleep", tel.state === "streaming" ? "ok" : null);

    const net = tel.network;
    const linkText = !net.cable ? "No Wi-Fi module" : net.online ? "Cloud connected" : `Offline · ${net.queued} queued`;
    setPill($("link-pill"), linkText, !net.cable ? "bad" : net.online ? "ok" : "warn");

    // KPIs
    const bw = tel.bandwidth;
    $("saved").textContent = bw.streaming_baseline_bytes > 0 ? pctDown(bw.saved_vs_streaming_pct, 2) : "--";
    $("saved-detail").textContent = `${fmtBytes(bw.uploaded_bytes)} sent instead of ${fmtBytes(bw.streaming_baseline_bytes)} of video`;
    $("events").textContent = tel.events.total;
    $("fps").textContent = tel.camera.fps;
    $("npu").textContent = tel.npu.jobs;
    $("npu-ms").textContent = `per second · ${tel.npu.device_ms} ms each`;
    $("heap").textContent = `${tel.heap.used_kb} KB`;
    $("heap-bar").style.width = `${(100 * tel.heap.used_kb / tel.heap.total_kb).toFixed(1)}%`;
    $("heap-note").textContent = `of ${tel.heap.total_kb} KB · peak ${tel.heap.peak_kb} KB`;

    // Frame budget: letters as well as color, so it never relies on hue alone.
    $("history").replaceChildren(...[...tel.governor.history].map((c) => {
      const s = document.createElement("span");
      s.className = c;
      s.textContent = c === "." ? "" : c;
      s.title = { P: "checked by the AI chip", D: "dropped by the governor", M: "stopped at the motion gate", ".": "no frame yet" }[c];
      return s;
    }));
    const used = Math.min(100, (tel.governor.admitted * tel.governor.est_ms) / 10);
    $("budget-used").textContent = `${Math.round(used)}%`;
    $("budget-bar").style.width = `${used}%`;
    $("est").textContent = `${tel.governor.est_ms} ms`;
    $("admit").textContent = `${tel.governor.admitted} / ${tel.governor.dropped}`;
    $("overload-badge").hidden = !tel.faults.cpu_overload;

    // Detections
    $("scores").replaceChildren(...["person", "vehicle", "animal"].map((cat) => {
      const v = tel.best[cat];
      const row = document.createElement("div");
      row.className = "score-row";
      row.innerHTML =
        `<span class="name">${cat}</span>` +
        `<div class="score-track"><div class="score-fill" style="width:${(v * 100).toFixed(0)}%"></div>` +
        `<div class="score-mark" title="event threshold 0.50"></div></div>` +
        `<b>${v.toFixed(2)}</b>`;
      return row;
    }), Object.assign(document.createElement("div"), {
      className: "score-legend",
      innerHTML: "<span>0</span><span>threshold 0.50</span><span>1</span>",
    }));

    // Health
    const names = { F: "free", W: "DMA writing", R: "queued", P: "processing", X: "lent away" };
    $("slots").replaceChildren(...[...tel.pool].map((c, i) => {
      const s = document.createElement("div");
      s.className = `slot ${c}`;
      s.innerHTML = `<span class="slot-name">Slot ${i}</span><span class="slot-state">${names[c] || c}</span>`;
      return s;
    }));
    $("late").textContent = tel.heap.late_allocs;
    $("late").classList.toggle("alert", tel.heap.late_allocs > 0);
    $("restarts").textContent = tel.npu.restarts;
    $("corrupt").textContent = tel.camera.corrupt;
    $("resets").textContent = tel.camera.sensor_resets;
    $("queued").textContent = net.queued;
    $("mem-badge").hidden = !tel.faults.mem_pressure;

    history.push({ t: tel.t, full: tel.motion.forwarded, drop: tel.governor.dropped, gate: tel.motion.gated });
    while (history.length > WINDOW) history.shift();
    drawChart();
    if (!$("table-scroll").hidden) fillTable();
  }

  function connect() {
    const es = new EventSource(`${MODULE}/api/telemetry/stream`);
    es.onmessage = (msg) => render(JSON.parse(msg.data));
    es.onerror = () => {
      $("banner").hidden = false;
      setPill($("link-pill"), "No Wi-Fi module", "bad");
      setPill($("state-pill"), "No telemetry", null);
    };
  }

  // ---- Chart: stacked bars per second ----------------------------------------

  const W = 640, H = 220, L = 30, R = 6, T = 10, B = 24;
  const SVG = "http://www.w3.org/2000/svg";

  function el(name, attrs, text) {
    const e = document.createElementNS(SVG, name);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function drawChart() {
    const svg = $("chart");
    svg.replaceChildren();
    const max = Math.max(35, ...history.map((d) => d.full + d.drop + d.gate));
    const y = (v) => T + (H - T - B) * (1 - v / max);
    const slot = (W - L - R) / WINDOW;
    const barW = Math.max(2, slot - 2); // 2px surface gap between bars

    for (const v of [0, 10, 20, 30]) {
      svg.append(el("line", { class: v ? "gridline" : "baseline", x1: L, x2: W - R, y1: y(v), y2: y(v) }));
      svg.append(el("text", { class: "tick", x: L - 8, y: y(v) + 4, "text-anchor": "end" }, v));
    }
    svg.append(el("text", { class: "tick", x: L, y: H - 6 }, "60 s ago"));
    svg.append(el("text", { class: "tick", x: W - R, y: H - 6, "text-anchor": "end" }, "now"));

    if (!history.length) {
      svg.append(el("text", { class: "placeholder", x: (W + L) / 2, y: H / 2, "text-anchor": "middle" }, "Waiting for frames"));
      return;
    }

    const offset = WINDOW - history.length;
    history.forEach((d, i) => {
      const x = L + (offset + i) * slot + 1;
      let base = 0;
      // Bottom to top: AI chip, dropped, motion gate (adjacent pairs validated).
      for (const [key, color] of [["full", "var(--series-full)"], ["drop", "var(--series-drop)"], ["gate", "var(--series-gate)"]]) {
        const v = d[key];
        if (v <= 0) continue;
        const top = y(base + v), bottom = y(base);
        const h = Math.max(0, bottom - top - (base > 0 ? 2 : 0)); // 2px gap between stacked segments
        svg.append(el("rect", { x, y: top, width: barW, height: h, rx: 1.5, fill: color }));
        base += v;
      }
      svg.append(el("rect", { class: "hit", x: x - 1, y: T, width: slot, height: H - T - B, rx: 2, "data-i": i }));
    });
  }

  function showTooltip(evt) {
    const i = evt.target.getAttribute && evt.target.getAttribute("data-i");
    const tip = $("tooltip");
    if (i === null || i === undefined) { tip.hidden = true; return; }
    const d = history[+i];
    tip.innerHTML =
      `<div class="tip-title">Second ${d.t}</div>` +
      `<div class="row"><span class="swatch s-full"></span>AI chip<b>${d.full}</b></div>` +
      `<div class="row"><span class="swatch s-drop"></span>Dropped<b>${d.drop}</b></div>` +
      `<div class="row"><span class="swatch s-gate"></span>Motion gate<b>${d.gate}</b></div>`;
    const wrap = $("chart-wrap").getBoundingClientRect();
    tip.hidden = false;
    const left = Math.min(evt.clientX - wrap.left + 14, wrap.width - tip.offsetWidth - 4);
    tip.style.left = `${Math.max(0, left)}px`;
    tip.style.top = `${Math.max(0, evt.clientY - wrap.top - tip.offsetHeight - 10)}px`;
  }

  function fillTable() {
    $("data-body").replaceChildren(...history.slice().reverse().map((d) => {
      const tr = document.createElement("tr");
      tr.innerHTML = `<td>${d.t}</td><td class="num">${d.full}</td><td class="num">${d.drop}</td><td class="num">${d.gate}</td>`;
      return tr;
    }));
  }

  $("chart").addEventListener("mousemove", showTooltip);
  $("chart").addEventListener("mouseleave", () => { $("tooltip").hidden = true; });
  $("table-toggle").addEventListener("click", (e) => {
    const table = $("table-scroll");
    table.hidden = !table.hidden;
    $("chart-wrap").hidden = !table.hidden;
    e.target.setAttribute("aria-pressed", String(!table.hidden));
    e.target.textContent = table.hidden ? "Show table" : "Show chart";
    if (!table.hidden) fillTable();
  });

  // ---- Event history from the cloud ------------------------------------------

  let events = [];

  function renderEvents() {
    if (!events.length) return;
    $("events-body").replaceChildren(...events.map((e) => {
      const tr = document.createElement("tr");
      const conf = typeof e.confidence === "number" ? e.confidence : null;
      tr.innerHTML =
        `<td class="when">${ago(e.received_at)}<small>${new Date(e.received_at).toLocaleTimeString()}</small></td>` +
        `<td><span class="chip">${label(e.event)}</span></td>` +
        `<td>${conf === null ? "--" : `<div class="conf"><div class="meter"><div class="meter-fill" style="width:${Math.round(conf * 100)}%"></div></div><span>${Math.round(conf * 100)}%</span></div>`}</td>` +
        `<td>${e.device}</td>` +
        `<td class="num">${e.seq ?? "--"}</td>`;
      return tr;
    }));
    const e = events[0];
    $("last-event").textContent = `last: ${label(e.event)}, ${ago(e.received_at)}`;
  }

  async function loadEvents() {
    try {
      const res = await fetch(`${API}?limit=10`);
      events = (await res.json()).events;
      $("api-source").textContent = API.startsWith(location.origin) ? "Latest events stored by the local backend" : `Latest events from ${new URL(API).host}`;
      renderEvents();
    } catch {
      $("api-source").textContent = `Cloud API not reachable at ${API}`;
    }
  }

  // ---- Controls -----------------------------------------------------------------

  document.querySelectorAll("button[data-fault], button[data-action]").forEach((b) => {
    b.addEventListener("click", async () => {
      const path = b.dataset.fault ? `/api/fault/${b.dataset.fault}` : "/api/motion";
      try {
        const res = await fetch(`${MODULE}${path}`, { method: "POST" });
        $("action-result").textContent = res.ok ? `Sent: ${b.textContent.toLowerCase()}` : `Refused (${res.status})`;
        b.classList.add("sent");
        setTimeout(() => b.classList.remove("sent"), 1200);
      } catch {
        $("action-result").textContent = "Wi-Fi module not reachable";
      }
    });
  });

  drawChart();
  connect();
  loadEvents();
  setInterval(loadEvents, 3000);
  setInterval(renderEvents, 1000); // keep "n s ago" current
})();
