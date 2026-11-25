// 워크스페이스 UI 틀: 왼쪽 패널 목록, 상단 상태줄, 주기 갱신, 패널 불러오기.
//
// 패널 = web/panels/<이름>/ 폴더 하나 (README "패널 추가").
//   panel.json  {"title", "group", "order", "worker_types"?: [워커 type...]}
//   panel.js    export default { mount(el, ctx), update(state, ctx), unmount?() }
// worker_types가 있으면 보드에 그 type 워커가 있을 때마다 왼쪽 목록에 항목이 하나씩 생기고,
// ctx.workerId로 어느 워커인지 받는다.

import * as ui from "/kit.js";
import * as sys from "/system.js";

const HISTORY = 180;          // 워커별 지표 기록 개수(그래프용)
const nav = document.getElementById("nav");
const main = document.getElementById("main");

let panels = [];              // panel.json 목록
const modules = {};           // id → 불러온 panel.js
let state = { system: null, workers: [], byId: {}, connected: false, error: null, ui: null };
const history = {};           // workerId → [{t, actual_state, metrics, resources}]
let current = null;           // {key, panel, workerId, mod, ctx}
let refreshMs = 1000;

// ---- 공용 도구 (패널이 ctx로 받는다) --------------------------------------------------

// 신분 확인(게이트웨이 GET /api/auth/me, docs/gateway.md 3장): {auth: "on"|"off", name, role, mode}.
// auth on인데 세션이 없으면 로그인 창을 먼저 띄운다. 이 엔드포인트가 없는 REST(Python 판)면 auth "none"
let auth = null;
const ROLE_KO = { viewer: "보기", operator: "조작", maintainer: "관리" };

async function loadAuth() {
  let r;
  try { r = await fetch("/api/auth/me"); } catch { return "down"; }
  if (r.status === 404) { auth = { auth: "none" }; return "ok"; }
  let data = null;
  try { data = await r.json(); } catch { /* 빈 답 */ }
  if (r.status === 401 && data && data.auth === "on") { auth = { auth: "on" }; return "login"; }
  if (r.status === 401) { auth = { auth: "none" }; return "ok"; }  // 토큰 판(WORKSPACE_REST_TOKEN): 아래 needToken이 묻는다
  if (r.status === 503) return "unavailable";  // 제품 모드인데 사용자가 없음
  if (!r.ok || !data) { auth = { auth: "none" }; return "ok"; }
  auth = data;
  return "ok";
}

// 세션이 끝났으면(12시간 쉼, 비밀번호 바꿈 등) 다시 불러와 로그인 창으로
function sessionLost(r) {
  if (r.status !== 401 || !auth || auth.auth !== "on") return false;
  location.reload();
  return true;
}

function showLogin(message) {
  nav.textContent = "";
  const name = h("input", { type: "text", autocomplete: "username", placeholder: "이름" });
  const pass = h("input", { type: "password", autocomplete: "current-password", placeholder: "비밀번호" });
  const note = h("div", { class: "hint", style: "margin-top:8px" }, message || "이 보드는 로그인해야 쓸 수 있습니다");
  const submit = async () => {
    if (!name.value || !pass.value) { note.textContent = "이름과 비밀번호를 넣으세요"; return; }
    let r;
    try {
      r = await fetch("/api/auth/login", { method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name: name.value.trim(), password: pass.value }) });
    } catch { note.textContent = "보드 응답 없음"; return; }
    if (r.ok) { location.reload(); return; }
    pass.value = "";
    note.textContent = r.status === 429 ? "여러 번 틀려 잠시 막혔습니다. 30초 뒤에 다시 하세요"
      : r.status === 401 ? "이름 또는 비밀번호가 틀립니다" : `로그인 실패 (${r.status})`;
  };
  for (const i of [name, pass]) i.addEventListener("keydown", (e) => { if (e.key === "Enter") submit(); });
  main.replaceChildren(h("div", { style: "max-width:380px; margin:60px auto" },
    ui.card("로그인", ui.formRows([["이름", name], ["비밀번호", pass]]), note,
      h("div", { class: "btnrow", style: "margin-top:12px; justify-content:flex-end" },
        ui.iconButton({ icon: "lock", label: "로그인", kind: "primary", onClick: submit })))));
  name.focus();
}

async function logout() {
  try { await fetch("/api/auth/logout", { method: "POST" }); } catch { /* 그래도 다시 불러옴 */ }
  location.reload();
}

// 머리: 개발·제품 표시(누를 것 없음), 사용자(역할)·로그아웃 또는 "인증 꺼짐"
let whoSig = "";
function renderWho() {
  const sig = JSON.stringify([auth, state.ui && state.ui.product]);
  if (sig === whoSig) return;
  whoSig = sig;
  const modeEl = document.getElementById("mode"), whoEl = document.getElementById("who");
  modeEl.replaceChildren();
  whoEl.replaceChildren();
  if (!auth || auth.auth === "none") return;
  if (auth.mode) {  // 배포 형태 표시(docs/modes.md): 워크스페이스 "개발", 제품 "제품", 운용 미리보기 "운용"
    const dev = auth.mode === "development";
    const label = dev ? "개발" : state.ui && state.ui.product ? "제품" : "운용";
    modeEl.appendChild(ui.pill(dev ? "wrench" : "check", label, dev ? "info" : "ok"));
  }
  if (auth.auth === "on" && auth.name) {
    whoEl.append(ui.pill("lock", `${auth.name} · ${ROLE_KO[auth.role] || auth.role}`, "info"),
      ui.iconButton({ icon: "power", title: "로그아웃", onClick: logout }));
  } else if (auth.auth === "off") {
    whoEl.appendChild(ui.pill("lock", "인증 꺼짐", "idle"));
  }
}

async function refreshAuth() {
  const prev = auth && auth.auth;
  const res = await loadAuth();
  if (res === "login" && prev === "on") { location.reload(); return; }  // 세션 끝남
  state.auth = auth;
  ui.setMode(auth && auth.mode);
  renderWho();
}

// 보드가 토큰을 요구하면(신분 확인 꺼짐 + WORKSPACE_REST_TOKEN) 한 번 묻고 쿠키에 둔다. 스트림(EventSource)도 쿠키로 간다
let askedToken = false;
function needToken(r) {
  if (sessionLost(r)) return true;
  if (r.status !== 401 || askedToken) return false;
  askedToken = true;
  const t = window.prompt("이 보드는 접근 토큰이 필요합니다. 토큰을 입력하세요");
  if (t) {
    document.cookie = `workspace_token=${encodeURIComponent(t.trim())}; path=/; SameSite=Strict`;
    location.reload();
  }
  return true;
}

async function api(method, path, body) {
  const r = await fetch("/api/board" + path, {
    method, headers: { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  needToken(r);
  // 워크스페이스(개발)는 명령 조건이 안 맞아도 보내고 알려 준다(docs/modes.md 3장)
  const adv = r.headers.get("X-Workspace-Advisory");
  if (adv) {
    let m = adv;
    try { m = JSON.parse(adv).message || adv; } catch { /* 그대로 */ }
    toast("운용이었으면 막힘: " + m, "warn");
  }
  const text = await r.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = { error: { message: text } }; }
  if (!r.ok) {
    // 운용 모드에서 막힘(docs/command-path.md 3장): 무엇을 하면 되는지 알려 준다
    if (r.status === 403 && data && data.need === "development") {
      const err = new Error(ui.DEV_ONLY_TITLE);
      err.detail = data.error;
      throw err;
    }
    const err = new Error((data && data.error && (data.error.message || data.error.code)) || r.status);
    err.detail = data && data.error;  // 설정 검사 오류는 detail.fields {경로: 메시지}
    throw err;
  }
  return data;
}

async function remote(name, params = {}) {
  const r = await fetch("/api/remote/" + name + "?" + new URLSearchParams(params));
  needToken(r);
  const data = await r.json();
  if (!r.ok || !data.ok) throw new Error(data.error || r.status);
  return data.lines;
}

// 보드는 설정 전체를 바꾼다(PUT). 지금 설정을 읽어 바꾼 값만 합쳐 보낸다. null이면 그 키를 지움(기본값)
// 키는 맨 위 키("watchdog_sec") 또는 모듈 칸 경로("gimbal.center_yaw" → config.gimbal.center_yaw). 모듈 칸 설정은
// 경로로 써야 한다: 맨 위에 쓰면 워커가 읽지 않는다(design.md 6.10 묶음 section)
function mergeChanges(config, changes) {
  const cfg = { ...config };
  for (const [key, v] of Object.entries(changes)) {
    const dot = key.indexOf(".");
    const del = v === null || v === undefined || v === "";
    if (dot < 0) {
      if (del) delete cfg[key]; else cfg[key] = v;
      continue;
    }
    const sec = key.slice(0, dot), sub = key.slice(dot + 1);
    const part = { ...(cfg[sec] && typeof cfg[sec] === "object" ? cfg[sec] : {}) };
    if (del) delete part[sub]; else part[sub] = v;
    if (Object.keys(part).length) cfg[sec] = part; else delete cfg[sec];
  }
  return cfg;
}

async function mergeConfig(id, changes) {
  const cur = await api("GET", `/workers/${id}/config`);
  return api("PUT", `/workers/${id}/config`, mergeChanges(cur.config, changes));
}

function toast(msg, kind = "") {
  const t = document.createElement("div");
  t.className = "toast " + kind;
  t.textContent = msg;
  document.getElementById("toasts").appendChild(t);
  setTimeout(() => t.remove(), kind === "ok" ? 3500 : 7000);
}

// 명령 실행 + 결과 알림. 성공하면 바로 상태를 다시 읽는다
async function act(label, fn) {
  try { await fn(); toast(label + " 완료", "ok"); poll(); refreshAuth(); }
  catch (e) { toast(label + " 실패: " + e.message, "bad"); }
}

function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "class") el.className = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (v !== false && v !== null && v !== undefined) el.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    el.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

function fmt(v, digits = 0, unit = "") {
  if (v === null || v === undefined || Number.isNaN(v)) return "—";
  return (typeof v === "number" ? v.toFixed(digits) : v) + unit;
}

function stateBadge(s) { return h("span", { class: "badge st-" + (s || "idle") }, s || "—"); }

// 작은 꺾은선. values: 숫자 배열(null은 끊김)
function spark(canvas, values, { min = 0, max = null, color = "#5aa9ff" } = {}) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, hgt = canvas.clientHeight;
  if (!w || !hgt) return;
  canvas.width = w * dpr; canvas.height = hgt * dpr;
  const g = canvas.getContext("2d");
  g.scale(dpr, dpr);
  g.clearRect(0, 0, w, hgt);
  const nums = values.filter((v) => typeof v === "number");
  if (!nums.length) return;
  const hi = max ?? Math.max(...nums, min + 1e-9);
  const lo = min ?? Math.min(...nums);
  const span = hi - lo || 1;
  g.strokeStyle = "#2a313b"; g.lineWidth = 1;
  g.beginPath(); g.moveTo(0, hgt - 0.5); g.lineTo(w, hgt - 0.5); g.stroke();
  g.strokeStyle = color; g.lineWidth = 1.5; g.beginPath();
  let pen = false;
  values.forEach((v, i) => {
    if (typeof v !== "number") { pen = false; return; }
    const x = (i / Math.max(values.length - 1, 1)) * w;
    const y = hgt - 2 - ((Math.min(Math.max(v, lo), hi) - lo) / span) * (hgt - 4);
    pen ? g.lineTo(x, y) : g.moveTo(x, y);
    pen = true;
  });
  g.stroke();
}

// ---- 상태 갱신 ---------------------------------------------------------------------------

async function poll() {
  try {
    const [system, workers] = await Promise.all([api("GET", "/system"), api("GET", "/workers")]);
    state = { ...state, system, workers, byId: Object.fromEntries(workers.map((w) => [w.id, w])),
              connected: true, error: null };
    const t = Date.now();
    for (const w of workers) {
      const hist = (history[w.id] ||= []);
      hist.push({ t, actual_state: w.actual_state, metrics: w.metrics, resources: w.resources });
      if (hist.length > HISTORY) hist.shift();
    }
    sys.sync(state, api);
  } catch (e) {
    state = { ...state, connected: false, error: e.message };
  }
  renderTop();
  renderHealth();
  renderNav();
  if (!current && state.connected && panels.length) { route(); return; }
  if (current && current.mod.update) {
    try { current.mod.update(state, current.ctx); } catch (e) { console.error(e); }
  }
}

async function pollUi() {
  try { state.ui = await (await fetch("/api/ui")).json(); } catch { /* 로컬 서버 꺼짐 */ }
  if (auth && auth.auth !== "none") refreshAuth();  // 모드(다른 사람이 바꿨을 수도)·세션
  // 화면 제목: 보드 REST의 WORKSPACE_UI_TITLE (기본 Workspace)
  const title = (state.ui && state.ui.title) || "Workspace";
  if (document.title !== title) {
    document.title = title;
    document.getElementById("brand-name").textContent = title;
    document.getElementById("brand-mark").textContent = title.charAt(0).toUpperCase();
  }
}

let topPills = null;
function renderTop() {
  if (!topPills) {
    topPills = { link: ui.pill("link", "연결 확인 중", "idle") };
    document.getElementById("link").replaceChildren(topPills.link);
  }
  if (state.connected) topPills.link.set("check", "보드 연결됨", "ok");
  else topPills.link.set("alert", "보드 응답 없음", "bad");
  const d = new Date();
  document.getElementById("clock").textContent = [d.getHours(), d.getMinutes(), d.getSeconds()]
    .map((n) => String(n).padStart(2, "0")).join(":");
}

// 건강 띠(ui/system-panel.md 4장): status-manager의 system 주제 health + fault 주제의 이상 수. 누르면 워크스페이스 › 개요.
// status-manager가 없으면 띠 없음
let healthEls = null;
function renderHealth() {
  const box = document.getElementById("health");
  if (!box) return;
  const s = sys.get("system");
  if (!s) { if (healthEls) { box.replaceChildren(); healthEls = null; } return; }
  if (!healthEls) {
    healthEls = { main: ui.pill("info", "상태 모름", "idle"), faults: ui.pill("alert", "", "warn") };
    const a = h("a", { href: panels.some((p) => p.id === "workspace") ? "#/workspace/overview" : null, class: "health-band", title: "" }, healthEls.main, healthEls.faults);
    healthEls.link = a;
    box.replaceChildren(a);
  }
  const t = state.connected && s.state === "running" && s.up ? s.topic : null;
  if (!t) {
    healthEls.main.set("info", "상태 모름", "idle");
    healthEls.link.title = s.state !== "running" ? `${s.worker} ${s.state || "꺼짐"}` : "스트림 기다리는 중";
  } else {
    const [icon, text, kind] = sys.HEALTH[t.health] || ["info", t.health, "idle"];
    const n = (t.reasons || []).length;
    healthEls.main.set(icon, text + (t.health !== "ok" && n ? ` · ${n}` : ""), kind);
    healthEls.link.title = (t.reasons || []).join("\n") || "모든 워커 정상";
  }
  const f = sys.get("fault");
  const ft = f && f.up ? f.topic : null;
  const count = ft && ft.active ? Object.keys(ft.active).length : 0;
  if (count) {
    const [, kind] = sys.SEVERITY[ft.worst] || ["", "warn"];
    healthEls.faults.set("alert", `이상 ${count}`, kind);
    healthEls.faults.style.display = "";
  } else {
    healthEls.faults.style.display = "none";
  }
}

// ---- 왼쪽 목록 ---------------------------------------------------------------------------

// 자동 상태 화면(panels/auto): 자기 화면이 없는 워커를 워커 계약(io/self.json)대로 그린다. panel.json이 없어
// 공용 목록에는 안 나오고, 여기서 그런 워커마다 항목을 만든다. 기본 탑재 워커(시스템 모니터 등)는 뺀다
const AUTO_PANEL = { id: "auto", title: "상태(자동)", group: "페이로드", icon: "list" };

// 왼쪽 목록(ui/workspace-screen.md): 맨 위(묶음 제목 없음) = 공용 패널(시스템 모니터, 워크스페이스), 묶음 "페이로드" =
// 워커 패널(들어간 모듈)과 자동 화면, "개발 도구" = 개발할 때만 쓰는 워커 패널. 묶음은 이 셋뿐(panel.json group)
const GROUPS = ["", "페이로드", "개발 도구"];
function groupOf(p) {
  if (p.id === "auto") return "페이로드";
  if (GROUPS.includes(p.group || "")) return p.group || "";
  console.warn(`panel ${p.id}: group "${p.group}" is not one of 페이로드, 개발 도구; using 페이로드`);
  return "페이로드";
}

function navItems() {
  const items = [];
  const covered = new Set();
  for (const p of panels) {
    if (p.worker_types) {
      for (const w of state.workers.filter((w) => p.worker_types.includes(w.type))) {
        covered.add(w.id);
        items.push({ key: `${p.id}/${w.id}`, panel: p, workerId: w.id, label: w.id, sub: p.title,
                     dot: w.actual_state, icon: p.icon });
      }
    } else {
      items.push({ key: p.id, panel: p, label: p.title, icon: p.icon });
    }
  }
  for (const w of state.workers) {
    if (covered.has(w.id) || w.builtin) continue;
    items.push({ key: `auto/${w.id}`, panel: AUTO_PANEL, workerId: w.id, label: w.id, sub: AUTO_PANEL.title,
                 dot: w.actual_state, icon: AUTO_PANEL.icon });
  }
  // 묶음 순서대로(같은 묶음 안은 panel.json order 순서 그대로, sort는 안정 정렬)
  for (const it of items) it.group = groupOf(it.panel);
  items.sort((a, b) => GROUPS.indexOf(a.group) - GROUPS.indexOf(b.group));
  return items;
}

let navSig = "";
function renderNav() {
  const items = navItems();
  const sig = JSON.stringify(items.map((i) => [i.key, i.dot])) + (current && current.key);
  if (sig === navSig) return;
  navSig = sig;
  nav.textContent = "";
  let group = null;
  for (const it of items) {
    const g = it.group;
    if (g !== group) { if (g) nav.appendChild(h("div", { class: "group" }, g)); group = g; }
    // 항목: 아이콘(panel.json icon) + 이름(워커면 id, 아래에 패널 이름) + 워커 상태 점
    nav.appendChild(h("a", { href: "#/" + it.key, class: current && current.key === it.key ? "active" : "" },
      h("span", { class: "nav-i" }, ui.icon(it.icon || "info")),
      h("span", { class: "nav-t" }, h("span", { class: "nav-l" }, it.label),
        it.sub && it.workerId ? h("span", { class: "nav-s" }, it.sub) : null),
      it.dot !== undefined ? h("span", { class: "dot " + (it.dot || ""), title: it.dot || "" }) : null));
  }
}

// ---- 패널 전환 ---------------------------------------------------------------------------

async function loadModule(id) {
  if (!modules[id]) modules[id] = (await import(`/panels/${id}/panel.js`)).default;
  return modules[id];
}

let routeSeq = 0;
async function route() {
  const seq = ++routeSeq;  // 빠르게 연달아 바뀌면 마지막 것만 그린다
  // "#/<패널>/<워커>?view=..." — 물음표 뒤는 패널이 읽는 값(예: 시스템 모니터 보기), 경로가 아님
  const key = decodeURIComponent(location.hash.replace(/^#\//, "").split("?")[0]);
  const items = navItems();
  // "#/<패널>/<탭>"(예 #/workspace/logs): 워커 패널이 아니면 뒤는 패널이 읽는 탭(ctx.sub)
  const head = key.split("/")[0], sub = key.includes("/") ? key.slice(head.length + 1) : "";
  const it = items.find((i) => i.key === key) || items.find((i) => i.key === head && !i.workerId) || (key ? null : items[0]);
  if (current && current.mod.unmount) { try { current.mod.unmount(current.ctx); } catch {} }
  current = null;
  if (!it) {
    // 워커 패널인데 아직 워커 목록이 없으면(보드 연결 전) poll()이 다시 시도한다
    main.replaceChildren(h("div", { class: "empty" }, state.connected ? "없는 항목" : "보드 연결 대기 중"));
    renderNav();
    return;
  }
  let mod;
  try { mod = await loadModule(it.panel.id); }
  catch (e) {
    if (seq === routeSeq) main.replaceChildren(h("div", { class: "empty err" }, `패널 ${it.panel.id} 불러오기 실패: ${e.message}`));
    return;
  }
  if (seq !== routeSeq) return;
  const el = h("div");
  main.replaceChildren(el);
  const ctx = { workerId: it.workerId, panel: it.panel, sub: it.key === key ? "" : sub, api, remote, mergeConfig, act, toast, h, fmt,
                stateBadge, spark, ui, history: (id) => history[id] || [], state: () => state };
  current = { key: it.key, panel: it.panel, workerId: it.workerId, mod, ctx };
  navSig = "";
  renderNav();
  try { mod.mount(el, ctx); if (mod.update) mod.update(state, ctx); }
  catch (e) { el.appendChild(h("div", { class: "empty err" }, "패널 오류: " + e.message)); console.error(e); }
}

async function start() {
  const a = await loadAuth();
  if (a === "login") { showLogin(); return; }
  if (a === "unavailable") {
    main.replaceChildren(h("div", { class: "empty err" },
      "이 보드에는 사용자가 없어 접속할 수 없습니다. 보드에서 gateway user add로 사용자를 만드세요"));
    return;
  }
  state.auth = auth;
  ui.setMode(auth && auth.mode);
  renderWho();
  await pollUi();
  ui.setProduct(state.ui && state.ui.product);  // 제품 모드: 패널을 띄우기 전에(kit.js 제품 모드)
  refreshMs = (state.ui && state.ui.refresh_ms) || 1000;
  const pr = await fetch("/api/panels");
  if (needToken(pr)) return;  // 토큰을 넣으면 다시 불러온다
  panels = await pr.json();
  await poll();
  window.addEventListener("hashchange", route);
  await route();
  setInterval(poll, refreshMs);
  setInterval(pollUi, 5000);
}

start();
