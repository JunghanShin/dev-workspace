// 시스템 모니터(sysmon, 기본 탑재). 공통 부품(ctx.ui)으로만 그린다. 맨 위 드롭다운으로 보기를 고른다(design.md 6.11):
//   보드 전체: 요약 숫자 → CPU 코어·자원 막대 → 그래프(실시간 3분 또는 지난 기록) → 프로세스·전용 HW·온도·전력·시스템
//              → 워커 목록(워커마다 CPU·메모리·프로세스·스레드, 누르면 그 워커 보기)
//   워커:      요약(상태·CPU·메모리·루프 시간·재시작·연속 실패) → 스레드·우선순위(/workers/{id}/tasks + 워커 runtime)
//              → 단계·큐(제공 도구를 쓴 워커) → 안전망(잡은 예외·경고·재시작) → 추이(보드와 같은 기간) → 자식 프로세스
// 고른 보기는 주소에 남긴다(#/board/<id>?view=<워커 id>). 지난 기록은 /api/remote/metrics_history(logger 기록).
let els = {};
let timers = [];

const CHARTS = [
  ["CPU", "cpu_avg_pct", "%", { min: 0, max: 100, color: "#5aa9ff" }, 0, 1],
  ["GPU", "gpu_pct", "%", { min: 0, max: 100, color: "#3ccf91" }, 0, 1],
  ["온도", "temp_max_c", "°C", { min: 20, max: 100, color: "#f0b33c" }, 0, 1],
  ["전력", "power_vdd_in_mw", "W", { min: 0, color: "#ef8f5f" }, 1, 1 / 1000],
  ["네트워크 송신", "net_tx_kbps", "Mbps", { min: 0, color: "#b08cff" }, 1, 1 / 1000],
];
const ENGINES = [["NVENC", "nvenc_mhz"], ["NVDEC", "nvdec_mhz"], ["VIC", "vic_mhz"], ["NVJPG", "nvjpg_mhz"],
                 ["DLA0", "nvdla0_mhz"], ["DLA1", "nvdla1_mhz"], ["OFA", "ofa_mhz"]];
const PERIODS = [["live", "실시간 3분"], ["60", "1시간"], ["360", "6시간"], ["1440", "24시간"]];
const SLOTS = 360;  // 지난 기록을 이 칸 수로 나눠 평균(보드와 워커가 같은 시간축)
// 워커 펼침 그래프: [이름, 지표 키, 단위, spark 옵션, 소수 자리]. 워커 자원은 Manager가 잰 값(자식 프로세스 포함)
const WCHARTS = [
  ["CPU", "res.cpu_pct", "%", { min: 0, color: "#5aa9ff" }, 0],
  ["메모리", "res.rss_mb", "MB", { min: 0, color: "#b08cff" }, 0],
];

function stored(key, fallback) {
  try { const v = localStorage.getItem(key); return v === null ? fallback : v; } catch { return fallback; }
}
function store(key, value) {
  try { localStorage.setItem(key, value); } catch { /* 저장 불가 환경 */ }
}

// 지난 기록 줄 → SLOTS칸 배열(시각으로 칸을 정해 보드·워커 그래프의 시간축을 맞춘다)
function toSlots(lines, minutes) {
  const now = Date.now() / 1000, since = now - minutes * 60, span = (now - since) / SLOTS;
  const out = new Array(SLOTS).fill(null);
  for (const l of lines) {
    let r;
    try { r = JSON.parse(l); } catch { continue; }
    const i = Math.floor((Date.parse(r.t) / 1000 - since) / span);
    if (i >= 0 && i < SLOTS) out[i] = r.m;
  }
  return out;
}

function uptime(sec) {
  if (sec == null) return "—";
  const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600), m = Math.floor((sec % 3600) / 60);
  return (d ? `${d}일 ` : "") + `${h}시간 ${m}분`;
}

async function loadProcesses(ctx) {
  try {
    const lines = await ctx.remote("processes", { lines: 10 });
    els.procs.replaceChildren(...lines.map((l) => {
      const [pid, comm, cpu, mem, rss] = l.trim().split(/\s+/);
      return ctx.h("tr", {}, ctx.h("td", { class: "mono faint" }, pid), ctx.h("td", {}, comm),
        ctx.h("td", { style: "text-align:right" }, cpu + "%"), ctx.h("td", { style: "text-align:right" }, mem + "%"),
        ctx.h("td", { style: "text-align:right" }, Math.round(Number(rss) / 1024) + " MB"));
    }));
  } catch (e) {
    els.procs.replaceChildren(ctx.h("tr", {}, ctx.h("td", { class: "muted" }, "읽지 못함: " + e.message)));
  }
}

async function loadPowerModes(ctx) {
  try {
    const lines = await ctx.remote("power_modes", {});
    els.modeNames = Object.fromEntries(lines.map((l) => /ID=(\d+)\s+NAME=(\S+)/.exec(l)).filter(Boolean).map((m) => [m[1], m[2]]));
  } catch { els.modeNames = {}; }
}

// 지난 기록: 기간을 360칸으로 줄인 평균
async function loadHistory(ctx) {
  const minutes = els.period;
  if (minutes === "live") { els.hist = null; drawCharts(ctx); drawWorkerView(ctx); return; }
  els.histNote.textContent = "기록 읽는 중";
  try {
    const keys = CHARTS.map((c) => c[1]).join(",");
    const lines = await ctx.remote("metrics_history", { id: ctx.workerId, minutes, points: 360, keys });
    els.hist = toSlots(lines, Number(minutes));
    els.histNote.textContent = els.hist.some(Boolean) ? `지난 ${Number(minutes) / 60}시간 · ${SLOTS}칸 평균` : "기록 없음";
  } catch (e) {
    els.hist = [];
    els.histNote.textContent = "기록을 읽지 못함: " + e.message;
  }
  drawCharts(ctx);
  if (els.view) loadWorkerHistory(ctx, els.view);
}

function drawCharts(ctx) {
  const { fmt, spark } = ctx;
  const live = !els.hist;
  const series = live ? ctx.history(ctx.workerId).map((s) => s.metrics || {}) : els.hist.map((m) => m || {});
  for (const [, key, unit, opt, digits, scale] of CHARTS) {
    const vals = series.map((m) => (m[key] != null ? m[key] * scale : null));
    spark(els.canvas[key].c, vals, opt.max ? opt : { ...opt });
    const nums = vals.filter((v) => v != null);
    els.canvas[key].v.textContent = !nums.length ? "—" : live
      ? fmt(nums[nums.length - 1], digits) + " " + unit
      : `평균 ${fmt(nums.reduce((a, b) => a + b, 0) / nums.length, digits)} · 최대 ${fmt(Math.max(...nums), digits)} ${unit}`;
  }
  if (live) els.histNote.textContent = "1초 간격 · 이 화면을 연 뒤부터";
}

// ---- 워커 목록(보드 전체 보기) -------------------------------------------------------------------

async function loadWorkerHistory(ctx, id) {
  if (els.period === "live") return;
  const minutes = Number(els.period);
  try {
    const lines = await ctx.remote("metrics_history", { id, minutes, points: SLOTS });
    els.whist[id] = toSlots(lines, minutes);
  } catch { els.whist[id] = new Array(SLOTS).fill(null); }
  drawWorkerView(ctx);
}

// 워커 줄은 한 번 만들고 값만 바꾼다
function buildWorkers(ctx, list) {
  const { h, ui } = ctx;
  // libworker 버전은 READY 뒤에 알게 되므로 목록 서명에 넣는다(바뀌면 줄을 다시 만듦)
  const sig = list.map((x) => `${x.id}:${x.libworker || ""}:${Object.values(x.peers || {}).join("+")}`).join(",");
  if (sig === els.wsig) return;
  els.wsig = sig;
  els.wrows = {};
  const rows = [];
  for (const x of list) {
    const id = x.id;
    const r = { cells: {} };
    for (const k of ["restart", "cpu", "rss", "procs", "threads", "err"]) r.cells[k] = h("td", k === "err" ? { class: "err" } : {});
    r.pill = ui.workerPill(x.actual_state);
    r.loop = ui.pill(null, "", "idle");
    r.loop.style.display = "none";
    r.controls = ui.workerControls(ctx, id, { autostart: true });
    r.dlg = els.workerDlgs[id] || (els.workerDlgs[id] = ui.workerSettings(ctx, id, { title: `${id} 설정` }));
    const go = ui.iconButton({ icon: "right", title: "이 워커 보기(스레드·우선순위·큐·안전망·그래프)", onClick: () => setView(ctx, id) });
    const cfgBtn = ui.settingsButton(r.dlg, `${id} 설정`);
    const name = h("a", { href: "#", title: "이 워커 보기", onclick: (e) => { e.preventDefault(); setView(ctx, id); } }, id);
    r.tr = h("tr", {},
      h("td", {}, h("span", { style: "display:inline-flex; align-items:center; gap:6px" }, go, name)),
      h("td", { class: "muted", title: x.libworker ? `libworker ${x.libworker}(워커가 READY로 알린 계약 버전)` : "" },
        x.type, x.libworker ? h("span", { class: "faint" }, ` · lw ${x.libworker}`) : null,
        Object.keys(x.peers || {}).length ? h("div", { class: "faint", title: "설정이 가리키는 다른 워커(워커 간 통신)" },
          "→ " + [...new Set(Object.values(x.peers))].join(", ")) : null), h("td", {}, r.pill, " ", r.loop),
      r.cells.restart, r.cells.cpu, r.cells.rss, r.cells.procs, r.cells.threads, r.cells.err,
      h("td", { style: "text-align:right; white-space:nowrap" }, cfgBtn, " ", r.controls));
    els.wrows[id] = r;
    rows.push(r.tr);
  }
  els.workers.replaceChildren(...rows);
}

// ---- 보기 고르기 -----------------------------------------------------------------------------------

function viewFromHash() {
  const q = location.hash.split("?")[1] || "";
  return new URLSearchParams(q).get("view") || "";
}

function setView(ctx, id) {
  els.view = id || "";
  const base = location.hash.split("?")[0];
  history.replaceState(null, "", base + (els.view ? `?view=${encodeURIComponent(els.view)}` : ""));
  els.viewSel.value = els.view;
  els.boardView.hidden = !!els.view;
  els.workerView.hidden = !els.view;
  if (els.view) {
    els.wv.title.textContent = els.view;
    els.wv.ksig = "";
    els.wv.runtime = null;
    els.wv.procs = [];
    renderWorkerView(ctx);
    loadWorkerRuntime(ctx);
    loadWorkerTasks(ctx);
    loadWorkerHistory(ctx, els.view);
  }
  if (els.lastState) updateViewOptions(ctx, els.lastState);
}

// 드롭다운: 보드 전체 + 워커(오류·재시작 중 먼저, 나머지 이름순). 상태는 글자 점으로
const DOT = { running: "●", starting: "◐", exited: "▲", error: "▲", stopped: "○", disabled: "○" };
function updateViewOptions(ctx, state) {
  const list = state.workers.filter((x) => !x.builtin);
  const bad = (x) => (x.crash_loop || x.actual_state === "error" || x.actual_state === "exited" ? 0 : 1);
  list.sort((a, b) => bad(a) - bad(b) || a.id.localeCompare(b.id));
  const opts = [["", "보드 전체"], ...list.map((x) => [x.id, `${DOT[x.actual_state] || "·"} ${x.id}${x.crash_loop ? " (연속 실패)" : ""}`])];
  const sig = opts.map((o) => o.join(":")).join(",");
  if (sig !== els.viewSig) {
    els.viewSig = sig;
    els.viewSel.set(opts, els.view);
  }
}

// ---- 워커 보기 -------------------------------------------------------------------------------------

async function loadWorkerRuntime(ctx) {
  const id = els.view;
  const w = id && ctx.state().byId[id];
  if (!w || w.actual_state !== "running") { els.wv.runtime = null; renderWorkerView(ctx); return; }
  try {
    const st = await ctx.api("POST", `/workers/${id}/command`, { cmd: "get_status" });
    if (els.view !== id) return;
    els.wv.runtime = st && st.runtime ? st.runtime : null;
    els.wv.ages = (st && st.threads) || {};
  } catch { els.wv.runtime = null; }
  renderWorkerView(ctx);
}

async function loadWorkerTasks(ctx) {
  const id = els.view;
  const w = id && ctx.state().byId[id];
  if (!w || w.actual_state !== "running" || !w.pid) { els.wv.procs = []; renderWorkerView(ctx); return; }
  try {
    const r = await ctx.api("GET", `/workers/${id}/tasks?pid=${w.pid}`);
    if (els.view !== id) return;
    els.wv.procs = r.processes || [];
  } catch { els.wv.procs = []; }
  renderWorkerView(ctx);
}

const PRIO = (t) => (t.policy && t.policy.startsWith("realtime") ? `실시간 ${t.rt_priority}` : t.policy === "normal" ? (t.nice ? `보통 (nice ${t.nice})` : "보통") : t.policy || "—");

function renderWorkerView(ctx) {
  const { h, fmt, ui } = ctx;
  const v = els.wv;
  const id = els.view;
  if (!id) return;
  const w = ctx.state().byId[id];
  const rt = v.runtime;
  // 요약
  v.state.set(w ? w.actual_state : null);
  v.crash.style.display = w && w.crash_loop ? "" : "none";
  const res = (w && w.resources) || {};
  v.t.cpu.set(res.cpu_pct, 1);
  v.t.mem.set(res.rss_mb, 0);
  v.t.loop.set(rt ? rt.loop.max_ms : (w && w.metrics ? w.metrics.host_loop_ms : null), 2);
  v.t.restart.set(w ? w.restart_count : null, 0);
  v.t.threads.set(res.threads, 0);
  // 스레드·우선순위: 보드에서 읽은 실제 값(/tasks) + 워커가 요청한 값(runtime 단계) + 마지막 beat(감시 중인 스레드)
  const main = v.procs.find((p) => w && p.pid === w.pid) || v.procs[0];
  const requested = {};
  for (const st of Object.values((rt && rt.stages) || {}))
    if (st.thread) requested[st.thread.name] = st.thread;
  const ages = v.ages || {};
  const threadRows = main ? main.threads.map((t) => {
    const req = requested[t.name];
    const reqText = req ? (req.requested.priority === "realtime" ? `실시간 ${req.requested.rt_priority}` : req.requested.nice ? `nice ${req.requested.nice}` : "보통") +
      (req.applied ? "" : " (적용 안 됨)") : "—";
    return h("tr", {}, h("td", { class: "mono" }, t.name), h("td", {}, PRIO(t)), h("td", { class: req && !req.applied ? "err" : "muted" }, reqText),
      h("td", { class: "mono" }, t.cpus || "—"), h("td", { style: "text-align:right" }, t.cpu == null ? "—" : fmt(t.cpu, 1, "%")),
      h("td", { style: "text-align:right" }, ages[t.name] != null ? `${ages[t.name]} ms` : "—"));
  }) : [h("tr", {}, h("td", { colspan: 6, class: "hint" }, w && w.actual_state === "running" ? "읽는 중" : "실행 중이 아님"))];
  v.threads.replaceChildren(...threadRows);
  // 단계·큐(제공 도구를 쓴 워커)
  const stages = Object.entries((rt && rt.stages) || {});
  v.stagesCard.hidden = !stages.length;
  v.stages.replaceChildren(...stages.map(([name, st]) => {
    const pct = st.queue ? (st.size / st.queue) * 100 : 0;
    const m = ui.meter("");
    m.set(pct, `${st.size} / ${st.queue}`);
    return h("tr", {}, h("td", { class: "mono" }, name), h("td", { class: "muted" }, st.kind === "input" ? "입력" : "출력"),
      h("td", { style: "min-width:140px" }, m), h("td", { style: "text-align:right" }, String(st.high_water)),
      h("td", { style: "text-align:right" }, fmt(st.lag_ms, 1, " ms")), h("td", { style: "text-align:right" }, String(st.dropped)),
      h("td", { style: "text-align:right", class: st.errors ? "err" : "" }, String(st.errors)),
      h("td", { class: "muted" }, `${st.overflow} · 대기 ${st.wait_ms} ms`));
  }));
  // 안전망
  v.safety.set([
    ["상태", w ? `${w.actual_state}${w.crash_loop ? " · 연속 실패" : ""}${w.restart_in_s != null ? ` · ${w.restart_in_s}초 뒤 다시 시작` : ""}` : "—"],
    ["재시작", w ? `${w.restart_count}회${w.last_error ? " · 마지막 오류: " + w.last_error : ""}` : "—"],
    ["잡은 예외", rt ? `${rt.errors}건` : "—"],
    ["루프 시간", rt ? `최근 ${fmt(rt.loop.last_ms, 2)} ms · 1초 최대 ${fmt(rt.loop.max_ms, 2)} ms · 예산 ${rt.loop.budget_ms} ms (넘김 ${rt.loop.budget_overruns})` : "—"],
    ["멈춤 판정", rt ? `스레드 ${rt.loop.thread_stall_sec} s · 워치독 ${rt.loop.watchdog_sec} s` : "—"],
    ["프로세스 우선순위", w && w.process ? Object.entries(w.process).map(([k, x]) => `${k} ${x}`).join(" · ") : "기본"],
    ["경고", rt && rt.warnings.length ? rt.warnings.slice(-3).join(" / ") : "없음"],
  ]);
  // 자식 프로세스
  const kids = v.procs.filter((p) => !w || p.pid !== w.pid);
  v.kidsCard.hidden = !kids.length;
  v.kids.replaceChildren(...kids.map((p) => h("tr", {}, h("td", { class: "mono faint" }, p.pid), h("td", {}, p.name),
    h("td", { style: "text-align:right" }, fmt(p.cpu, 1, "%")), h("td", { style: "text-align:right" }, fmt(p.rss_mb, 0, " MB")),
    h("td", { style: "text-align:right" }, String(p.thread_count)))));
  drawWorkerView(ctx);
}

function drawWorkerView(ctx) {
  const { fmt, spark, h } = ctx;
  const id = els.view;
  if (!id) return;
  const v = els.wv;
  const live = els.period === "live";
  const series = live
    ? ctx.history(id).map((s) => {
        const m = { ...(s.metrics || {}) };
        for (const [k, x] of Object.entries(s.resources || {})) m["res." + k] = x;
        return m;
      })
    : (els.whist[id] || []).map((m) => m || {});
  const keys = [...new Set(series.flatMap((m) => Object.keys(m)
    .filter((k) => typeof m[k] === "number" && !k.startsWith("res.") && k !== "restart_count")))].sort();
  const ksig = keys.join(",");
  if (v.ksig !== ksig) {
    v.ksig = ksig;
    const want = stored(`workspace.board.metric.${id}`, "") || keys.find((k) => /fps/.test(k)) || keys.find((k) => !k.startsWith("host_")) || keys[0] || "";
    v.select.replaceChildren(...(keys.length ? keys : ["(지표 없음)"]).map((k) => h("option", { value: k }, k)));
    if (keys.includes(want)) v.select.value = want;
  }
  const draw = (ch, key, unit, opt, digits) => {
    const vals = series.map((m) => (typeof m[key] === "number" ? m[key] : null));
    spark(ch.c, vals, opt);
    const nums = vals.filter((x) => x != null);
    ch.v.textContent = !nums.length ? "—" : live ? `${fmt(nums[nums.length - 1], digits)} ${unit}`
      : `평균 ${fmt(nums.reduce((a, b) => a + b, 0) / nums.length, digits)} · 최대 ${fmt(Math.max(...nums), digits)} ${unit}`;
  };
  draw(v.charts.cpu, "res.cpu_pct", "%", { min: 0, color: "#5aa9ff" }, 0);
  draw(v.charts.mem, "res.rss_mb", "MB", { min: 0, color: "#b08cff" }, 0);
  draw(v.charts.loop, "host_loop_ms", "ms", { min: 0, color: "#f0b33c" }, 2);
  draw(v.charts.metric, v.select.value, "", { min: 0, color: "#3ccf91" }, 1);
}

function buildWorkerView(ctx) {
  const { h, ui } = ctx;
  const v = {};
  v.title = h("span", { class: "card2-t", style: "margin:0" });
  // 기간(보드 전체 보기의 추이와 같은 값)
  v.period = h("select", { onchange: (e) => { els.period = e.target.value; els.periodSel.value = els.period; loadHistory(ctx); loadWorkerHistory(ctx, els.view); } },
    ...PERIODS.map(([val, t]) => h("option", { value: val }, t)));
  v.state = ui.workerPill(null);
  v.crash = ui.pill("alert", "연속 실패", "bad");
  v.crash.style.display = "none";  // pill은 display를 정해 hidden 속성이 안 먹음
  v.t = { cpu: ui.tile("CPU", "%"), mem: ui.tile("메모리", "MB"), loop: ui.tile("루프 시간(1초 최대)", "ms"),
          restart: ui.tile("재시작"), threads: ui.tile("스레드") };
  v.threads = h("tbody");
  v.stages = h("tbody");
  v.safety = ui.kvList();
  v.kids = h("tbody");
  const chart = () => ({ c: h("canvas"), v: h("span", { style: "color:var(--text); font-weight:600" }) });
  v.charts = { cpu: chart(), mem: chart(), loop: chart(), metric: chart() };
  v.select = h("select", { title: "볼 지표", style: "margin-left:6px; max-width:120px" });
  v.select.addEventListener("change", () => { store(`workspace.board.metric.${els.view}`, v.select.value); drawWorkerView(ctx); });
  const box = (label, ch, extra) => h("div", { class: "chart" }, h("div", { class: "k" }, h("span", {}, label, extra || null), ch.v), ch.c);
  v.stagesCard = ui.card("단계 · 큐 (제공 도구)", h("table", {},
    h("thead", {}, h("tr", {}, ["단계", "종류", "채움", "최대", "밀림", "버림", "오류", "넘칠 때 · 대기 상한"].map((t) => h("th", {}, t)))), v.stages));
  v.kidsCard = ui.card("자식 프로세스", h("table", {},
    h("thead", {}, h("tr", {}, ["PID", "프로세스", "CPU", "RSS", "스레드"].map((t) => h("th", {}, t)))), v.kids));
  v.root = h("div", {},
    h("div", { class: "ph-sub", style: "margin:4px 0 10px" }, v.title, " ", v.state, " ", v.crash),
    ui.tiles(...Object.values(v.t)),
    h("div", { style: "margin-top:12px" }, ui.card("스레드 · 우선순위 (보드에서 읽은 실제 값)", h("table", {},
      h("thead", {}, h("tr", {}, ["스레드", "우선순위", "워커 요청", "CPU 고정", "사용률", "마지막 beat"].map((t) => h("th", {}, t)))), v.threads))),
    h("div", { style: "margin-top:12px" }, v.stagesCard),
    h("div", { class: "cols2", style: "margin-top:12px" },
      ui.card("안전망", v.safety),
      ui.card(null, h("div", { class: "ph-sub", style: "margin-bottom:10px" }, h("span", { class: "card2-t", style: "margin:0" }, "추이"),
          h("span", { class: "spacer" }), v.period),
        h("div", { class: "charts", style: "grid-template-columns: repeat(auto-fit, minmax(150px, 1fr))" },
          box("CPU", v.charts.cpu), box("메모리", v.charts.mem), box("루프 시간", v.charts.loop), box("지표", v.charts.metric, v.select)))),
    h("div", { style: "margin-top:12px" }, v.kidsCard));
  return v;
}

async function exportCsv(ctx) {
  const minutes = els.period === "live" ? "60" : els.period;
  const lines = await ctx.remote("metrics_history", { id: ctx.workerId, minutes, points: 0, csv: "1" });
  const stamp = new Date().toISOString().slice(0, 16).replace(/[-:T]/g, "");
  ctx.ui.download(`${ctx.workerId}_${minutes}min_${stamp}.csv`, lines.join("\n") + "\n");
}

export default {
  mount(el, ctx) {
    const { h, ui, act } = ctx;
    const id = ctx.workerId;
    els = { canvas: {}, cores: {}, period: "live", hist: null, modeNames: {}, workerDlgs: {},
            wrows: {}, whist: {}, view: "" };
    // 보기: 보드 전체 또는 워커 하나(드롭다운, 주소 ?view=)
    els.viewSel = ui.select([["", "보드 전체"]], "", (val) => setView(ctx, val), "보기: 보드 전체 또는 워커 하나");
    els.viewSel.style.marginLeft = "10px";
    els.wv = buildWorkerView(ctx);
    els.pill = ui.workerPill(null);
    els.controls = ui.workerControls(ctx, id);
    els.settingsDlg = ui.workerSettings(ctx, id, { title: "시스템 모니터 설정" });
    const settingsBtn = ui.settingsButton(els.settingsDlg, "설정");

    // 요약 숫자
    els.t = { cpu: ui.tile("CPU", "%"), gpu: ui.tile("GPU", "%"), ram: ui.tile("메모리", "GB"),
              temp: ui.tile("최고 온도", "°C"), power: ui.tile("전력", "W"), net: ui.tile("네트워크 ↓ / ↑", "Mbps") };

    // CPU 코어 막대(코어 수는 첫 지표에서 정해짐), 자원 막대
    els.coreBox = h("div");
    els.m = { gpu: ui.meter("GPU"), ram: ui.meter("메모리"), swap: ui.meter("SWAP"), emc: ui.meter("EMC"),
              disk: ui.meter("디스크"), fan: ui.meter("팬") };

    // 그래프 + 기간
    els.charts = h("div", { class: "charts" });
    for (const [label, key] of CHARTS) {
      const c = h("canvas");
      els.canvas[key] = { c, v: h("span", { style: "color:var(--text); font-weight:600" }) };
      els.charts.append(h("div", { class: "chart" }, h("div", { class: "k" }, h("span", {}, label), els.canvas[key].v), c));
    }
    els.periodSel = h("select", { onchange: (e) => { els.period = e.target.value; els.wv.period.value = els.period; loadHistory(ctx); if (els.view) loadWorkerHistory(ctx, els.view); } },
      ...PERIODS.map(([v, t]) => h("option", { value: v }, t)));
    els.histNote = h("span", { class: "hint" });
    const csvBtn = ui.iconButton({ icon: "external", title: "기록 내려받기(CSV, 선택한 기간, 1초 간격. 실시간이면 1시간)",
      onClick: () => act("기록 내려받기", () => exportCsv(ctx)) });

    // 아래 판들
    els.procs = h("tbody");
    els.engines = ui.kvList();
    els.temps = ui.kvList();
    els.power = ui.kvList();
    els.sys = ui.kvList();

    const trend = ui.card(null,
        h("div", { class: "ph-sub", style: "margin-bottom:10px" }, h("span", { class: "card2-t", style: "margin:0" }, "추이"),
          h("span", { class: "spacer" }), els.histNote, els.periodSel, csvBtn),
        els.charts);
    els.workerView = h("div", { hidden: true }, els.wv.root);
    els.boardView = h("div", {},
      ui.tiles(...Object.values(els.t)),
      h("div", { class: "cols2", style: "margin-top:12px" },
        ui.card("CPU 코어", els.coreBox),
        ui.card("자원", ...Object.values(els.m))),
      h("div", { style: "margin-top:12px" }, trend),
      h("div", { class: "cols2", style: "margin-top:12px" },
        ui.card("프로세스 (CPU 순, 3초마다)", h("table", {},
          h("thead", {}, h("tr", {}, ["PID", "이름", "CPU", "메모리", "RSS"].map((t) => h("th", {}, t)))), els.procs)),
        ui.card("시스템", els.sys)),
      h("div", { class: "cols3", style: "margin-top:12px" },
        ui.card("전용 하드웨어", els.engines),
        ui.card("온도", els.temps),
        ui.card("전력 레일", els.power)),
      // 전체 워커: 상태·자원(워커와 자식 프로세스 합)·오류, 설정, 자동 시작·시작/정지. 시스템 모니터 자신은 빼고.
      // 워커 이름(또는 앞 버튼)을 누르면 그 워커 보기
      h("div", { style: "margin-top:12px" }, ui.card("워커", h("table", {},
        h("thead", {}, h("tr", {}, ["워커", "type", "상태", "재시작", "CPU", "메모리", "프로세스", "스레드", "마지막 오류", ""].map((t) => h("th", {}, t)))),
        els.workers = h("tbody")))));
    el.append(
      ui.panelHead({ title: "시스템 모니터", badges: [els.viewSel, els.pill],
        actions: [settingsBtn, h("span", { class: "ph-sep" }), els.controls] }),
      els.boardView,
      els.workerView);

    loadPowerModes(ctx);
    loadProcesses(ctx);
    const v0 = viewFromHash();
    if (v0) setView(ctx, v0);
    timers.push(setInterval(() => { if (!els.view) loadProcesses(ctx); else loadWorkerTasks(ctx); }, 3000),
      setInterval(() => { if (els.view) loadWorkerRuntime(ctx); }, 2000),
      setInterval(() => { if (els.period !== "live") { loadHistory(ctx); if (els.view) loadWorkerHistory(ctx, els.view); } }, 60000));
  },

  update(state, ctx) {
    const { h, fmt, ui } = ctx;
    const w = state.byId[ctx.workerId];
    if (!w) return;
    const m = w.metrics || {};
    els.pill.set(w.actual_state);
    els.controls.update(w);
    els.lastState = state;
    updateViewOptions(ctx, state);
    const list = state.workers.filter((x) => !x.builtin);
    buildWorkers(ctx, list);
    for (const x of list) {
      const r = els.wrows[x.id];
      if (!r) continue;
      const res = x.resources || {};
      r.pill.set(x.actual_state);
      r.controls.update(x);
      r.dlg.update(x);
      r.cells.restart.textContent = x.restart_count;
      r.cells.cpu.textContent = fmt(res.cpu_pct, 0, "%");
      r.cells.rss.textContent = fmt(res.rss_mb, 0, " MB");
      r.cells.procs.textContent = res.procs ?? "—";
      r.cells.threads.textContent = res.threads ?? "—";
      r.cells.err.textContent = x.last_error || "";
      r.loop.style.display = x.crash_loop ? "" : "none";
      if (x.crash_loop) r.loop.set("alert", "연속 실패", "bad");
    }
    if (els.view) renderWorkerView(ctx);
    els.settingsDlg.update(w);

    els.t.cpu.set(m.cpu_avg_pct, 0);
    els.t.gpu.set(m.gpu_pct, 0);
    els.t.ram.set(m.ram_used_mb != null ? `${fmt(m.ram_used_mb / 1024, 1)} / ${fmt(m.ram_total_mb / 1024, 0)}` : null);
    els.t.temp.set(m.temp_max_c, 0);
    els.t.power.set(m.power_vdd_in_mw != null ? m.power_vdd_in_mw / 1000 : null, 1);
    els.t.net.set(m.net_rx_kbps != null ? `${fmt(m.net_rx_kbps / 1000, 1)} / ${fmt(m.net_tx_kbps / 1000, 1)}` : null);

    // 코어별 막대
    const cores = Object.keys(m).map((k) => /^cpu(\d+)_pct$/.exec(k)).filter(Boolean).map((r) => Number(r[1])).sort((a, b) => a - b);
    const sig = cores.join(",");
    if (sig !== els.coreSig) {
      els.coreSig = sig;
      els.cores = {};
      els.coreBox.replaceChildren(...cores.map((i) => (els.cores[i] = ui.meter(`코어 ${i}`))));
      if (!cores.length) els.coreBox.replaceChildren(h("span", { class: "muted" }, "코어별 지표 없음(sysmon 갱신 전)"));
    }
    for (const i of cores) els.cores[i].set(m[`cpu${i}_pct`], `${m[`cpu${i}_pct`]}% · ${m[`cpu${i}_mhz`]} MHz`);

    els.m.gpu.set(m.gpu_pct, m.gpu_pct != null ? `${m.gpu_pct}%${m.gpu_mhz != null ? " · " + m.gpu_mhz + " MHz" : ""}` : "—");
    els.m.ram.set(m.ram_total_mb ? (m.ram_used_mb / m.ram_total_mb) * 100 : null,
      m.ram_total_mb ? `${fmt(m.ram_used_mb / 1024, 1)} / ${fmt(m.ram_total_mb / 1024, 1)} GB` : "—");
    els.m.swap.set(m.swap_total_mb ? (m.swap_used_mb / m.swap_total_mb) * 100 : null,
      m.swap_total_mb ? `${fmt(m.swap_used_mb / 1024, 1)} / ${fmt(m.swap_total_mb / 1024, 1)} GB` : "—");
    els.m.emc.set(m.emc_pct, m.emc_pct != null ? `${m.emc_pct}%` : "—");
    els.m.disk.set(m.disk_total_gb ? (m.disk_used_gb / m.disk_total_gb) * 100 : null,
      m.disk_total_gb ? `${fmt(m.disk_used_gb, 0)} / ${fmt(m.disk_total_gb, 0)} GB` : "—");
    els.m.fan.set(m.fan_pct, m.fan_pct != null ? `${m.fan_pct}%${m.fan_rpm != null ? " · " + m.fan_rpm + " rpm" : ""}` : "—");

    if (!els.hist) drawCharts(ctx);

    els.sys.set([
      ["전원 모드", m.power_mode != null ? `${els.modeNames[String(m.power_mode)] || "모드"} (${m.power_mode})` : "—"],
      ["CPU 코어", m.cpu_online != null ? `${m.cpu_online}개 사용 중 · 최고 ${m.cpu_mhz_max} MHz` : "—"],
      ["가동 시간", uptime(m.uptime_s)],
      ["수집 방식", m.collector_root === 1 ? "root 수집기(전 항목)" : m.collector_root === 0 ? "직접 실행(EMC·전용 HW 없음)" : "—"],
    ]);
    els.engines.set(ENGINES.map(([label, key]) => [label, m[key] === undefined ? "—" : m[key] ? fmt(m[key], 0, " MHz") : "꺼짐"]));
    const rows = (prefix, suffix, conv) => Object.keys(m)
      .filter((k) => k.startsWith(prefix) && k.endsWith(suffix) && k !== "temp_max_c").sort()
      .map((k) => [k.slice(prefix.length, k.length - suffix.length), conv(m[k])]);
    els.temps.set(rows("temp_", "_c", (v) => fmt(v, 1, " °C")));
    els.power.set(rows("power_", "_mw", (v) => fmt(v / 1000, 2, " W")));
  },

  unmount() {
    timers.forEach(clearInterval); timers = [];
    if (els.settingsDlg) els.settingsDlg.dispose();
    for (const d of Object.values(els.workerDlgs || {})) d.dispose();
    els = {};
  },
};
