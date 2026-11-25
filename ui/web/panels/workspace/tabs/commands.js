// 워크스페이스 › 명령 이력: 게이트웨이 명령 기록(<데이터>/records/gateway, docs/gateway.md)의 끝에서.
// 화면·원격에서 들어온 쓰기 요청(워커 명령, 시작/정지, 설정)마다 한 줄: 누가·어디서·무엇을·결과. 읽기 명령은 기록에 없음
let els = {};
let timer = null;

// 경로에서 대상: /api/board/workers/<id>/... → <id>, 그 밖은 경로 그대로
function target(path) {
  const m = /\/workers\/([^/]+)/.exec(path || "");
  return m ? decodeURIComponent(m[1]) : (path || "").replace(/^\/api\/board/, "");
}

function result(ui, code) {
  if (code >= 200 && code < 300) return ui.pill("check", String(code), "ok");
  if (code === 403 || code === 401) return ui.pill("lock", String(code), "warn");
  return ui.pill("alert", String(code), "bad");
}

async function load(ctx) {
  const { h, ui } = ctx;
  try {
    const lines = await ctx.remote("commands", { lines: els.n.value });
    const rows = [];
    for (const l of lines) { try { rows.push(JSON.parse(l)); } catch { /* 잘린 줄 */ } }
    rows.reverse();
    els.body.replaceChildren(...rows.map((c) => h("tr", {},
      h("td", { class: "mono" }, (c.t || "").replace("T", " ").slice(0, 19)),
      h("td", {}, c.user && c.user !== "-" ? c.user : h("span", { class: "muted" }, "—")),
      h("td", { class: "muted" }, c.from || (c.remote ? c.remote : "")),
      h("td", { class: "mono" }, target(c.path)),
      h("td", { class: "mono" }, c.cmd || `${c.method} ${(c.path || "").replace(/^\/api\/board/, "")}`),
      h("td", {}, result(ui, c.code), c.advisory
        ? h("div", { class: "hint", style: "color:var(--warn)" }, "운용이었으면 막힘: " + (c.advisory.message || c.advisory.condition || ""))
        : null),
      h("td", { class: "mono muted" }, c.ms !== undefined ? `${c.ms} ms` : ""))));
    els.status.textContent = `${rows.length}건`;
  } catch (e) {
    els.status.textContent = "읽지 못했습니다: " + e.message;
  }
}

export default {
  mount(el, ctx) {
    const { h, ui } = ctx;
    els.n = h("select", { onchange: () => load(ctx) }, [50, 200, 500].map((n) => h("option", { value: n }, `최근 ${n}건`)));
    els.n.value = "200";
    els.status = h("span", { class: "hint" });
    els.body = h("tbody");
    el.append(
      ui.panelHead({ title: "명령 이력", badges: [els.status],
        actions: [els.n, ui.iconButton({ icon: "refresh", title: "새로 읽기", onClick: () => load(ctx) })] }),
      h("div", { class: "hint", style: "font-size:12px;color:var(--muted);margin-bottom:8px" },
        "화면·원격에서 들어온 쓰기 요청(워커 명령, 시작/정지, 설정 저장)입니다. 상태 읽기는 남기지 않습니다"),
      ui.card(null, h("table", {},
        h("thead", {}, h("tr", {}, ["시각(보드)", "사람", "보낸 곳", "대상", "명령", "결과", "걸린 시간"].map((t) => h("th", {}, t)))),
        els.body)));
    load(ctx);
    timer = setInterval(() => load(ctx), 5000);
  },

  unmount() { clearInterval(timer); timer = null; els = {}; },
};
