// 연결 화면(워크스페이스 design.md 6.6.7.4, docs/command-path.md 4.1): 워커끼리 누가 누구 데이터를 받는지 그림으로 보고,
// 입력마다 상대를 고른다(연결 선언). 명령하는 곳(commanders)은 보기만(파일로).
//
//   그림   워커 = 상자, 선 = 입력 연결(상대 → 받는 워커, 선 위에 입력 이름). 실선 = 이어짐, 빨간 점선 = 문제, 회색 점선 = 설정(프리셋)
//   표     입력마다 [상대 고르기: 자동 | 내보내는 워커들], 지금 상태(방법·문제). 저장하면 PUT /connections(개발 모드에서만)
//
// 값: GET /connections(process-manager get_connections: connections·inputs·check·errors), 워커 목록, 워커마다 GET /workers/{id}/io(쓰는 정의·내보내는 역할).
// 서버에 화면 전용 기능을 더하지 않는다(있는 경로만)
const SVG = "http://www.w3.org/2000/svg";
let els = {};
let data = { conn: null, ios: {}, workers: [] };

const HOW_KO = { declared: "선언", auto: "자동", config: "설정" };

function svgEl(tag, attrs = {}, text) {
  const e = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (text !== undefined) e.textContent = text;
  return e;
}

// 깊이(왼쪽 → 오른쪽): 입력이 없는 워커가 0, 받는 워커는 상대보다 한 칸 오른쪽
function layout(ids, edges) {
  const depth = Object.fromEntries(ids.map((id) => [id, 0]));
  for (let i = 0; i < ids.length; i++) {
    let moved = false;
    for (const e of edges) {
      if (e.from in depth && e.to in depth && depth[e.to] < depth[e.from] + 1 && depth[e.from] < ids.length) {
        depth[e.to] = depth[e.from] + 1;
        moved = true;
      }
    }
    if (!moved) break;
  }
  const cols = {};
  for (const id of ids) (cols[depth[id]] = cols[depth[id]] || []).push(id);
  const pos = {};
  const W = 150, H = 44, GX = 90, GY = 22;
  let maxRows = 0;
  for (const [c, list] of Object.entries(cols)) {
    list.sort();
    list.forEach((id, r) => { pos[id] = { x: 20 + Number(c) * (W + GX), y: 20 + r * (H + GY), w: W, h: H }; });
    maxRows = Math.max(maxRows, list.length);
  }
  const ncols = Object.keys(cols).length || 1;
  return { pos, width: 40 + ncols * W + (ncols - 1) * GX, height: 40 + maxRows * H + Math.max(0, maxRows - 1) * GY };
}

function drawGraph() {
  const ids = data.workers.map((w) => w.id);
  const edges = [];
  const inputs = (data.conn && data.conn.inputs) || {};
  for (const [to, ins] of Object.entries(inputs)) {
    for (const [name, x] of Object.entries(ins)) {
      if (x.from && ids.includes(x.from)) edges.push({ from: x.from, to, name, ok: x.ok, how: x.how });
    }
  }
  const { pos, width, height } = layout(ids, edges);
  const svg = svgEl("svg", { viewBox: `0 0 ${width} ${height}`, width: "100%", style: `max-width:${width}px` });
  const defs = svgEl("defs");
  for (const [id, color] of [["a-ok", "var(--ok)"], ["a-bad", "var(--bad)"], ["a-cfg", "var(--muted)"]]) {
    const m = svgEl("marker", { id, viewBox: "0 0 10 10", refX: 9, refY: 5, markerWidth: 6, markerHeight: 6, orient: "auto-start-reverse" });
    m.appendChild(svgEl("path", { d: "M1 1L9 5L1 9", fill: "none", stroke: color, "stroke-width": 1.5 }));
    defs.appendChild(m);
  }
  svg.appendChild(defs);
  for (const e of edges) {
    const a = pos[e.from], b = pos[e.to];
    if (!a || !b) continue;
    const x1 = a.x + a.w, y1 = a.y + a.h / 2, x2 = b.x - 2, y2 = b.y + b.h / 2;
    const kind = !e.ok ? "bad" : e.how === "config" ? "cfg" : "ok";
    const color = kind === "bad" ? "var(--bad)" : kind === "cfg" ? "var(--muted)" : "var(--ok)";
    const mx = (x1 + x2) / 2;
    svg.appendChild(svgEl("path", { d: `M${x1} ${y1} C${mx} ${y1} ${mx} ${y2} ${x2} ${y2}`, fill: "none", stroke: color,
      "stroke-width": 1.4, "stroke-dasharray": kind === "ok" ? "" : "5 4", "marker-end": `url(#a-${kind})` }));
    svg.appendChild(svgEl("text", { x: mx, y: (y1 + y2) / 2 - 5, "text-anchor": "middle", fill: color, "font-size": 11 },
      `${e.name}${HOW_KO[e.how] ? " · " + HOW_KO[e.how] : ""}`));
  }
  for (const w of data.workers) {
    const p = pos[w.id];
    const running = w.actual_state === "running";
    svg.appendChild(svgEl("rect", { x: p.x, y: p.y, width: p.w, height: p.h, rx: 7, fill: "var(--panel-2)",
      stroke: running ? "var(--line-2)" : "var(--line)", "stroke-dasharray": running ? "" : "4 3" }));
    svg.appendChild(svgEl("text", { x: p.x + p.w / 2, y: p.y + 19, "text-anchor": "middle", fill: "var(--text)", "font-size": 13 }, w.id));
    const roles = Object.keys(((data.ios[w.id] || {}).provides) || {});
    svg.appendChild(svgEl("text", { x: p.x + p.w / 2, y: p.y + 34, "text-anchor": "middle", fill: "var(--muted)", "font-size": 11 },
      roles.length ? roles.join(" · ") : (running ? "내보내는 역할 없음" : "꺼짐")));
  }
  els.graph.replaceChildren(svg);
}

// 입력마다 고르기: 자동(선언 없음) | 그 역할을 내보내는 워커들
function drawTable(ctx) {
  const { h, ui } = ctx;
  const declared = {};
  for (const c of ((data.conn && data.conn.connections && data.conn.connections.connections) || [])) declared[c.to] = c.from;
  const rows = [];
  els.pick = {};
  for (const w of data.workers) {
    const uses = ((data.ios[w.id] || {}).uses) || {};
    for (const [name, u] of Object.entries(uses)) {
      const key = `${w.id}.${name}`;
      const cands = data.workers.filter((x) => x.id !== w.id && (((data.ios[x.id] || {}).provides) || {})[u.from]).map((x) => x.id);
      const opts = [["", "자동(하나뿐이면)"], ...cands.map((c) => [c, c])];
      if (declared[key] && !cands.includes(declared[key])) opts.push([declared[key], declared[key] + " (없음)"]);
      const sel = ui.select(opts, declared[key] || "");
      sel.disabled = !ui.isDevMode();
      if (!ui.isDevMode()) sel.title = ui.DEV_ONLY_TITLE;
      els.pick[key] = sel;
      const st = (((data.conn || {}).inputs || {})[w.id] || {})[name] || {};
      const state = st.error ? h("span", { style: "color:var(--bad)" }, st.error)
        : h("span", { class: "muted" }, [st.from || "이어지지 않음", HOW_KO[st.how] || ""].filter(Boolean).join(" · "));
      rows.push(h("tr", null, h("td", { class: "mono" }, key + (u.required ? " *" : "")), h("td", { class: "mono" }, `${u.from} ${u.version || ""}`),
        h("td", null, sel), h("td", null, state)));
    }
  }
  els.table.replaceChildren(rows.length
    ? h("table", { class: "tbl" }, h("thead", null, h("tr", null, h("th", null, "입력"), h("th", null, "쓰는 정의"), h("th", null, "상대"), h("th", null, "지금"))),
        h("tbody", null, ...rows))
    : h("div", { class: "muted" }, "입력(io.json uses)이 있는 워커가 없습니다"));
  const errs = (data.conn && data.conn.errors) || [];
  els.errors.replaceChildren(...errs.map((e) => h("div", { style: "color:var(--bad)" }, "선언 오류: " + e)));
  els.save.disabled = !ui.isDevMode() || !rows.length;
  els.save.title = ui.isDevMode() ? "연결 선언 저장(connections.json). 바뀐 입력을 가진 워커만 다시 시작" : ui.DEV_ONLY_TITLE;
}

function drawCommanders(ctx) {
  const check = (data.conn && data.conn.check) || {};
  const com = (data.conn && data.conn.connections && data.conn.connections.commanders) || {};
  const rows = Object.keys(com).sort().map((k) => {
    const c = check[k] || {};
    const note = c.ok === false ? "문제: " + c.error : (c.warnings && c.warnings.length ? "알림: " + c.warnings.join(", ") : "");
    return [k, (com[k] || []).join(", ") + (note ? "  — " + note : "")];
  });
  els.commanders.set(rows.length ? rows : [["(없음)", "화면만 명령합니다(늘 허용)"]]);
  els.guard.textContent = `검사 단계: ${(data.conn && data.conn.guard) || "—"} · 모드: ${(data.conn && data.conn.access_mode) || "—"}`;
}

async function load(ctx) {
  try {
    const [conn, workers] = await Promise.all([ctx.api("GET", "/connections"), ctx.api("GET", "/workers")]);
    data.conn = conn;
    data.workers = (workers || []).filter((w) => !w.builtin);
    const ios = {};
    await Promise.all(data.workers.map(async (w) => {
      try { ios[w.id] = await ctx.api("GET", `/workers/${w.id}/io`); } catch { ios[w.id] = null; }
    }));
    data.ios = ios;
    els.note.textContent = conn.error ? "연결 선언 파일 오류: " + conn.error + " (직전 값으로 동작 중)" : "";
    drawGraph();
    drawTable(ctx);
    drawCommanders(ctx);
  } catch (e) {
    els.note.textContent = "읽지 못함: " + e.message;
  }
}

async function save(ctx) {
  const list = Object.entries(els.pick).filter(([, s]) => s.value).map(([to, s]) => ({ to, from: s.value }));
  const file = Object.assign({}, (data.conn && data.conn.connections) || {});
  file.connections = list;
  await ctx.act("연결 저장", () => ctx.api("PUT", "/connections", file));
  await load(ctx);
}

export default {
  async mount(el, ctx) {
    const { h, ui } = ctx;
    els = {};
    els.note = h("div", { style: "color:var(--bad)" });
    els.graph = h("div", { style: "overflow-x:auto" });
    els.table = h("div");
    els.errors = h("div");
    els.save = ui.iconButton({ icon: "check", label: "저장", kind: "primary", onClick: () => save(ctx) });
    els.commanders = ui.kvList([]);
    els.guard = h("div", { class: "muted", style: "margin-top:6px" });
    el.appendChild(h("div", { class: "stack" },
      ui.panelHead({ title: "워커 연결", actions: [ui.iconButton({ icon: "refresh", title: "새로 읽기", onClick: () => load(ctx) })] }),
      h("div", { class: "hint", style: "font-size:12px;color:var(--muted)" },
        "어느 워커가 어느 워커의 출력을 받는지(입력 연결)와, 워커에 명령할 수 있는 곳(commanders)입니다. 입력 상대를 바꾸면 그 워커만 다시 시작합니다"),
      els.note,
      ui.card("연결 그림", els.graph,
        h("div", { class: "hint", style: "margin-top:6px" }, "실선 = 이어짐 · 빨간 점선 = 문제(버전·없는 항목 등) · 회색 점선 = 프리셋 설정 · 점선 상자 = 꺼진 워커")),
      ui.card("입력마다 상대", els.table, els.errors,
        h("div", { class: "btnrow", style: "margin-top:8px; justify-content:flex-end" }, els.save),
        h("div", { class: "hint" }, "자동: 그 역할을 내보내는 워커가 하나뿐이면 저절로 잇습니다. 여럿이면 골라야 합니다. 저장하면 바뀐 입력을 가진 워커만 다시 시작합니다")),
      ui.card("명령하는 곳(commanders)", els.commanders, els.guard,
        h("div", { class: "hint" }, "연결 선언 파일의 commanders. 화면은 적지 않아도 늘 허용(docs/command-path.md)"))));
    await load(ctx);
  },
  update() {},
  unmount() { els = {}; },
};
