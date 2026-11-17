// 워크스페이스 UI 틀: 왼쪽 패널 목록, 상단 상태줄, 주기 갱신, 패널 불러오기.
//
// 패널 = web/panels/<이름>/ 폴더 하나 (README "패널 추가").
//   panel.json  {"title", "group", "order", "worker_types"?: [워커 type...]}
//   panel.js    export default { mount(el, ctx), update(state, ctx), unmount?() }
// worker_types가 있으면 보드에 그 type 워커가 있을 때마다 왼쪽 목록에 항목이 하나씩 생기고,
// ctx.workerId로 어느 워커인지 받는다.

import * as ui from "/kit.js";

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

// 보드가 토큰을 요구하면(제품 모드 WORKSPACE_REST_TOKEN) 한 번 묻고 쿠키에 둔다. 스트림(EventSource)도 쿠키로 간다
let askedToken = false;
function needToken(r) {
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
  const text = await r.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = { error: { message: text } }; }
  if (!r.ok) {
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
async function mergeConfig(id, changes) {
  const cur = await api("GET", `/workers/${id}/config`);
  const cfg = { ...cur.config };
  for (const [k, v] of Object.entries(changes)) {
    if (v === null || v === undefined || v === "") delete cfg[k]; else cfg[k] = v;
  }
  return api("PUT", `/workers/${id}/config`, cfg);
}

function toast(msg, kind = "") {
  const t = document.createElement("div");
  t.className = "toast " + kind;
  t.textContent = msg;
  document.getElementById("toasts").appendChild(t);
  setTimeout(() => t.remove(), kind === "bad" ? 7000 : 3500);
}

// 명령 실행 + 결과 알림. 성공하면 바로 상태를 다시 읽는다
async function act(label, fn) {
  try { await fn(); toast(label + " 완료", "ok"); poll(); }
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
  } catch (e) {
    state = { ...state, connected: false, error: e.message };
  }
  renderTop();
  renderNav();
  if (!current && state.connected && panels.length) { route(); return; }
  if (current && current.mod.update) {
    try { current.mod.update(state, current.ctx); } catch (e) { console.error(e); }
  }
}

async function pollUi() {
  try { state.ui = await (await fetch("/api/ui")).json(); } catch { /* 로컬 서버 꺼짐 */ }
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

// ---- 왼쪽 목록 ---------------------------------------------------------------------------

function navItems() {
  const items = [];
  for (const p of panels) {
    if (p.worker_types) {
      for (const w of state.workers.filter((w) => p.worker_types.includes(w.type))) {
        items.push({ key: `${p.id}/${w.id}`, panel: p, workerId: w.id, label: w.id, sub: p.title,
                     dot: w.actual_state, icon: p.icon });
      }
    } else {
      items.push({ key: p.id, panel: p, label: p.title, icon: p.icon });
    }
  }
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
    const g = it.panel.group || "기타";
    if (g !== group) { nav.appendChild(h("div", { class: "group" }, g)); group = g; }
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
  const it = items.find((i) => i.key === key) || (key ? null : items[0]);
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
  const ctx = { workerId: it.workerId, panel: it.panel, api, remote, mergeConfig, act, toast, h, fmt,
                stateBadge, spark, ui, history: (id) => history[id] || [], state: () => state };
  current = { key: it.key, panel: it.panel, workerId: it.workerId, mod, ctx };
  navSig = "";
  renderNav();
  try { mod.mount(el, ctx); if (mod.update) mod.update(state, ctx); }
  catch (e) { el.appendChild(h("div", { class: "empty err" }, "패널 오류: " + e.message)); console.error(e); }
}

async function start() {
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
