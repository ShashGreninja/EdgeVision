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
  const history = []; // { t, full, drop, gate }

  // ---- Live telemetry ---------------------------------------------------------

  function setPill(pill, label, cls) {
    pill.classList.remove("ok", "bad", "warn");
    if (cls) pill.classList.add(cls);
    pill.querySelector("span:last-child").textContent = label;
  }

  function render(tel) {
    $("device").textContent = tel.device;
    $("uptime").textContent = `uptime ${tel.t} s`;
    setPill($("state-pill"), tel.state === "streaming" ? "streaming" : "idle (sensor off)", tel.state === "streaming" ? "ok" : null);

    const net = tel.network;
    const linkLabel = !net.cable ? "no Wi-Fi module" : net.online ? "cloud connected" : `offline, ${net.queued} queued`;
    setPill($("link-pill"), linkLabel, !net.cable ? "bad" : net.online ? "ok" : "warn");

    const bw = tel.bandwidth;
    $("saved").textContent = bw.streaming_baseline_bytes > 0 ? pctDown(bw.saved_vs_streaming_pct, 2) : "-";
    $("saved-detail").textContent =
      `${fmtBytes(bw.uploaded_bytes)} sent, instead of ${fmtBytes(bw.streaming_baseline_bytes)} of video ` +
      `(2 Mbit/s for the ${(tel.camera.awake_ms / 1000).toFixed(0)} s the camera was awake)`;
    $("saved-always").textContent = `Against 24/7 streaming: ${pctDown(bw.saved_vs_always_on_pct, 3)} saved.`;

    $("events").textContent = tel.events.total;
    $("fps").textContent = tel.camera.fps;
    $("npu").textContent = tel.npu.jobs;
    $("npu-ms").textContent = `${tel.npu.device_ms} ms`;

    // Governor strip: letters as well as color, so it never relies on hue alone.
    $("history").replaceChildren(...[...tel.governor.history].map((c) => {
      const s = document.createElement("span");
      s.className = c;
      s.textContent = c === "." ? "" : c;
      s.title = { P: "checked by the AI chip", D: "dropped", M: "stopped at motion gate", ".": "no frame yet" }[c];
      return s;
    }));
    $("est").textContent = `${tel.governor.est_ms} ms`;
    $("overload").textContent = tel.faults.cpu_overload ? "ON (costs x4)" : "off";

    $("scores").replaceChildren(...["person", "vehicle", "animal"].map((cat) => {
      const v = tel.best[cat];
      const row = document.createElement("div");
      row.className = "score-row";
      row.innerHTML = `<span>${cat}</span><div class="score-track"><div class="score-fill" style="width:${(v * 100).toFixed(0)}%"></div><div class="score-mark" title="event threshold 0.50"></div></div><b>${v.toFixed(2)}</b>`;
      return row;
    }));

    const names = { F: "free", W: "DMA writing", R: "ready", P: "processing", X: "lent away" };
    $("slots").replaceChildren(...[...tel.pool].map((c, i) => {
      const s = document.createElement("span");
      s.className = c;
      s.textContent = `slot ${i}: ${names[c] || c}`;
      return s;
    }));
    $("heap-bar").style.width = `${(100 * tel.heap.used_kb / tel.heap.total_kb).toFixed(1)}%`;
    $("heap").textContent = `${tel.heap.used_kb} / ${tel.heap.total_kb} KB (peak ${tel.heap.peak_kb})`;
    $("late").textContent = tel.heap.late_allocs;
    $("restarts").textContent = tel.npu.restarts;
    $("corrupt").textContent = tel.camera.corrupt;
    $("resets").textContent = tel.camera.sensor_resets;
    $("queued").textContent = net.queued;

    history.push({ t: tel.t, full: tel.motion.forwarded, drop: tel.governor.dropped, gate: tel.motion.gated });
    while (history.length > WINDOW) history.shift();
    drawChart();
    if (!$("data-table").hidden) fillTable();
  }

  function connect() {
    const es = new EventSource(`${MODULE}/api/telemetry/stream`);
    es.onmessage = (msg) => render(JSON.parse(msg.data));
    es.onerror = () => {
      setPill($("link-pill"), "no Wi-Fi module", "bad");
      setPill($("state-pill"), "no telemetry", null);
    };
  }

  // ---- Chart: stacked bars per second ----------------------------------------

  const W = 640, H = 200, L = 30, R = 6, T = 8, B = 22;
  const SVG = "http://www.w3.org/2000/svg";

  function el(name, attrs) {
    const e = document.createElementNS(SVG, name);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
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
      const label = el("text", { class: "tick", x: L - 6, y: y(v) + 4, "text-anchor": "end" });
      label.textContent = v;
      svg.append(label);
    }
    const xl = el("text", { class: "tick", x: W - R, y: H - 4, "text-anchor": "end" });
    xl.textContent = "now";
    svg.append(xl);
    const xr = el("text", { class: "tick", x: L, y: H - 4 });
    xr.textContent = "60 s ago";
    svg.append(xr);

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
      const hit = el("rect", { class: "hit", x: x - 1, y: T, width: slot, height: H - T - B, "data-i": i });
      svg.append(hit);
    });
  }

  function showTooltip(evt) {
    const i = evt.target.getAttribute && evt.target.getAttribute("data-i");
    const tip = $("tooltip");
    if (i === null || i === undefined) { tip.hidden = true; return; }
    const d = history[+i];
    tip.innerHTML =
      `<b>second ${d.t}</b>` +
      `<div class="row"><span class="swatch s-full"></span>AI chip ${d.full}</div>` +
      `<div class="row"><span class="swatch s-drop"></span>dropped ${d.drop}</div>` +
      `<div class="row"><span class="swatch s-gate"></span>motion gate ${d.gate}</div>`;
    const wrap = $("chart-wrap").getBoundingClientRect();
    tip.hidden = false;
    const left = Math.min(evt.clientX - wrap.left + 12, wrap.width - tip.offsetWidth - 4);
    tip.style.left = `${Math.max(0, left)}px`;
    tip.style.top = `${Math.max(0, evt.clientY - wrap.top - tip.offsetHeight - 8)}px`;
  }

  function fillTable() {
    $("data-table").querySelector("tbody").replaceChildren(...history.slice().reverse().map((d) => {
      const tr = document.createElement("tr");
      tr.innerHTML = `<td>${d.t}</td><td>${d.full}</td><td>${d.drop}</td><td>${d.gate}</td>`;
      return tr;
    }));
  }

  $("chart").addEventListener("mousemove", showTooltip);
  $("chart").addEventListener("mouseleave", () => { $("tooltip").hidden = true; });
  $("table-toggle").addEventListener("click", (e) => {
    const table = $("data-table");
    table.hidden = !table.hidden;
    e.target.setAttribute("aria-pressed", String(!table.hidden));
    e.target.textContent = table.hidden ? "Show as table" : "Hide table";
    if (!table.hidden) fillTable();
  });

  // ---- Event history from the cloud ------------------------------------------

  async function loadEvents() {
    try {
      const res = await fetch(`${API}?limit=20`);
      const { events } = await res.json();
      $("api-source").textContent = `from ${API}`;
      if (!events.length) return;
      $("events-body").replaceChildren(...events.map((e) => {
        const tr = document.createElement("tr");
        const when = new Date(e.received_at).toLocaleTimeString();
        const conf = typeof e.confidence === "number" ? `${Math.round(e.confidence * 100)}%` : "-";
        tr.innerHTML = `<td>${when}</td><td>${e.event}</td><td>${conf}</td><td>${e.device}</td><td>${e.seq ?? "-"}</td>`;
        return tr;
      }));
    } catch {
      $("api-source").textContent = `cloud API not reachable at ${API}`;
    }
  }

  // ---- Controls -----------------------------------------------------------------

  document.querySelectorAll("button[data-fault], button[data-action]").forEach((b) => {
    b.addEventListener("click", async () => {
      const path = b.dataset.fault ? `/api/fault/${b.dataset.fault}` : "/api/motion";
      try {
        const res = await fetch(`${MODULE}${path}`, { method: "POST" });
        $("action-result").textContent = res.ok ? `Sent: ${b.textContent}` : `Module refused: ${res.status}`;
      } catch {
        $("action-result").textContent = "Wi-Fi module not reachable.";
      }
    });
  });

  drawChart();
  connect();
  loadEvents();
  setInterval(loadEvents, 3000);
})();
