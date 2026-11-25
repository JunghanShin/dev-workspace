// 패널 "시스템"(ui/system-panel.md 3장): 매니저 6개의 상태를 한눈에. 주제는 /system.js가 받은 매니저 스트림에서,
// record는 /workers 지표, 명령 임대는 패널이 열려 있을 때만 5 s마다 워커 get_status(읽기 명령)의 leases.
// 공통 부품(ctx.ui)으로만 그린다
import * as sys from "/system.js";

const LEASE_MS = 5000;
let els = {};
let leaseTimer = null;
let leases = {};       // 워커 id → {역할: {cmd, from, origin, left_ms}}
let leaseAt = 0;
let unwant = null;     // 띠에 안 쓰는 매니저 스트림(mission·payload·config)은 패널이 열려 있는 동안만

function ago(ms) {
  if (typeof ms !== "number") return "—";
  const s = Math.round(ms / 1000);
  return s < 60 ? `${s}초` : s < 3600 ? `${Math.floor(s / 60)}분 ${s % 60}초` : `${Math.floor(s / 3600)}시간 ${Math.floor((s % 3600) / 60)}분`;
}

function bytes(n) {
  if (typeof n !== "number") return "—";
  return n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KB` : n < 1073741824 ? `${(n / 1048576).toFixed(1)} MB` : `${(n / 1073741824).toFixed(2)} GB`;
}

function table(h, head, rows, empty) {
  if (!rows.length) return h("div", { class: "hint" }, empty);
  return h("table", {}, h("thead", {}, h("tr", {}, head.map((t) => h("th", {}, t)))),
    h("tbody", {}, rows.map((r) => h("tr", {}, r.map((c) => h("td", {}, c))))));
}

// 카드 하나 = 상태 배지 + 본문. 매니저가 없거나 꺼졌으면 본문 대신 그 글자
function makeCard(ui, h, title) {
  const pill = ui.pill(null, "", "idle");
  const body = h("div");
  const c = ui.card(title, h("div", { class: "row" }, pill), body);
  c.show = (bad, nodes) => {
    if (bad) { pill.set(bad[1] === "idle" ? "stop" : "alert", bad[0], bad[1]); pill.style.display = ""; body.replaceChildren(); return; }
    pill.style.display = "none";
    body.replaceChildren(...nodes);
  };
  c.pill = pill;
  return c;
}

async function loadLeases(ctx) {
  const st = ctx.state();
  const ids = (st.workers || []).filter((w) => w.actual_state === "running").map((w) => w.id);
  const got = {};
  await Promise.all(ids.map(async (id) => {
    try {
      const d = await ctx.api("POST", `/workers/${encodeURIComponent(id)}/command`, { cmd: "get_status", args: {} });
      if (d && d.leases && typeof d.leases === "object" && Object.keys(d.leases).length) got[id] = d.leases;
    } catch { /* 응답 없는 워커는 건너뜀 */ }
  }));
  leases = got;
  leaseAt = Date.now();
  if (els.root) render(ctx.state(), ctx);
}

function healthPill(ui, health) {
  const [icon, text, kind] = sys.HEALTH[health] || ["info", health || "—", "idle"];
  return ui.pill(icon, text, kind);
}

function render(state, ctx) {
  const { h, ui } = ctx;
  const S = sys.get("system"), F = sys.get("fault"), M = sys.get("mission"), P = sys.get("payload"), C = sys.get("config");
  const rec = sys.record();

  // 머리 타일
  const st = S && S.up ? S.topic : null;
  els.tHealth.set(st ? (sys.HEALTH[st.health] || [null, st.health])[1] : (S ? "모름" : "설치 안 됨"));
  const ft = F && F.up ? F.topic : null;
  const nFault = ft && ft.active ? Object.keys(ft.active).length : null;
  els.tFault.set(F ? nFault : "설치 안 됨", 0);
  const mt = M && M.up ? M.topic : null;
  els.tRefused.set(mt ? (mt.busy || 0) + (mt.forbidden || 0) : (M ? null : "설치 안 됨"), 0);
  els.tRecord.set(rec ? (rec.actual_state === "running" ? "기록 중" : "꺼짐") : "설치 안 됨");

  // 상태
  els.status.show(sys.feedState(S, "status-manager"), st ? [
    ui.kvList([["전체", healthPill(ui, st.health)], ["이유", (st.reasons || []).join(" · ") || "없음"],
               ["process-manager", st.pm_link ? "연결" : "끊김"]]),
    table(h, ["워커", "건강", "상태 · 이유"],
      Object.entries(st.workers || {}).sort(([a], [b]) => a.localeCompare(b)).map(([id, w]) => [
        h("span", { class: "mono" }, id), healthPill(ui, w.health),
        h("span", {}, w.state || "—", w.why ? h("span", { class: "muted" }, " · " + w.why) : null)]), "워커 없음"),
  ] : []);

  // 이상
  els.fault.show(sys.feedState(F, "fault-manager"), ft ? [
    table(h, ["심각도", "내용", "지난 시간", ""],
      Object.entries(ft.active || {}).map(([key, a]) => {
        const [text, kind] = sys.SEVERITY[a.severity] || [a.severity, "warn"];
        return [ui.pill("alert", text, kind),
          h("span", {}, a.text, h("span", { class: "muted mono" }, ` · ${a.rule}${a.worker ? " · " + a.worker : ""}`)), ago(a.age_ms),
          a.acked ? ui.pill("check", "확인함", "idle")
            : ui.iconButton({ icon: "check", title: "확인(봤음 표시)", onClick: () =>
                ctx.act("이상 확인", () => ctx.api("POST", `/workers/${encodeURIComponent(F.worker)}/command`,
                  { cmd: "fault.ack", args: { id: key } })) })];
      }), "올라간 이상 없음"),
    // 잡아 둔 대응(워크스페이스는 대응을 보내지 않고 잡아 둠, docs/modes.md 3장): 하나씩 실행
    (ft.held_actions || []).length ? table(h, ["잡아 둔 대응", "보낼 명령", "지난 시간", ""],
      ft.held_actions.map((x) => [h("span", { class: "mono" }, `${x.id} · ${x.stage ? "단계 " + x.stage : "처음"}`),
        h("span", { class: "mono" }, (x.actions || []).map((c) => `${c.target} ${c.cmd}`).join(", ")), ago(x.age_ms),
        ui.iconButton({ icon: "play", title: "이 대응 보내기(fault.run_action)", onClick: () =>
          ctx.act("대응 실행", () => ctx.api("POST", `/workers/${encodeURIComponent(F.worker)}/command`,
            { cmd: "fault.run_action", args: { id: x.id, stage: x.stage } })) })]), "") : null,
  ].filter(Boolean) : []);

  // 명령
  const mEvents = M ? M.events.filter((e) => /^command_(busy|forbidden|timeout)$/.test(e.code)).slice(-5).reverse() : [];
  els.mission.show(sys.feedState(M, "mission-manager"), mt ? [
    ui.kvList([["보냄", String(mt.sent ?? 0)], ["busy · timeout", `${mt.busy ?? 0} · ${mt.timeouts ?? 0}`],
               ["forbidden · unavailable", `${mt.forbidden ?? 0} · ${mt.unavailable ?? 0}`]]),
    table(h, ["대상.역할", "대기", "보내는 중"],
      Object.entries(mt.queues || {}).map(([k, q]) => [h("span", { class: "mono" }, k), String(q.waiting ?? 0),
        q.inflight ? h("span", {}, h("span", { class: "mono" }, q.inflight.cmd),
          h("span", { class: "muted" }, ` · ${q.inflight.origin || "—"} · ${q.inflight.age_ms} ms`)) : "—"]),
      (mt.sent ?? 0) ? "기다리는 명령 없음(큐는 비면 지워짐)" : "명령 큐 없음(아직 보낸 명령 없음)"),
    mEvents.length ? table(h, ["최근 거절", "내용"], mEvents.map((e) => [e.code.replace("command_", ""), h("span", { class: "muted" }, e.text)]), "") : null,
  ].filter(Boolean) : []);

  // 기록
  const rm = rec && rec.metrics ? rec.metrics : {};
  els.record.show(rec ? (rec.actual_state === "running" ? null : [`${rec.id} 꺼짐 (${rec.actual_state || "—"})`, "idle"])
                      : ["record-manager 설치 안 됨", "idle"], rec ? [
    ui.kvList([["줄 · 크기", `${rm.lines ?? "—"} · ${bytes(rm.bytes)}`], ["빠짐 · 쓰기 오류", `${rm.drops ?? 0} · ${rm.write_errors ?? 0}`],
               ["파일", String(rm.files ?? "—")], ["남은 디스크", rm.disk_free_mb !== undefined ? `${rm.disk_free_mb} MB` : "—"],
               ["구독", String(rm.subs ?? "—")]]),
  ] : []);

  // 모듈
  const pt = P && P.up ? P.topic : null;
  els.payload.show(sys.feedState(P, "payload-manager"), pt ? [
    table(h, ["모듈", "역할", "상태", "자동 시작", "건강"],
      Object.entries(pt.modules || {}).sort(([a], [b]) => a.localeCompare(b)).map(([id, m]) => [
        h("span", { class: "mono" }, id), (m.roles || []).join(", ") || "—", ui.workerPill(m.state),
        m.autostart ? "켬" : "끔", m.health ? healthPill(ui, m.health) : "—"]), "모듈 없음"),
  ] : []);

  // 설정
  const ct = C && C.up ? C.topic : null;
  els.config.show(sys.feedState(C, "config-manager"), ct ? [
    ui.kvList([["버전 어긋남", (ct.mismatch || []).length ? (ct.mismatch || []).map((x) => typeof x === "string" ? x : JSON.stringify(x)).join(" · ") : "없음"],
               ["스냅숏", `${(ct.snapshots || []).length}개`], ["마지막 스냅숏 뒤 바뀜", ct.drift ? "있음" : "없음"],
               ["프로필", ct.profile ? (ct.profile.name || JSON.stringify(ct.profile)) : "—"]]),
  ] : []);

  // 명령 임대
  const rows = [];
  for (const [id, ls] of Object.entries(leases))
    for (const [role, l] of Object.entries(ls))
      rows.push([h("span", { class: "mono" }, id), role, h("span", { class: "mono" }, l.cmd), l.origin || l.from || "—", `${l.left_ms} ms`]);
  els.lease.show(leaseAt ? null : ["읽는 중", "idle"],
    [table(h, ["워커", "역할", "명령", "보낸 곳", "남은 시간"], rows, "걸린 임대 없음")]);
}

export default {
  mount(el, ctx) {
    const { h, ui } = ctx;
    els.root = el;
    els.tHealth = ui.tile("시스템");
    els.tFault = ui.tile("이상");
    els.tRefused = ui.tile("명령 거절");
    els.tRecord = ui.tile("기록");
    els.status = makeCard(ui, h, "상태 (status)");
    els.fault = makeCard(ui, h, "이상 (fault)");
    els.mission = makeCard(ui, h, "명령 (mission)");
    els.record = makeCard(ui, h, "기록 (record)");
    els.payload = makeCard(ui, h, "모듈 (payload)");
    els.config = makeCard(ui, h, "설정 (config)");
    els.lease = makeCard(ui, h, "명령 임대");
    el.append(
      ui.panelHead({ title: "시스템", actions: [ui.iconButton({ icon: "refresh", title: "임대 다시 읽기", onClick: () => loadLeases(ctx) })] }),
      ui.tiles(els.tHealth, els.tFault, els.tRefused, els.tRecord),
      h("div", { class: "cols2" }, els.status, els.fault, els.mission, els.lease, els.payload, els.record, els.config));
    unwant = sys.want(["mission", "payload", "config"]);
    loadLeases(ctx);
    leaseTimer = setInterval(() => loadLeases(ctx), LEASE_MS);
  },

  update(state, ctx) { render(state, ctx); },

  unmount() {
    if (unwant) unwant();
    unwant = null;
    clearInterval(leaseTimer); leaseTimer = null; els = {}; leases = {}; leaseAt = 0; },
};
