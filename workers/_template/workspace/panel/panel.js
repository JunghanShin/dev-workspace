// 예제 워커 패널(워크스페이스 연동 ③층, 워커 폴더 workspace/panel/). 워크스페이스 화면이 이 폴더를 읽어 왼쪽 목록에 붙인다.
// 공통 부품(ctx.ui)으로만 그린다(ui/README.md "패널 디자인 규칙"). 명령은 "<모듈>.<명령>", 상태는 get_status의 모듈 칸.
// 실시간 값(source)은 텔레메트리 스트림(design.md 9.7)으로, 나머지는 2초마다 get_status.
// 스트림은 셸의 ctx.stream(화면 하나에 연결 하나, docs/stream-mux.md). 옛 길 EventSource("/workers/<id>/stream")도 그대로 된다
const STATUS_MS = 2000;
const STREAM_HZ = 10;
let els = {};
let timer = null;

async function command(ctx, cmd, args = {}) {
  return ctx.api("POST", `/workers/${ctx.workerId}/command`, { cmd, args });
}

// 스트림: 첫 메시지는 기준 상태(kf, 모든 주제), 이후 바뀐 주제만. 워커가 꺼지거나 연결이 끊기면 {"stream": "down"}
function startStream(ctx) {
  const mine = els;
  els.unstream = ctx.stream(ctx.workerId, STREAM_HZ, (msg) => {
    if (els !== mine) return;
    if (msg.stream === "down") { els.topics = null; showSource(null); return; }
    if (msg.kf) els.topics = { ...msg.topics };
    else if (els.topics) Object.assign(els.topics, msg.topics);
    if (els.topics) showSource(els.topics.source);
  });
}

function showSource(s) {
  els.seq.set(s ? s.seq : null, 0);
  els.mean.set(s ? s.mean : null, 3);
  els.rate.set(s ? s.rate_hz : null, 1);
  els.live.set(s ? "rx" : null, s ? "스트림 수신" : "스트림 없음", s ? "ok" : "idle");
}

async function refresh(ctx) {
  const w = ctx.state().byId[ctx.workerId];
  if (!w || w.actual_state !== "running") { render(null); return; }
  try {
    render(await command(ctx, "get_status"));
  } catch {
    render(null);
  }
}

function render(d) {
  const src = d && d.source, pr = d && d.processor, pe = d && d.peer;
  if (!els.topics) showSource(src || null);
  els.src.set(src ? [
    ["데이터 버스", `${src.bus.socket} · 받는 워커 ${src.bus.consumers} · 버림 ${src.bus.dropped}`],
    ["자식 프로세스", src.helper.cmd ? `${src.helper.cmd} · ${src.helper.running ? "실행 중" : "멈춤"} · 다시 띄움 ${src.helper.restarts}` : "없음"],
  ] : [["상태", "워커 꺼짐"]]);
  els.proc.set(pr ? [
    ["창", `${pr.count}/${pr.window}`],
    ["평균 · 최소 · 최대", `${pr.avg.toFixed(3)} · ${pr.min.toFixed(3)} · ${pr.max.toFixed(3)}`],
    ["받은 표본(포트)", String(pr.total)],
  ] : [["상태", "—"]]);
  els.peer.set(pe && pe.peer ? [
    ["상대 워커", pe.peer],
    ["연결", `데이터 ${pe.data_link ? "연결" : "끊김"} · 스트림 ${pe.stream_link ? "연결" : "끊김"}`],
    ["받음 · 빠짐", `${pe.received} · ${pe.lost} (상대 다시 시작 ${pe.peer_restarts})`],
    ["상대 상태(스트림)", pe.peer_topics && pe.peer_topics.source ? `seq ${pe.peer_topics.source.seq} · ${pe.peer_topics.source.rate_hz} Hz` : "—"],
  ] : [["상대 워커", "없음(설정 peer를 비움)"]]);
  els.peerRate.disabled = !(pe && pe.peer);
}

export default {
  mount(el, ctx) {
    const { h, ui, act } = ctx;
    const id = ctx.workerId;
    els.pill = ui.workerPill(null);
    els.live = ui.pill(null, "스트림 없음", "idle");
    els.controls = ui.workerControls(ctx, id);
    els.settings = ui.workerSettings(ctx, id, { title: "예제 워커 설정" });
    const settingsBtn = ui.settingsButton(els.settings, "설정");
    els.seq = ui.tile("표본 번호");
    els.mean = ui.tile("최근 평균");
    els.rate = ui.tile("생성 주기", "Hz");
    els.src = ui.kvList();
    els.proc = ui.kvList();
    els.peer = ui.kvList();
    const rateIn = h("input", { type: "text", inputmode: "decimal", placeholder: "Hz", style: "width:80px" });
    const setRate = ui.iconButton({ icon: "send", label: "내 주기", title: "source.set_rate (바로 적용, 저장 안 함)",
      onClick: () => act("주기", () => command(ctx, "source.set_rate", { hz: Number(rateIn.value) })) });
    els.peerRate = ui.iconButton({ icon: "send", label: "상대 주기", title: "peer.set_rate → 상대 워커의 source.set_rate (워커 간 명령)",
      onClick: () => act("상대 주기", () => command(ctx, "peer.set_rate", { hz: Number(rateIn.value) })) });
    const reset = ui.iconButton({ icon: "refresh", label: "초기화", title: "source.reset, processor.reset, peer.reset",
      onClick: () => act("초기화", async () => {
        await command(ctx, "source.reset"); await command(ctx, "processor.reset"); await command(ctx, "peer.reset");
      }) });
    // 탭(ui/worker-panels.md): 운용 = 주 값과 운용 조작(역할 명령), 상태 = 모듈별 상세 값, 도구 = 정비·시험(개발할 때만)
    const run = h("div", {}, ui.tiles(els.seq, els.mean, els.rate),
      h("div", { style: "margin-top:12px" }, ui.card("주기", h("div", { class: "btnrow" }, rateIn, setRate, els.peerRate))));
    const status = h("div", { class: "cols2" },
      ui.card("데이터 만들기(source)", els.src),
      ui.card("워커 안 처리(processor, 포트로 받음)", els.proc),
      ui.card("워커 간 받기(peer)", els.peer));
    const tools = h("div", {}, ui.card("시험", h("div", { class: "btnrow" }, reset),
      h("div", { class: "hint", style: "margin-top:6px" }, "셈을 처음부터(source·processor·peer)")));
    el.append(
      ui.panelHead({ title: id, badges: [els.pill, els.live], actions: [settingsBtn, h("span", { class: "ph-sep" }), els.controls] }),
      ui.panelTabs([{ id: "run", label: "운용", body: run }, { id: "status", label: "상태", body: status },
                    { id: "tools", label: "도구", body: tools, devOnly: true }]),
    );
    refresh(ctx);
    timer = setInterval(() => refresh(ctx), STATUS_MS);
    startStream(ctx);
  },

  update(state, ctx) {
    const w = state.byId[ctx.workerId];
    if (!w) return;
    els.pill.set(w.actual_state);
    els.controls.update(w);
    els.settings.update(w);
  },

  unmount() {
    if (els.unstream) els.unstream();
    clearInterval(timer);
    timer = null;
    if (els.settings) els.settings.dispose();
    els = {};
  },
};
