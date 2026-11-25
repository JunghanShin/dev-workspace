// 워크스페이스(ui/workspace-screen.md): 탭 여섯을 담는 패널. 탭은 주소에 남는다(#/workspace/<탭>).
// 탭마다 파일 하나(tabs/<탭>.js, 패널과 같은 mount·update·unmount). 열린 탭만 스트림·폴링을 연다
const TABS = [
  ["overview", "개요"],
  ["logs", "로그"],
  ["commands", "명령 이력"],
  ["connections", "워커 연결"],
  ["settings", "설정"],
  ["manage", "관리"],
];
const mods = {};
let cur = null;  // { id, mod, ctx }

// 탭 안에서는 패널 제목을 다시 그리지 않는다: 탭의 panelHead는 도구 줄(배지·버튼)만
function tabUi(ui) {
  return Object.assign({}, ui, {
    panelHead(o = {}) {
      const head = ui.panelHead({ ...o, title: "" });
      head.classList.add("ph-tool");
      if (!(o.badges || []).length && !(o.actions || []).filter(Boolean).length) head.style.display = "none";
      return head;
    },
  });
}

export default {
  async mount(el, ctx) {
    const { h, ui } = ctx;
    const want = TABS.some(([id]) => id === ctx.sub) ? ctx.sub : "overview";
    const strip = h("div", { class: "tabs", role: "tablist" },
      TABS.map(([id, label]) => h("a", { href: `#/workspace/${id}`, role: "tab", class: "tab" + (id === want ? " on" : ""),
                                         "aria-selected": id === want ? "true" : "false" }, label)));
    const body = h("div", { class: "tab-body" });
    el.append(ui.panelHead({ title: "워크스페이스" }), strip, body);
    try {
      if (!mods[want]) mods[want] = (await import(`./tabs/${want}.js`)).default;
    } catch (e) {
      body.appendChild(h("div", { class: "empty err" }, `탭 ${want} 불러오기 실패: ${e.message}`));
      return;
    }
    const tctx = { ...ctx, ui: tabUi(ui), tab: want };
    cur = { id: want, mod: mods[want], ctx: tctx };
    mods[want].mount(body, tctx);
    if (mods[want].update) mods[want].update(ctx.state(), tctx);
  },

  update(state) {
    if (cur && cur.mod.update) cur.mod.update(state, cur.ctx);
  },

  unmount() {
    if (cur && cur.mod.unmount) { try { cur.mod.unmount(cur.ctx); } catch { /* 이미 떠남 */ } }
    cur = null;
  },
};
