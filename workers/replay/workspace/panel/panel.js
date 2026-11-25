// replay 워커 패널(워크스페이스 연동 ③층). 데이터 버스 녹화·재생.
// 공통 부품(ctx.ui)으로만 그린다(ui/README.md "패널 디자인 규칙"). 명령은 "replay.<명령>", 상태는 get_status의 replay 칸(2초마다).
const STATUS_MS = 2000;
let els = {};
let timer = null;

async function command(ctx, cmd, args = {}) {
  return ctx.api("POST", `/workers/${ctx.workerId}/command`, { cmd, args });
}

async function refresh(ctx) {
  const w = ctx.state().byId[ctx.workerId];
  if (!w || w.actual_state !== "running") { render(ctx, null, null); return; }
  try {
    const [s, l] = await Promise.all([command(ctx, "get_status"), command(ctx, "replay.list")]);
    render(ctx, s && s.replay, l);
  } catch {
    render(ctx, null, null);
  }
}

function render(ctx, r, l) {
  const { h, ui, act } = ctx;
  const rec = r && r.recording, pl = r && r.playing;
  els.recPill.set(rec && rec.active ? "record" : null, rec && rec.active ? (rec.link ? "녹화 중" : "생산자 기다림") : "녹화 없음",
    rec && rec.active ? (rec.link ? "bad" : "warn") : "idle");
  els.playPill.set(pl && pl.active ? "play" : null, pl && pl.active ? "재생 중" : "재생 없음", pl && pl.active ? "ok" : "idle");
  els.recFrames.set(rec ? rec.frames : null, 0);
  els.recMb.set(rec ? rec.mb : null, 1);
  els.playFrames.set(pl ? pl.frames : null, 0);
  els.consumers.set(pl ? pl.consumers : null, 0);
  els.state.set(r ? [
    ["녹화", rec.name ? `${rec.name} ← ${rec.source}${rec.active ? "" : " (끝남)"}` : "—"],
    ["녹화 오류", rec.error || "없음"],
    ["재생", pl.name ? `${pl.name} · ${pl.frames}/${pl.total} · ${pl.speed}배속${pl.loop ? ` · 반복 ${pl.loops}` : ""}${pl.active ? "" : " (끝남)"}` : "—"],
    ["재생 데이터 버스", r.bus_socket],
    ["녹화 폴더", l ? `${r.dir} · ${l.total_mb.toFixed(1)} / ${l.max_total_mb} MB` : r.dir],
  ] : [["상태", "워커 꺼짐"]]);
  const items = (l && l.recordings) || [];
  els.list.replaceChildren(...(items.length ? items.map((i) => h("tr", {},
    h("td", {}, i.name), h("td", {}, i.source), h("td", {}, i.created || "—"),
    h("td", {}, `${i.width}×${i.height} ${i.pixel}`), h("td", {}, String(i.frames)),
    h("td", {}, `${i.seconds.toFixed(1)} s`), h("td", {}, `${i.mb.toFixed(1)} MB`),
    h("td", {}, h("div", { class: "btnrow" },
      ui.iconButton({ icon: "play", title: "이 녹화를 재생(배속·반복은 위 입력)", onClick: () => act("재생", () =>
        command(ctx, "replay.play_start", { name: i.name, speed: Number(els.speed.value) || 1, loop: els.loop.checked })) }),
      ui.iconButton({ icon: "trash", title: "삭제", kind: "danger", onClick: () => {
        if (confirm(`녹화 ${i.name}을(를) 지울까요? 되돌릴 수 없습니다.`)) act("삭제", () => command(ctx, "replay.delete", { name: i.name }));
      } }))))) : [h("tr", {}, h("td", { colspan: 8, class: "hint" }, r ? "녹화 없음" : "워커 꺼짐"))]));
}

function stamp() {
  const d = new Date(), p = (n) => String(n).padStart(2, "0");
  return `rec-${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

export default {
  mount(el, ctx) {
    const { h, ui, act } = ctx;
    const id = ctx.workerId;
    els.pill = ui.workerPill(null);
    els.recPill = ui.pill(null, "녹화 없음", "idle");
    els.playPill = ui.pill(null, "재생 없음", "idle");
    els.controls = ui.workerControls(ctx, id);
    els.settings = ui.workerSettings(ctx, id, { title: "녹화·재생 설정" });
    const settingsBtn = ui.settingsButton(els.settings, "설정");
    els.recFrames = ui.tile("녹화 프레임");
    els.recMb = ui.tile("녹화 크기", "MB");
    els.playFrames = ui.tile("재생 프레임");
    els.consumers = ui.tile("재생 받는 워커");
    els.state = ui.kvList();

    const source = h("input", { type: "text", placeholder: "워커 id(예: ximea)", style: "width:150px" });
    const name = h("input", { type: "text", placeholder: "이름(비우면 시각)", style: "width:170px" });
    const seconds = h("input", { type: "text", inputmode: "decimal", placeholder: "초(선택)", style: "width:80px" });
    const recStart = ui.iconButton({ icon: "record", label: "녹화", title: "replay.record_start: 그 워커의 데이터 버스를 녹화",
      onClick: () => act("녹화", () => {
        const args = { source: source.value.trim(), name: name.value.trim() || stamp() };
        if (seconds.value.trim()) args.seconds = Number(seconds.value);
        return command(ctx, "replay.record_start", args);
      }) });
    const recStop = ui.iconButton({ icon: "stop", label: "녹화 멈춤", title: "replay.record_stop",
      onClick: () => act("녹화 멈춤", () => command(ctx, "replay.record_stop")) });
    els.speed = h("input", { type: "text", inputmode: "decimal", value: "1", title: "배속(0 초과 100 이하)", style: "width:60px" });
    els.loop = h("input", { type: "checkbox" });
    const playStop = ui.iconButton({ icon: "stop", label: "재생 멈춤", title: "replay.play_stop",
      onClick: () => act("재생 멈춤", () => command(ctx, "replay.play_stop")) });
    els.list = h("tbody");

    // 탭(ui/worker-panels.md): 운용 = 녹화·재생·녹화 목록, 상태 = 셈 타일·상태. 개발 도구라 도구 탭 없음
    const run = h("div", {},
      h("div", { class: "cols2" },
        ui.card("녹화", h("div", { class: "btnrow" }, source, name, seconds, recStart, recStop),
          h("div", { class: "hint", style: "margin-top:6px" }, "받을 워커의 데이터 버스를 순서대로 전부 저장. 생산자가 아직 없으면 기다렸다가 붙음")),
        ui.card("재생", h("div", { class: "btnrow" }, h("span", {}, "배속"), els.speed,
          h("label", {}, els.loop, " 반복"), playStop),
          h("div", { class: "hint", style: "margin-top:6px" }, "아래 목록의 ▶로 시작. 받는 워커는 설정 상대 워커(peer)를 이 워커 id로"))),
      h("div", { style: "margin-top:12px" }, ui.card("녹화 목록", h("table", {},
        h("thead", {}, h("tr", {}, ["이름", "출처", "만든 시각", "형식", "프레임", "길이", "크기", ""].map((t) => h("th", {}, t)))),
        els.list))));
    const status = h("div", {}, ui.tiles(els.recFrames, els.recMb, els.playFrames, els.consumers),
      h("div", { style: "margin-top:12px" }, ui.card("상태", els.state)));
    el.append(
      ui.panelHead({ title: id, badges: [els.pill, els.recPill, els.playPill],
        actions: [settingsBtn, h("span", { class: "ph-sep" }), els.controls] }),
      ui.panelTabs([{ id: "run", label: "운용", body: run }, { id: "status", label: "상태", body: status }]),
    );
    refresh(ctx);
    timer = setInterval(() => refresh(ctx), STATUS_MS);
  },

  update(state, ctx) {
    const w = state.byId[ctx.workerId];
    if (!w) return;
    els.pill.set(w.actual_state);
    els.controls.update(w);
    els.settings.update(w);
  },

  unmount() {
    clearInterval(timer);
    timer = null;
    if (els.settings) els.settings.dispose();
    els = {};
  },
};
