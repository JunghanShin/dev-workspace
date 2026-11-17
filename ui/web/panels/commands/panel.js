// 명령 이력: 보드 workspace-logger 기록(<데이터>/records)의 명령 줄. 누가 무엇을 보냈고 성공했는지.
// 공통 부품(ctx.ui)으로만 그린다
let els = {};
let timer = null;

function detail(c) {
  if (c.config) return JSON.stringify(c.config);
  if (c.autostart !== undefined) return "autostart=" + c.autostart;
  if (c.worker) return JSON.stringify(c.worker);
  return "";
}

async function load(ctx) {
  const { h, ui } = ctx;
  try {
    const lines = await ctx.remote("commands", { lines: els.n.value });
    const rows = [];
    for (const l of lines) { try { rows.push(JSON.parse(l)); } catch { /* 잘린 줄 */ } }
    rows.reverse();
    els.body.replaceChildren(...rows.map((c) => h("tr", {},
      h("td", { class: "mono" }, (c.t || "").replace("T", " ").slice(0, 23)),
      h("td", {}, c.op), h("td", {}, c.id || ""), h("td", { class: "muted" }, c.client || ""),
      h("td", {}, ui.pill(c.ok ? "check" : "alert", c.ok ? "성공" : "실패", c.ok ? "ok" : "bad")),
      h("td", { class: "mono muted", style: "word-break:break-all" },
        c.ok ? detail(c) : ((c.error && (c.error.message || c.error.code)) || "")))));
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
      ui.card(null, h("table", {},
        h("thead", {}, h("tr", {}, ["시각(보드)", "명령", "워커", "보낸 곳", "결과", "내용"].map((t) => h("th", {}, t)))),
        els.body)));
    load(ctx);
    timer = setInterval(() => load(ctx), 5000);
  },

  unmount() { clearInterval(timer); timer = null; els = {}; },
};
