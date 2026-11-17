// 로그: 워커 로그(stdout/stderr), 서비스 로그(workspace-start·Manager·REST·logger). 보드 파일을 SSH로 읽는다.
// 공통 부품(ctx.ui)으로만 그린다
let els = {};
let timer = null;

async function load(ctx) {
  const src = els.source.value;
  const lines = els.lines.value;
  try {
    const out = src === "__service"
      ? await ctx.remote("service_log", { lines })
      : await ctx.remote("worker_log", { id: src, lines });
    const f = els.filter.value.trim().toLowerCase();
    const shown = f ? out.filter((l) => l.toLowerCase().includes(f)) : out;
    const atBottom = els.pre.scrollTop + els.pre.clientHeight >= els.pre.scrollHeight - 20;
    els.pre.textContent = shown.join("\n") || "(로그 없음)";
    if (atBottom) els.pre.scrollTop = els.pre.scrollHeight;
    els.status.textContent = `${shown.length}줄 · ${new Date().toLocaleTimeString("ko-KR", { hour12: false })}`;
  } catch (e) {
    els.status.textContent = "";
    els.pre.textContent = "로그를 읽지 못했습니다: " + e.message;
  }
}

function syncSources(state) {
  const want = ["__service", ...state.workers.map((w) => w.id)];
  const have = [...els.source.options].map((o) => o.value);
  if (JSON.stringify(want) === JSON.stringify(have)) return;
  const keep = els.source.value;
  els.source.replaceChildren(...want.map((v) => {
    const o = document.createElement("option");
    o.value = v;
    o.textContent = v === "__service" ? "서비스 (workspace)" : v;
    return o;
  }));
  if (want.includes(keep)) els.source.value = keep;
}

function schedule(ctx) {
  clearInterval(timer);
  timer = els.autoOn ? setInterval(() => load(ctx), 3000) : null;
  els.auto.setOn(els.autoOn);
}

export default {
  mount(el, ctx) {
    const { h, ui } = ctx;
    els.source = h("select", { onchange: () => load(ctx) });
    els.lines = h("select", { onchange: () => load(ctx) }, [100, 200, 500, 1000].map((n) => h("option", { value: n }, `${n}줄`)));
    els.lines.value = "200";
    els.filter = h("input", { placeholder: "찾을 글자", oninput: () => load(ctx) });
    els.autoOn = true;
    els.auto = ui.iconButton({ icon: "clock", title: "3초마다 새로 읽기", onClick: () => { els.autoOn = !els.autoOn; schedule(ctx); } });
    els.status = h("span", { class: "hint" });
    els.pre = h("pre", { class: "log" }, "");
    el.append(
      ui.panelHead({ title: "로그", badges: [els.status], actions: [
        els.source, els.lines, h("span", { class: "sl-i" }, ui.icon("search")), els.filter, els.auto,
        ui.iconButton({ icon: "refresh", title: "새로 읽기", onClick: () => load(ctx) })] }),
      els.pre);
    syncSources(ctx.state());
    const first = ctx.state().workers.find((w) => w.type === "ximea_camera" || w.type === "siyi_gimbal");
    if (first) els.source.value = first.id;
    load(ctx);
    schedule(ctx);
  },

  update(state) { syncSources(state); },

  unmount() { clearInterval(timer); timer = null; els = {}; },
};
