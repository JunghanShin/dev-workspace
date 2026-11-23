// 자동 상태 화면: 자기 화면(workspace/panel)이 없는 워커를 워커 계약(io/self.json, 워크스페이스 design.md 6.6.7.1)대로 그린다.
// 틀(shell.js)이 자기 패널이 없는 워커마다 왼쪽 목록에 "상태(자동)" 항목을 만들고 이 패널을 그 워커로 붙인다.
// panel.json이 없는 것은 일부러다: 공용 패널 목록에 따로 나오지 않게.
//
//   지표: 계약 metrics(+ include 조각)의 이름·단위·설명 → 값은 heartbeat 지표(1초, 워커 목록 상태)
//   상태: 계약 status의 모듈 칸 → 값은 get_status(돌고 있을 때 1초마다). 칸 설명·단위는 계약에서
//   계약이 없으면 값만(이름 그대로) 보인다. 읽기만 한다(명령 버튼 없음). 공통 부품(ctx.ui)으로만 그린다
let els = {};
let timer = null;

const STATUS_MS = 1000;

// 계약의 지표 표: {이름: {type, unit, description}}. 이름에 <…> 자리가 있으면 정규식으로
function metricSpecs(io) {
  const out = [];
  const add = (m) => {
    for (const [name, spec] of Object.entries((m && m.metrics) || {})) {
      const rx = name.includes("<") ? new RegExp("^" + name.split(/<[^<>]+>/).map((p) => p.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join("[a-z0-9_]+") + "$") : null;
      out.push({ name, rx, spec: spec || {} });
    }
  };
  if (io) {
    add(io.self);
    for (const f of Object.values(io.includes || {})) add(f);
  }
  return out;
}

function findSpec(specs, name) {
  return specs.find((s) => !s.rx && s.name === name) || specs.find((s) => s.rx && s.rx.test(name)) || null;
}

// 계약의 상태 모듈 칸: {모듈: JSON Schema}
function statusSchemas(io) {
  const out = {};
  if (io) {
    Object.assign(out, (io.self && io.self.status) || {});
    for (const f of Object.values(io.includes || {})) Object.assign(out, f.status || {});
  }
  return out;
}

// 값 하나를 글자로: 숫자는 소수 둘째 자리까지, 배열·객체는 짧게
function text(v, unit) {
  if (v === null || v === undefined) return "—";
  let s;
  if (typeof v === "number") s = Number.isInteger(v) ? String(v) : String(Math.round(v * 100) / 100);
  else if (typeof v === "boolean") s = v ? "예" : "아니오";
  else if (typeof v === "object") s = JSON.stringify(v).slice(0, 120);
  else s = String(v);
  return unit && typeof v === "number" ? `${s} ${unit}` : s;
}

// 상태 값을 "a.b.c" 줄로 펼친다. 단위·설명은 스키마에서(없으면 이름만). 배열은 줄 하나
function flatten(value, schema, prefix, rows) {
  const props = (schema && schema.properties) || {};
  if (value && typeof value === "object" && !Array.isArray(value)) {
    for (const [k, v] of Object.entries(value)) {
      const sub = props[k] || (schema && typeof schema.additionalProperties === "object" ? schema.additionalProperties : null);
      flatten(v, sub, prefix ? `${prefix}.${k}` : k, rows);
    }
    return;
  }
  const label = schema && schema.description ? `${prefix} · ${schema.description}` : prefix;
  rows.push([label, text(value, schema && schema.unit)]);
}

function renderMetrics(ctx, w) {
  const m = (w && w.metrics) || {};
  const names = Object.keys(m).sort();
  if (!names.length) {
    els.metrics.replaceChildren(ctx.h("span", { class: "muted" }, w && w.actual_state === "running" ? "지표 기다리는 중" : "워커가 돌고 있지 않음"));
    return;
  }
  els.metrics.replaceChildren(ctx.ui.kvList(names.map((n) => {
    const s = findSpec(els.specs, n);
    const label = s && s.spec.description ? `${n} · ${s.spec.description}` : n;
    return [label, text(m[n], s && s.spec.unit)];
  })));
}

async function refreshStatus(ctx) {
  const w = ctx.state().byId[ctx.workerId];
  if (!els.status) return;
  if (!w || w.actual_state !== "running") {
    els.status.replaceChildren(ctx.h("span", { class: "muted" }, "워커가 돌고 있지 않음"));
    return;
  }
  let d;
  try {
    d = await ctx.api("POST", `/workers/${ctx.workerId}/command`, { cmd: "get_status", args: {} });
  } catch (e) {
    els.status.replaceChildren(ctx.h("span", { class: "muted" }, "상태를 읽지 못함: " + e.message));
    return;
  }
  if (!els.status) return;  // 패널을 떠남
  const schemas = statusSchemas(els.io);
  const skip = new Set(["id", "config_version", "runtime", "threads", "sim"]);  // 공통 겉모양(속사정은 시스템 모니터 워커 보기에)
  const modules = Object.keys(d || {}).filter((k) => !skip.has(k)).sort();
  els.status.replaceChildren(...modules.map((mod) => {
    const rows = [];
    flatten(d[mod], schemas[mod] || null, "", rows);
    return ctx.ui.card(mod, ctx.ui.kvList(rows.length ? rows : [["(비어 있음)", ""]]));
  }));
}

export default {
  async mount(el, ctx) {
    const { h, ui } = ctx;
    const id = ctx.workerId;
    els = { specs: [], io: null };
    els.pill = ui.workerPill(null);
    els.controls = ui.workerControls(ctx, id);
    els.settings = ui.workerSettings(ctx, id, { title: `${id} 설정` });
    els.note = h("div", { class: "muted" }, "계약 읽는 중…");
    els.metrics = h("div");
    els.status = h("div", { class: "stack" });
    el.appendChild(h("div", { class: "stack" },
      ui.panelHead({ title: id, badges: [els.pill], actions: [ui.settingsButton(els.settings, `${id} 설정`), h("span", { class: "ph-sep" }), els.controls] }),
      els.note,
      ui.card("지표 (heartbeat, 1초)", els.metrics),
      ui.card("상태 (get_status, 모듈별)", els.status)));
    try {
      els.io = await ctx.api("GET", `/workers/${id}/io`);
      els.specs = metricSpecs(els.io);
      els.note.textContent = els.io && els.io.self
        ? "이 화면은 워커 계약(io/self.json)으로 자동으로 그린 것입니다. 워커가 자기 화면(workspace/panel)을 가지면 그것이 대신 나옵니다."
        : "이 워커는 워커 정의(io/self.json)가 없어 값만 이름 그대로 보입니다.";
    } catch (e) {
      els.note.textContent = "이 워커는 계약(io.json)이 없어 값만 이름 그대로 보입니다.";
    }
    renderMetrics(ctx, ctx.state().byId[id]);
    refreshStatus(ctx);
    timer = setInterval(() => refreshStatus(ctx), STATUS_MS);
  },

  update(state, ctx) {
    if (!els.pill) return;
    const w = state.byId[ctx.workerId];
    els.pill.set(w && w.actual_state);
    els.controls.update(w);
    els.settings.update(w);
    renderMetrics(ctx, w);
  },

  unmount() {
    if (timer) clearInterval(timer);
    timer = null;
    if (els.settings) els.settings.dispose();
    els = {};
  },
};
