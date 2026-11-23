// 워크스페이스: 이 보드에 적용된 프로필과 워크스페이스 설정 파일(읽기 전용), 보드 연결 상태.
// 워크스페이스 설정은 파일로만 바꾼다(design.md 6.10). 이 화면은 보여 주기만 한다. 데이터: REST GET /system/workspace
// 운용/정비 모드(원칙 17)만 여기서 바꾼다: 저장하지 않는 상태라 설정이 아님. 게이트웨이 GET·PUT /mode, 관리 역할만
const RELOAD_MS = 15000;

const ORIGIN = {
  profile: ["check", "프로필 그대로", "ok"],
  default: ["check", "기본값", "idle"],
  changed: ["wrench", "보드에서 바뀜", "warn"],
  missing: [null, "없음", "idle"],
  unknown: [null, "비교 불가", "idle"],
};
const FILE_DESC = {
  "network.conf": "보드 망 설정(고정 IP 등, 설치할 때 적용)",
  "manager.json": "Manager 동작(워치독·재시작 정책 등)",
  "workspace.env": "워크스페이스 실행 설정(REST 포트, 화면 제목, 기록 등)",
  "mediamtx.yml": "미디어 서버(MediaMTX) 설정",
};

let els = {};
let timer = null;

export default {
  mount(el, ctx) {
    const { ui, h } = ctx;
    els.link = ui.pill("link", "—", "idle");
    els.prof = ui.pill("sliders", "—", "idle");
    els.summary = ui.kvList();
    els.env = h("tbody");
    els.files = h("div");
    els.state = ui.kvList();
    els.err = h("div", { class: "muted" });
    els.buildInfo = ui.kvList();
    els.buildBtn = ui.iconButton({ icon: "play", label: "제품 빌드", kind: "primary",
      title: "이 보드에서 제품 묶음(장비 탑재용 결과물 하나)을 만들고 내려받기", onClick: () => openBuild(ctx) });
    els.buildDl = h("a", { class: "ib", href: "/api/board/system/product-build/download", download: "", style: "display:none" },
      ui.icon("rx"), h("span", { class: "ib-label" }, "마지막 묶음 내려받기"));
    els.buildDlg = buildDialog(ctx);
    els.modeInfo = ui.kvList();
    els.modePill = ui.pill("check", "—", "idle");
    els.modeBtn = ui.iconButton({ icon: "wrench", label: "정비 모드 켜기", onClick: () => toggleMode(ctx) });
    els.modeBtn.style.display = "none";
    els.modeCard = ui.card("운용/정비 모드", h("div", { class: "btnrow", style: "align-items:center" }, els.modePill, els.modeBtn),
      h("div", { style: "margin-top:8px" }, els.modeInfo),
      h("div", { class: "hint", style: "margin-top:6px" },
        "정비 모드는 장비를 하나씩 시험할 때 켭니다. 켜고 끈 시각과 그동안의 명령은 모두 기록되고, "
        + "워크스페이스를 다시 시작하면 운용 모드로 돌아갑니다"));
    el.append(
      ui.panelHead({ title: "워크스페이스", badges: [els.prof, els.link],
        actions: [ui.iconButton({ icon: "refresh", title: "새로 읽기", onClick: () => load(ctx) })] }),
      els.err,
      h("div", { style: "margin-bottom:12px" }, els.modeCard),
      ui.card("제품 빌드", els.buildInfo,
        h("div", { class: "hint", style: "margin-top:6px" },
          "커밋된 그대로 장비 탑재용 묶음 하나를 만든다(소스 없음·컴파일·심볼 제거, 기본 화면만, 약 2분). "
          + "프로필의 product.json 기준. 마지막 묶음 하나만 보드에 둔다"),
        h("div", { class: "btnrow", style: "margin-top:8px" }, els.buildBtn, els.buildDl)),
      h("div", { style: "margin-top:12px" }, ui.card("프로필", els.summary)),
      h("div", { style: "margin-top:12px" }, ui.card("실행 설정 (workspace.env)",
        h("table", {}, h("thead", {}, h("tr", {}, ["키", "보드 값", "기본값", "설명"].map((t) => h("th", {}, t)))), els.env))),
      h("div", { style: "margin-top:12px" }, ui.card("설정 파일 (<데이터>/config)", els.files)),
      h("div", { style: "margin-top:12px" }, ui.card("연결", els.state)),
      h("div", { style: "margin-top:12px" }, ui.card("바꾸는 법", h("div", { class: "muted" },
        "워크스페이스 설정은 화면에서 바꾸지 않습니다(잘못 바꾸면 화면 자체가 끊길 수 있음). 보드에서 위 파일을 고친 뒤 "
        + "workspace-start.sh로 다시 시작합니다. 프로필 값으로 되돌리려면 install_workspace.sh --reset-config. "
        + "워커 설정은 각 워커의 설정 창에서 바꿉니다. 자세한 것은 워크스페이스 docs/config.md."))),
    );
    load(ctx);
    timer = setInterval(() => load(ctx), RELOAD_MS);
  },

  update(state) {
    els.auth = state.auth;
    // 모드는 머리 배지와 같은 값(셸이 /api/auth/me로 5초마다 읽음)이 바뀌면 다시 읽는다
    if (state.auth && state.auth.mode && els.mode && state.auth.mode !== els.mode.mode) loadMode();
    const ui = state.ui || {};
    const ago = (sec) => (sec ? `${Math.round(Date.now() / 1000 - sec)}초 전` : "—");
    els.link.set("link", state.connected ? "보드 연결됨" : "보드 응답 없음", state.connected ? "ok" : "bad");
    els.state.set([
      ["화면 제공", location.host],
      ["마지막 REST 성공", ago(ui.rest_last_ok)],
      ["마지막 REST 오류", ui.rest_last_error || "—"],
      ["영상 주소 틀", (ui.media && ui.media.url) || "—"],
      ["Manager 클라이언트", state.system ? Object.entries(state.system.clients || {}).map(([k, v]) => `${k}: ${v}`).join(", ") : "—"],
    ]);
  },

  unmount() {
    clearInterval(timer);
    timer = null;
    clearInterval(els.buildTimer);
    if (els.buildDlg) els.buildDlg.dispose();
    els = {};
    modeCtx = null;
  },
};

// ---- 제품 빌드(REST /system/product-build, design.md 11.4) ----------------------------------------------
// 모달 세 단계: 점검(프로필 고르기, 빌드 전 확인) → 진행(진행 막대·단계·로그, 취소) → 완료(내려받기) / 실패(이유·로그)

function buildDialog(ctx) {
  const { h, ui } = ctx;
  const b = {};
  b.profile = h("select", {});
  b.profile.addEventListener("change", () => { pickPower(); runCheck(ctx); });
  // 전원 모드: 장비에 설치할 때 맞출 nvpmodel 모드. 기본은 프로필 product.json의 power_mode
  b.power = h("select", { title: "장비 설치(install.sh)가 맞출 Jetson 전원 모드. 그대로 = 장비 설정을 건드리지 않음" });
  b.checks = h("tbody");
  b.checkHint = h("div", { class: "hint" });
  b.start = ui.iconButton({ icon: "play", label: "빌드 시작", kind: "primary", onClick: () => startBuild(ctx) });
  b.recheck = ui.iconButton({ icon: "refresh", label: "다시 점검", onClick: () => runCheck(ctx) });
  b.checkBox = h("div", {},
    h("div", { class: "btnrow", style: "align-items:center" }, h("span", { class: "muted" }, "프로필"), b.profile,
      h("span", { class: "muted", style: "margin-left:12px" }, "전원 모드"), b.power),
    h("table", { style: "margin-top:8px" }, b.checks), b.checkHint,
    h("div", { class: "btnrow", style: "margin-top:10px" }, b.start, b.recheck));
  b.step = h("div", { class: "muted" });
  b.fill = h("span", { class: "mtr-f" });
  b.pct = h("span", { class: "hint" });
  b.cancel = ui.iconButton({ icon: "stop", label: "취소", kind: "danger", onClick: () => cancelBuild(ctx) });
  b.runBox = h("div", {}, b.step,
    h("div", { class: "mtr-b", style: "margin:8px 0 4px" }, b.fill), b.pct,
    h("div", { class: "btnrow", style: "margin-top:10px" }, b.cancel));
  b.result = ui.kvList();
  b.dl = h("a", { class: "ib primary", href: "/api/board/system/product-build/download", download: "" },
    ui.icon("rx"), h("span", { class: "ib-label" }, "내려받기"));
  b.again = ui.iconButton({ icon: "refresh", label: "다시 빌드", onClick: () => showCheck(ctx) });
  b.doneBox = h("div", {}, b.result,
    h("div", { class: "hint", style: "margin-top:6px" }, "장비 설치: tar xzf <파일> && sudo bash <폴더>/install.sh --owner <계정>"),
    h("div", { class: "btnrow", style: "margin-top:10px" }, b.dl, b.again));
  b.log = h("pre", { class: "log", style: "max-height:30vh; margin:0" });
  b.logBox = h("details", {}, h("summary", { class: "muted" }, "자세한 로그"), b.log);
  const d = ui.dialog("제품 빌드", b.checkBox, b.runBox, b.doneBox, b.logBox);
  d.style.width = "min(560px, 94vw)";
  d.b = b;
  d.addEventListener("close", () => { clearInterval(els.buildTimer); els.buildTimer = null; });
  return d;
}

function show(box) {
  const b = els.buildDlg.b;
  for (const k of ["checkBox", "runBox", "doneBox"]) b[k].hidden = b[k] !== box;
}

async function openBuild(ctx) {
  els.buildDlg.show();
  let s;
  try { s = await ctx.api("GET", "/system/product-build"); } catch (e) { ctx.toast("제품 빌드 상태를 읽지 못함: " + e.message, "bad"); return; }
  const b = els.buildDlg.b;
  b.profiles = s.profiles || [];
  b.profile.replaceChildren(...b.profiles.map((p) => ctx.h("option", { value: p.profile }, `${p.profile} (${p.name})`)));
  if (s.profile) b.profile.value = s.profile;
  let modes = [];
  try { modes = Object.values((await ctx.api("GET", "/system/power_modes")).modes || {}); } catch { /* Jetson이 아님 */ }
  b.power.replaceChildren(ctx.h("option", { value: "keep" }, "그대로(장비 설정)"),
    ...modes.map((m) => ctx.h("option", { value: m }, m)));
  pickPower();
  if (s.state === "running") { renderBuild(ctx, s); poll(ctx); }
  else showCheck(ctx);
}

function pickPower() {
  const b = els.buildDlg.b;
  const p = (b.profiles || []).find((x) => x.profile === b.profile.value);
  const want = (p && p.power_mode) || "keep";
  if (![...b.power.options].some((o) => o.value === want)) b.power.append(new Option(want, want));
  b.power.value = want;
}

function showCheck(ctx) {
  show(els.buildDlg.b.checkBox);
  els.buildDlg.b.logBox.hidden = true;
  runCheck(ctx);
}

async function runCheck(ctx) {
  const { h, ui } = ctx;
  const b = els.buildDlg.b;
  if (!b.profile.value) { b.checkHint.textContent = "product.json이 있는 프로필이 없습니다(install/profiles/<이름>/product.json)"; b.start.disabled = true; return; }
  b.checks.replaceChildren(h("tr", {}, h("td", { class: "muted" }, "점검 중…")));
  b.checkHint.className = "hint";
  b.start.disabled = true;
  let c;
  try { c = await ctx.api("GET", "/system/product-build/check?profile=" + encodeURIComponent(b.profile.value)); }
  catch (e) { b.checks.replaceChildren(h("tr", {}, h("td", { class: "err" }, "점검 실패: " + e.message))); return; }
  if (!els.buildDlg) return;
  b.checks.replaceChildren(...c.items.map((i) => h("tr", {},
    h("td", {}, ui.iconTag(i.ok ? "check" : "alert", "", i.ok ? "ok" : "bad")),
    h("td", { style: "white-space:nowrap" }, i.label), h("td", { class: i.ok ? "muted" : "err" }, i.detail || ""))));
  b.start.disabled = !c.ok;
  b.checkHint.textContent = c.ok ? "빌드하는 동안(약 2분) 보드 CPU를 많이 씁니다. 창을 닫아도 빌드는 계속됩니다"
    : "빨간 항목을 해결한 뒤 다시 점검하세요";
}

async function startBuild(ctx) {
  const b = els.buildDlg.b;
  b.start.disabled = true;
  try {
    renderBuild(ctx, await ctx.api("POST", "/system/product-build", { profile: b.profile.value, power_mode: b.power.value }));
    poll(ctx);
  } catch (e) {
    ctx.toast("빌드 시작 실패: " + e.message, "bad");
    b.start.disabled = false;
  }
}

async function cancelBuild(ctx) {
  if (!confirm("빌드를 취소할까요?")) return;
  try { await ctx.api("DELETE", "/system/product-build"); } catch (e) { ctx.toast("취소 실패: " + e.message, "bad"); }
}

function poll(ctx) {
  clearInterval(els.buildTimer);
  els.buildTimer = setInterval(async () => {
    if (!els.buildDlg || !els.buildDlg.open) return;
    try { renderBuild(ctx, await ctx.api("GET", "/system/product-build")); } catch { /* 다음에 */ }
  }, 1000);
}

const mmss = (sec) => `${Math.floor(sec / 60)}:${String(Math.floor(sec % 60)).padStart(2, "0")}`;

function renderBuild(ctx, s) {
  const b = els.buildDlg && els.buildDlg.b;
  if (!b) return;
  b.logBox.hidden = false;
  const atBottom = b.log.scrollTop + b.log.clientHeight >= b.log.scrollHeight - 4;
  b.log.textContent = (s.log || []).join("\n");
  if (atBottom) b.log.scrollTop = b.log.scrollHeight;
  if (s.state === "running") {
    show(b.runBox);
    b.step.textContent = `${s.profile} · ${s.step || ""} · ${mmss(s.elapsed || 0)}`;
    b.fill.style.width = `${s.pct || 0}%`;
    b.pct.textContent = `${s.pct || 0}%`;
    return;
  }
  clearInterval(els.buildTimer);
  els.buildTimer = null;
  if (s.state === "done" && s.result) {
    show(b.doneBox);
    b.result.set([
      ["파일", s.result.file],
      ["크기", `${s.result.mb} MB`],
      ["걸린 시간", mmss(s.elapsed || 0)],
      ["심볼 제거", s.result.stripped ? "됨" : "안 됨"],
      ["전원 모드(설치 시)", s.result.power_mode === "keep" ? "그대로(장비 설정)" : s.result.power_mode],
      ["SHA-256", s.result.sha256.slice(0, 16) + "…"],
    ]);
    load(ctx);
  } else if (s.state === "failed" || s.state === "cancelled") {
    show(b.checkBox);
    b.checkHint.textContent = (s.state === "cancelled" ? "취소됨" : "빌드 실패: " + (s.error || "")) + ". 자세한 로그를 확인하세요";
    b.checkHint.className = "hint bad";
    b.logBox.open = s.state === "failed";
    b.start.disabled = false;
  }
}

async function loadBuild(ctx) {
  let s;
  try { s = await ctx.api("GET", "/system/product-build"); } catch { return; }
  if (!els.buildInfo) return;
  els.buildInfo.set([
    ["마지막 묶음", s.last ? `${s.last.file} · ${s.last.mb} MB · ${s.last.created.replace("T", " ")}` : "없음"],
    ["진행", s.state === "running" ? `빌드 중 ${s.pct || 0}% · ${s.step || ""}` : s.state === "failed" ? `실패: ${s.error || ""}` : "—"],
  ]);
  els.buildDl.style.display = s.last ? "" : "none";  // .ib는 display를 정해 hidden 속성이 안 먹음
}

// ---- 운용/정비 모드(게이트웨이 GET·PUT /mode, docs/gateway.md 3장) ----------------------------------------
let modeCtx = null;

async function loadMode(ctx = modeCtx) {
  if (!ctx) return;
  modeCtx = ctx;
  let m;
  try { m = await ctx.api("GET", "/mode"); }
  catch {
    // 모드를 모르는 REST(Python 판): 카드를 숨김
    if (els.modeCard) els.modeCard.style.display = "none";
    return;
  }
  if (!els.modeCard) return;
  els.mode = m;
  els.modeCard.style.display = "";
  const maint = m.mode === "maintenance";
  els.modePill.set(maint ? "wrench" : "check", maint ? "정비 모드" : "운용 모드", maint ? "warn" : "ok");
  els.modeInfo.set([
    ["바꾼 시각", m.since ? m.since.replace("T", " ").slice(0, 19) : "시작한 뒤 그대로"],
    ["바꾼 사람", m.by || "—"],
  ]);
  const a = els.auth || ctx.state().auth;
  const canSet = !a || a.auth === "none" || a.role === "maintainer";
  els.modeBtn.style.display = canSet ? "" : "none";
  els.modeBtn.querySelector(".ib-label").textContent = maint ? "운용 모드로" : "정비 모드 켜기";
  els.modeBtn.className = "ib" + (maint ? " primary" : " danger");
}

async function toggleMode(ctx) {
  const toMaint = !(els.mode && els.mode.mode === "maintenance");
  const msg = toMaint
    ? "정비 모드로 바꿉니다. 장비를 하나씩 시험하는 명령이 허용됩니다. 켜고 끈 시각과 그동안의 명령은 모두 기록됩니다."
    : "운용 모드로 돌아갑니다.";
  if (!confirm(msg)) return;
  await ctx.act(toMaint ? "정비 모드 켜기" : "운용 모드로", () => ctx.api("PUT", "/mode", { mode: toMaint ? "maintenance" : "operation" }));
  loadMode(ctx);
}

async function load(ctx) {
  loadBuild(ctx);
  loadMode(ctx);
  const { h } = ctx;
  let r;
  try {
    r = await ctx.api("GET", "/system/workspace");
  } catch (e) {
    if (els.err) els.err.textContent = `보드에서 읽지 못함: ${e.message || e}`;
    return;
  }
  if (!els.summary) return;  // 그 사이 화면을 떠남
  els.err.textContent = "";
  const p = r.profile;
  const ws = r.workspace || {};
  if (!p) els.prof.set("sliders", "프로필 기록 없음", "idle");
  else if (p.guessed) els.prof.set("sliders", `프로필 ${p.name} (추정)`, "info");
  else els.prof.set("sliders", `프로필 ${p.name}`, "ok");

  const changed = (r.files || []).filter((f) => f.origin === "changed").map((f) => f.name);
  els.summary.set([
    ["프로필", p ? `${p.name}${p.guessed ? ` (기록 없음 · 보드 파일 ${p.matching_files}개가 이 프로필과 같아 추정)` : ""}` : "기록 없음"],
    ["프로필 폴더", p ? p.source : "—"],
    ["적용", p && !p.guessed ? `${p.applied_at}${p.reset_config ? " (--reset-config)" : ""}${p.workspace_commit ? `, 워크스페이스 ${p.workspace_commit}` : ""}` : "—"],
    ["워크스페이스", `${ws.root || "—"} · ${ws.branch || "?"} ${ws.commit || ""}${ws.dirty ? " (고친 파일 있음)" : ws.dirty === null ? " (고친 파일 확인 못 함)" : ""}`],
    ["데이터 폴더", r.data_dir || "—"],
    ["프로필과 다른 파일", changed.length ? changed.join(", ") : "없음"],
  ]);

  let group = null;
  els.env.replaceChildren(...(r.env || []).flatMap((e) => {
    const rows = [];
    if ((e.group || "") !== group) {  // 묶음 제목 줄(기본 workspace.env의 "## " 줄)
      group = e.group || "";
      if (group) rows.push(h("tr", {}, h("td", { colspan: 4, class: "muted", style: "padding-top:10px;font-weight:600" }, group)));
    }
    const differs = e.value != null && e.default != null && e.value !== e.default;
    const val = e.value != null ? e.value : (e.default != null ? `(기본 ${e.default})` : "(없음)");
    rows.push(h("tr", {},
      h("td", { class: "mono" }, e.key),
      h("td", { class: "mono" + (e.value == null ? " faint" : ""), style: differs ? "color:var(--warn)" : "" }, val),
      h("td", { class: "mono faint" }, e.default ?? (e.example ? `예: ${e.example}` : "—")),
      h("td", { class: "muted" }, e.desc || "—")));
    return rows;
  }));

  els.files.replaceChildren(...(r.files || []).map((f) => {
    const [icon, text, kind] = ORIGIN[f.origin] || ORIGIN.unknown;
    const badge = ctx.ui.pill(icon, text, kind);
    const head = h("summary", {}, h("span", { class: "mono" }, f.name), " ", badge,
      h("span", { class: "muted" }, ` ${FILE_DESC[f.name] || ""}`));
    const body = f.exists
      ? h("pre", { class: "log", style: "max-height:40vh" }, f.text + (f.truncated ? "\n… (64 KB까지만 표시)" : ""))
      : h("div", { class: "muted" }, "보드에 이 파일이 없습니다(시작할 때 기본값으로 만들거나, 쓰지 않는 기능).");
    const keep = els.open && els.open.has(f.name);
    const d = h("details", { class: "cfg-item", style: "margin-top:6px" }, head,
      h("div", { class: "muted", style: "margin:6px 0" }, f.path), body);
    if (keep) d.open = true;
    d.addEventListener("toggle", () => {
      els.open = els.open || new Set();
      if (d.open) els.open.add(f.name); else els.open.delete(f.name);
    });
    return d;
  }));
}
