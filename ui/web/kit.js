// Workspace UI 공통 부품. 모든 패널이 같은 모양·동작을 쓰도록 여기서만 만든다 (README "패널 디자인 규칙").
// 패널은 ctx.ui로 받는다. 스타일은 shell.css의 "공통 부품" 절.
//
//   icon(name)                     아이콘(SVG). 이름은 아래 ICONS
//   iconButton(opts)               아이콘 버튼. 글자 대신 쓰고 이름은 title(마우스 올리면 표시)
//   holdButton(opts)               누르고 있는 동안 동작(onStart), 떼면 정지(onStop)
//   tapHoldButton(opts)            짧게 누름 = onTap(미세 조정), 길게 = onHoldStart/onHoldStop(연속)
//   pill(icon, text, kind)         상태 배지. kind: ok | warn | bad | info | idle
//   iconTag(icon, text, tone)      표 안의 아이콘 + 짧은 글자(송신·수신 등). tone은 pill과 같은 색
//   readout(label, unit)           큰 숫자 + 작은 이름표. .set(value, digits)
//   slider(opts)                   값 슬라이더(양끝 아이콘·버튼). 끄는 동안은 상태 갱신이 값을 덮지 않는다
//   jog(opts)                      가운데로 돌아오는 슬라이더. 왼쪽/오른쪽으로 끄는 동안 onDir(-1|1), 놓으면 onDir(0)
//   segmented(options, onPick)     아이콘 묶음 선택. .set(value)
//   dialog(title, ...children)     대화창. .show() .close(), 열림 여부 .open
//   kvList(rows)                   [[이름, 값]...] 작은 키·값 목록
//   media(opts)                    16:9 영상 틀(iframe). .setUrl(url) .setMessage(text), onPoint(xr, yr) 클릭 위치(0~1)
//   panelHead(opts)                패널 머리: 제목, 배지 자리, 오른쪽 아이콘 버튼 자리
//   formRows(rows)                 [[이름, 입력 요소, 단위?]...] 설정 폼
//   block(title, ...children)      오른쪽 열(side)의 제목 달린 묶음. 제목은 짧게(예: "짐벌", "줌 · 초점")
//   section(title, ...children)    대화창 안의 제목 달린 묶음
//   card(title, ...children)       본문의 제목 달린 판(표·목록·그래프)
//   tile(label, unit)              요약 숫자 판. .set(value, digits). tiles(...)로 한 줄에 배치
//   meter(label)                   가로 막대 게이지(이름, 막대, 값 글자). .set(pct 0~100, text). 70%↑ 주황, 90%↑ 빨강
//   download(name, text, type)     파일로 내려받기(CSV 등)
//   workerPill(state)              워커 상태 배지(running·starting·stopped·exited·error·disabled). .set(state)
//   workerControls(ctx, id, opts)  시작/정지 아이콘 버튼(목록에서는 {autostart: true}로 자동 시작 버튼도), 가짜 장치 모드면 "가상". .update(worker)
//   autostartRow(ctx, id)          설정 창의 "보드를 켤 때 자동 시작" 줄. .update(worker)
//   toggleRow(label, hint, onChange), actions(...)  설정 창의 스위치 줄, 아래쪽 버튼 줄
//   workerSettings(ctx, id, opts)  워커 설정 창: 자동 시작 + 워커 스키마(config_schema)로 만든 설정 폼(design.md 6.10).
//                                  opts.before/after: 패널 전용 묶음(section). .show() .update(worker) .dispose()
//   settingsButton(dlg, title)     설정 창을 여는 아이콘 버튼. 패널은 iconButton 대신 이것을 쓴다(제품 모드면 null)
//   setProduct(on), isProduct()    제품 모드(셸이 /api/ui "product"로 정함): 설정 창·프리셋·워커 시작/정지·자동 시작을 그리지 않음

const SVG = "http://www.w3.org/2000/svg";

// 24x24, 선 굵기는 CSS(.icon)에서. 선(path d) 또는 [태그, 속성] 목록
export const ICONS = {
  up: "M6 15l6-6 6 6",
  down: "M6 9l6 6 6-6",
  left: "M15 6l-6 6 6 6",
  right: "M9 6l6 6-6 6",
  center: [["circle", { cx: 12, cy: 12, r: 3 }], ["path", { d: "M12 2v4M12 18v4M2 12h4M18 12h4" }]],
  lookDown: "M12 4v11M7 10l5 5 5-5M5 20h14",
  angle: "M4 19h16M4 19L15 6M9.5 19a6 6 0 0 0-1.6-4.2",
  zoomIn: [["circle", { cx: 10, cy: 10, r: 6 }], ["path", { d: "M20 20l-5.5-5.5M7.5 10h5M10 7.5v5" }]],
  zoomOut: [["circle", { cx: 10, cy: 10, r: 6 }], ["path", { d: "M20 20l-5.5-5.5M7.5 10h5" }]],
  focus: [["circle", { cx: 12, cy: 12, r: 3 }], ["path", { d: "M4 8V5h3M17 4h3v4M20 16v3h-3M7 20H4v-3" }]],
  near: [["circle", { cx: 12, cy: 9, r: 3.5 }], ["path", { d: "M12 12.5V20M8 17c2 0 4 1 4 3M16 17c-2 0-4 1-4 3" }]],
  far: "M3 19l6-9 4 5 3-3.5 5 7.5z",
  lock: [["rect", { x: 5.5, y: 11, width: 13, height: 9, rx: 2 }], ["path", { d: "M8.5 11V8a3.5 3.5 0 0 1 7 0v3" }]],
  follow: [["circle", { cx: 6, cy: 18, r: 2 }], ["circle", { cx: 18, cy: 6, r: 2 }], ["path", { d: "M8 18h6a3 3 0 0 0 0-6h-4a3 3 0 0 1 0-6h6" }]],
  eye: [["path", { d: "M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z" }], ["circle", { cx: 12, cy: 12, r: 3 }]],
  camera: [["path", { d: "M4 8h3l2-3h6l2 3h3v11H4z" }], ["circle", { cx: 12, cy: 13, r: 3.5 }]],
  record: [["circle", { cx: 12, cy: 12, r: 6, fill: "currentColor" }]],
  recordStop: [["rect", { x: 7, y: 7, width: 10, height: 10, rx: 1.5, fill: "currentColor" }]],
  info: [["circle", { cx: 12, cy: 12, r: 9 }], ["path", { d: "M12 11v5.5M12 7.5h.01" }]],
  sliders: [["path", { d: "M4 7h16M4 12h16M4 17h16" }], ["circle", { cx: 15, cy: 7, r: 2, fill: "var(--panel)" }],
    ["circle", { cx: 8, cy: 12, r: 2, fill: "var(--panel)" }], ["circle", { cx: 17, cy: 17, r: 2, fill: "var(--panel)" }]],
  tool: "M14.5 6.5a4 4 0 0 0-5.3 5.3L4 17v3h3l5.2-5.2a4 4 0 0 0 5.3-5.3l-2.6 2.6-2.4-.6-.6-2.4z",
  play: "M8 5l11 7-11 7z",
  plus: "M12 5v14M5 12h14",
  undo: "M9 14L4 9l5-5M4 9h10.5a5.5 5.5 0 0 1 0 11H11",
  stop: [["rect", { x: 6, y: 6, width: 12, height: 12, rx: 1.5 }]],
  power: "M12 3v8M6.5 6.5a8 8 0 1 0 11 0",
  video: "M4 7h11v10H4zM15 10.5l5-3v9l-5-3",
  videoOff: "M4 7h11v10H4zM15 10.5l5-3v9l-5-3M3 3l18 18",
  close: "M6 6l12 12M18 6L6 18",
  link: "M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1",
  broadcast: [["circle", { cx: 12, cy: 12, r: 2 }], ["path", { d: "M8.5 15.5a5 5 0 0 1 0-7M15.5 8.5a5 5 0 0 1 0 7M5.6 18.4a9 9 0 0 1 0-12.8M18.4 5.6a9 9 0 0 1 0 12.8" }]],
  check: "M5 12.5l4.5 4.5L19 7.5",
  alert: "M12 4l9 16H3zM12 10v4.5M12 17.5h.01",
  refresh: "M20 11a8 8 0 1 0-2.3 5.7M20 5v6h-6",
  external: "M14 4h6v6M20 4l-9 9M18 14v5H5V6h5",
  trash: "M5 7h14M10 7V4h4v3M7 7l1 13h8l1-13",
  clock: [["circle", { cx: 12, cy: 12, r: 8.5 }], ["path", { d: "M12 7.5V12l3 2" }]],
  chart: "M4 20V4M4 20h16M8 16v-5M12 16V8M16 16v-3",
  log: "M6 3h9l4 4v14H6zM9 9h3M9 13h7M9 17h7",
  home: "M4 11l8-7 8 7v9h-5v-6H9v6H4z",
  pin: [["path", { d: "M12 21s-6-5.6-6-11a6 6 0 0 1 12 0c0 5.4-6 11-6 11z" }], ["circle", { cx: 12, cy: 10, r: 2 }]],
  list: "M9 6h11M9 12h11M9 18h11M4.5 6h.01M4.5 12h.01M4.5 18h.01",
  gimbal: [["circle", { cx: 12, cy: 13, r: 4.5 }], ["circle", { cx: 12, cy: 13, r: 1.5 }], ["path", { d: "M7 4h10M12 4v4.5M5 21h14" }]],
  wrench: "M14.5 6.5a4 4 0 0 0-5.3 5.3L4 17v3h3l5.2-5.2a4 4 0 0 0 5.3-5.3l-2.6 2.6-2.4-.6-.6-2.4z",
  search: [["circle", { cx: 11, cy: 11, r: 6 }], ["path", { d: "M20 20l-4.5-4.5" }]],
  overlay: [["rect", { x: 3, y: 5, width: 18, height: 14, rx: 2 }], ["path", { d: "M6 16h6M12 9v3l2 1" }]],
  cpu: [["rect", { x: 7, y: 7, width: 10, height: 10, rx: 1.5 }], ["path", { d: "M10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4" }]],
  // 통신 방향(이벤트·로그): 송신 위쪽 화살표, 수신 아래쪽 화살표
  tx: "M12 19V5M6 11l6-6 6 6",
  rx: "M12 5v14M6 13l6 6 6-6",
  send: "M4 12l16-8-6 16-3-7z M11 13l9-9",
  message: "M4 5h16v11H9l-5 4z",
};

export function icon(name, size) {
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("class", "icon");
  svg.setAttribute("aria-hidden", "true");
  if (size) svg.style.width = svg.style.height = size + "px";
  const def = ICONS[name] || ICONS.info;
  const parts = typeof def === "string" ? [["path", { d: def }]] : def;
  for (const [tag, attrs] of parts) {
    const el = document.createElementNS(SVG, tag);
    for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
    svg.appendChild(el);
  }
  return svg;
}

function el(tag, cls, ...children) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    e.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return e;
}

// { icon, title, onClick, kind: ""|"primary"|"danger"|"accent", size: ""|"lg", label?: 짧은 글자(아이콘 옆) }
export function iconButton({ icon: name, title, onClick, kind = "", size = "", label } = {}) {
  const b = el("button", ["ib", kind, size].filter(Boolean).join(" "), icon(name), label ? el("span", "ib-label", label) : null);
  b.type = "button";
  if (title) { b.title = title; b.setAttribute("aria-label", title); }
  if (onClick) b.addEventListener("click", onClick);
  b.setOn = (on) => b.classList.toggle("on", !!on);
  return b;
}

// 누르는 동안 onStart, 떼거나 벗어나면 onStop (줌·초점처럼 연속 동작)
export function holdButton({ icon: name, title, onStart, onStop, kind = "", size = "" } = {}) {
  const b = iconButton({ icon: name, title, kind, size });
  let active = false;
  const stop = () => { if (!active) return; active = false; b.classList.remove("on"); onStop && onStop(); };
  b.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    b.setPointerCapture(e.pointerId);
    active = true;
    b.classList.add("on");
    onStart && onStart();
  });
  for (const ev of ["pointerup", "pointercancel", "lostpointercapture"]) b.addEventListener(ev, stop);
  return b;
}

// 짧게 누르면 onTap(미세 조정), holdMs 넘게 누르고 있으면 onHoldStart, 떼면 onHoldStop (줌·초점 끝 버튼, 방향 버튼과 같은 방식)
export function tapHoldButton({ icon: name, title, onTap, onHoldStart, onHoldStop, holdMs = 300, kind = "", size = "" } = {}) {
  const b = iconButton({ icon: name, title, kind, size });
  let timer = null, holding = false, down = false;
  const end = (fire) => {
    if (!down) return;
    down = false;
    clearTimeout(timer);
    b.classList.remove("on");
    if (holding) { holding = false; onHoldStop && onHoldStop(); }
    else if (fire) onTap && onTap();
  };
  b.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    b.setPointerCapture(e.pointerId);
    down = true;
    b.classList.add("on");
    timer = setTimeout(() => { holding = true; onHoldStart && onHoldStart(); }, holdMs);
  });
  b.addEventListener("pointerup", () => end(true));
  b.addEventListener("pointercancel", () => end(false));
  b.addEventListener("lostpointercapture", () => end(false));
  return b;
}

export function pill(name, text, kind = "idle") {
  const p = el("span", "pill " + kind, name ? icon(name) : null, text ? el("span", "", text) : null);
  p.set = (n, t, k) => { p.replaceChildren(...[n ? icon(n) : null, t ? el("span", "", t) : null].filter(Boolean)); p.className = "pill " + (k || "idle"); };
  return p;
}

// 표 안의 아이콘 + 짧은 글자(테두리 없는 작은 표시). tone: ok | info | warn | bad | idle (색)
export function iconTag(name, text, tone = "idle") {
  return el("span", "itag " + tone, icon(name), text ? el("span", "", text) : null);
}

export function readout(label, unit = "") {
  const v = el("span", "ro-v", "—");
  const r = el("div", "ro", v, el("span", "ro-k", label));
  r.set = (value, digits = 1, u = unit) => {
    if (value === null || value === undefined || Number.isNaN(value)) { v.textContent = "—"; return; }
    v.replaceChildren(typeof value === "number" ? value.toFixed(digits) : String(value), u ? el("small", "", u) : "");
  };
  return r;
}

// { min, max, step, value, left, right: 아이콘 이름 또는 요소, format(v), onChange(v) 놓았을 때, onInput(v) }
export function slider({ min = 0, max = 100, step = 1, value = min, left, right, format, onChange, onInput } = {}) {
  const input = el("input", "range");
  Object.assign(input, { type: "range", min, max, step, value });
  const out = el("span", "sl-v", format ? format(Number(value)) : value);
  let dragging = false;
  input.addEventListener("pointerdown", () => { dragging = true; });
  input.addEventListener("input", () => { out.textContent = format ? format(Number(input.value)) : input.value; onInput && onInput(Number(input.value)); });
  input.addEventListener("change", () => { dragging = false; onChange && onChange(Number(input.value)); });
  const side = (x) => (typeof x === "string" ? el("span", "sl-i", icon(x)) : x || null);
  const box = el("div", "sl", side(left), input, side(right), out);
  box.input = input;
  // 상태 갱신용. 끄는 중이면 덮지 않는다
  box.set = (v, maxV) => {
    if (maxV !== undefined && Number(input.max) !== maxV) input.max = maxV;
    if (dragging || v === null || v === undefined) return;
    input.value = v;
    out.textContent = format ? format(Number(v)) : v;
  };
  return box;
}

// 가운데(0)로 돌아오는 슬라이더. 위치가 아니라 방향·속도를 주는 조작(위치를 알 수 없는 수동 초점 등)
export function jog({ left, right, onDir, dead = 0.25 } = {}) {
  const input = el("input", "range jog");
  Object.assign(input, { type: "range", min: -1, max: 1, step: 0.01, value: 0 });
  let dir = 0;
  const emit = (d) => { if (d !== dir) { dir = d; onDir && onDir(d); } };
  input.addEventListener("input", () => {
    const v = Number(input.value);
    emit(v <= -dead ? -1 : v >= dead ? 1 : 0);
  });
  const release = () => { input.value = 0; emit(0); };
  for (const ev of ["change", "pointerup", "pointercancel", "blur"]) input.addEventListener(ev, release);
  const side = (x) => (typeof x === "string" ? el("span", "sl-i", icon(x)) : x || null);
  const box = el("div", "sl", side(left), input, side(right));
  box.input = input;
  return box;
}

// options: [{ value, icon, title }]
export function segmented(options, onPick) {
  const box = el("div", "seg");
  const buttons = options.map((o) => {
    const b = iconButton({ icon: o.icon, title: o.title, onClick: () => onPick(o.value) });
    b.dataset.value = o.value;
    box.appendChild(b);
    return b;
  });
  box.set = (v) => { for (const b of buttons) b.classList.toggle("on", b.dataset.value === v); };
  box.buttons = buttons;
  return box;
}

export function dialog(title, ...children) {
  const close = iconButton({ icon: "close", title: "닫기", onClick: () => d.close() });
  const body = el("div", "dlg-body", ...children);
  const d = el("dialog", "dlg", el("div", "dlg-head", el("span", "dlg-title", title), close), body);
  d.addEventListener("click", (e) => { if (e.target === d) d.close(); });  // 바깥을 누르면 닫힘
  document.body.appendChild(d);
  d.body = body;
  d.show = () => { if (!d.open) d.showModal(); };  // 열려 있는지는 d.open(기본 속성)
  d.dispose = () => d.remove();
  return d;
}

export function kvList(rows = []) {
  const box = el("div", "kvl");
  box.set = (rs) => box.replaceChildren(...rs.flatMap(([k, v]) => [el("span", "kvl-k", k), el("span", "kvl-v", v ?? "—")]));
  box.set(rows);
  return box;
}

// { title, message } → .setUrl(url) .setMessage(text) .onPoint = (xr, yr) => {}
export function media({ title = "영상" } = {}) {
  const frame = el("iframe");
  frame.setAttribute("allow", "autoplay; fullscreen");
  frame.title = title;
  const msg = el("div", "media-msg");
  const hit = el("div", "media-hit");
  const mark = el("div", "media-mark");
  const box = el("div", "media", frame, msg, hit, mark);
  box.onPoint = null;
  hit.addEventListener("click", (e) => {
    if (!box.onPoint) return;
    const r = hit.getBoundingClientRect();
    const xr = (e.clientX - r.left) / r.width, yr = (e.clientY - r.top) / r.height;
    mark.style.left = xr * 100 + "%";
    mark.style.top = yr * 100 + "%";
    mark.classList.remove("show"); void mark.offsetWidth; mark.classList.add("show");
    box.onPoint(xr, yr);
  });
  box.setUrl = (url) => {
    const want = url || "about:blank";
    if (frame.getAttribute("src") !== want) frame.src = want;
    frame.style.visibility = url ? "visible" : "hidden";
    hit.style.display = url && box.onPoint ? "block" : "none";
  };
  box.setMessage = (t) => { msg.textContent = t || ""; };
  box.stop = () => { frame.src = "about:blank"; };
  return box;
}

// { title, badges: [요소], actions: [요소] }
export function panelHead({ title, badges = [], actions = [] } = {}) {
  // 제품 모드: 설정·시작 버튼이 빠지므로 구분선(ph-sep)도 뺀다
  if (PRODUCT) actions = actions.filter((a) => a && !(a.classList && a.classList.contains("ph-sep")));
  const b = el("div", "ph-badges", ...badges);
  const a = el("div", "ph-actions", ...actions);
  const head = el("div", "ph", el("span", "ph-title", title), b, a);
  head.badges = b;
  head.actions = a;
  return head;
}

export function formRows(rows) {
  return el("div", "frm", ...rows.flatMap(([label, input, unit]) =>
    [el("label", "frm-k", label), el("div", "frm-v", input, unit ? el("span", "frm-u", unit) : null)]));
}

export function block(title, ...children) {
  return el("div", "blk", title ? el("div", "blk-t", title) : null, ...children);
}

// title: 문자열 또는 { title, hint, danger }. hint는 제목 아래 한 줄 설명, danger는 위험한 동작 묶음(붉은 테두리)
export function section(title, ...children) {
  const o = typeof title === "string" ? { title } : title || {};
  return el("div", "dsec" + (o.danger ? " danger" : ""),
    el("div", "dsec-h", el("div", "dsec-t", o.title), o.hint ? el("div", "dsec-d", o.hint) : null),
    el("div", "dsec-b", ...children));
}

// 설정 창 아래쪽 버튼 줄(오른쪽 정렬)
export function actions(...children) {
  return el("div", "dsec-a", ...children);
}

// 켜기·끄기 한 줄: 이름 + 설명 + 스위치. onChange(checked)
export function toggleRow(label, hint, onChange) {
  const input = el("input");
  input.type = "checkbox";
  input.addEventListener("change", () => onChange && onChange(input.checked));
  const row = el("label", "tgl", el("span", "tgl-t", el("span", "tgl-l", label), hint ? el("span", "tgl-d", hint) : null),
    el("span", "switch", input));
  row.input = input;
  row.set = (v) => { input.checked = !!v; };
  return row;
}

// 워커 "보드를 켤 때 자동 시작" 설정 줄. update(state)에서 .update(worker)
export function autostartRow(ctx, id) {
  const row = toggleRow("보드를 켤 때 자동 시작", "보드 전원을 켜면 이 워커를 바로 시작합니다",
    (on) => ctx.act("자동 시작 변경", () => ctx.api("PUT", `/workers/${id}/autostart`, { autostart: on })));
  row.update = (w) => { if (w && document.activeElement !== row.input) row.set(w.autostart); };
  return row;
}

export function card(title, ...children) {
  return el("div", "card2", title ? el("div", "card2-t", title) : null, ...children);
}

export function tile(label, unit = "") {
  const r = readout(label, unit);
  r.classList.add("tile2");
  return r;
}

export function tiles(...items) {
  return el("div", "tiles", ...items);
}

const WORKER_STATE = {
  running: ["check", "실행 중", "ok"], starting: ["refresh", "시작 중", "info"], stopped: ["stop", "정지", "idle"],
  exited: ["alert", "재시작 대기", "warn"], error: ["alert", "오류", "bad"], disabled: ["stop", "사용 안 함", "idle"],
};
export function workerPill(state) {
  const p = pill(null, "", "idle");
  p.set = ((orig) => (st) => { const [n, t, k] = WORKER_STATE[st] || ["info", st || "—", "idle"]; orig(n, t, k); })(p.set);
  p.set(state);
  return p;
}

// ---- 제품 모드(보드 REST WORKSPACE_PRODUCT → /api/ui "product", design.md 11.4) -----------------------------
// 제품 화면은 기본 화면(시스템 모니터 + 워커 패널)만. 설정 창·프리셋·워커 시작/정지·자동 시작 부품은 그리지 않는다
// (설정은 잠겨 있고 워커는 서비스가 항상 띄운다). 셸이 패널을 띄우기 전에 setProduct를 부른다
let PRODUCT = false;
export function setProduct(on) { PRODUCT = !!on; }
export function isProduct() { return PRODUCT; }

// 워커 설정 창을 여는 버튼(패널 머리·워커 표). 제품 모드면 null(panelHead·h가 null을 건너뜀)
export function settingsButton(dlg, title = "설정") {
  if (PRODUCT) return null;
  return iconButton({ icon: "sliders", title, onClick: () => dlg.show() });
}

// 워커 머리의 공통 버튼: 자동 시작(전원 아이콘, 켜지면 강조), 시작/정지. 제품 모드면 빈 칸
export function workerControls(ctx, id, { startLabel = "워커 시작", stopLabel = "워커 정지", autostart = false } = {}) {
  if (PRODUCT) {
    const none = el("span", null);
    none.update = () => {};
    return none;
  }
  const auto = iconButton({ icon: "power", title: "보드를 켤 때 자동 시작" });
  const run = iconButton({ icon: "play" });
  // 가짜 장치 모드(워커 상태 sim, design.md 6.6.8): 실기와 헷갈리지 않게 머리에 "가상"
  const sim = el("span", "sim-badge", "가상");
  sim.title = "가짜 장치 모드: 실제 장비가 아니라 가짜 장치(시험용)로 돌고 있음. 설정 창에서 프리셋을 바꾸면 실기로";
  sim.hidden = true;
  const box = el("span", "seg", sim, autostart ? auto : null, run);
  box.update = (w) => {
    if (!w) return;
    sim.hidden = !(w.sim && w.actual_state === "running");
    const running = w.desired_state === "running";
    run.replaceChildren(icon(running ? "stop" : "play"));
    run.title = running ? stopLabel : startLabel;
    run.className = "ib " + (running ? "danger" : "primary");
    run.onclick = () => ctx.act(running ? "정지" : "시작", () => ctx.api("POST", `/workers/${id}/${running ? "stop" : "start"}`));
    auto.setOn(!!w.autostart);
    auto.title = w.autostart ? "보드를 켤 때 자동 시작: 켜짐" : "보드를 켤 때 자동 시작: 꺼짐";
    auto.onclick = () => ctx.act("자동 시작 변경", () => ctx.api("PUT", `/workers/${id}/autostart`, { autostart: !w.autostart }));
  };
  return box;
}

export function meter(label) {
  const fill = el("span", "mtr-f");
  const val = el("span", "mtr-v", "—");
  const box = el("div", "mtr", el("span", "mtr-k", label), el("span", "mtr-b", fill), val);
  box.set = (pct, text) => {
    const p = pct === null || pct === undefined || Number.isNaN(pct) ? 0 : Math.max(0, Math.min(100, pct));
    fill.style.width = p + "%";
    fill.className = "mtr-f" + (p >= 90 ? " bad" : p >= 70 ? " warn" : "");
    val.textContent = text ?? (pct == null ? "—" : Math.round(p) + "%");
  };
  return box;
}

export function download(name, text, type = "text/csv") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = el("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// ---- 워커 설정 폼: 워커 type 선언의 config_schema로 그린다 (design.md 6.10) -------------------------
// 검사 규칙은 Manager(workspace_manager/schema.py)와 같다. 저장할 때 Manager가 같은 스키마로 다시 검사한다.
// 스키마에 없는 키(내부 값)는 폼에 나오지 않고 저장할 때 그대로 간다. 빈 칸 = 키를 지움(워커 기본값)
// 묶음에 "section": "<모듈>"이 있으면 그 칸들은 config["<모듈>"] 안에 있다(워커 모듈 구조, 경로 "<모듈>.<키>")
// 설정은 워커 폴더의 프리셋 파일(presets/<이름>.json, 깃)에 있다(design.md 6.3.1). 창 위에서 프리셋을 고르고,
// 적용(임시: 저장하지 않고 이번 부팅 동안) / 저장(사용 중 프리셋에) / 새 이름으로 저장 / 이름 바꾸기 / 삭제

const IPV4_RE = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
const HOST_RE = /^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$/;
const STREAM_URL_RE = /^(rtsp|rtsps|srt|udp|rtmp):\/\/[^/\s:]+(:\d{1,5})?(\/\S*)?$/;
const optVal = (o) => (Array.isArray(o) ? o[0] : o);
const optName = (o) => (Array.isArray(o) ? o[1] : String(o));
const clone = (v) => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const groupsOf = (schema) => (schema && schema.groups) || [];

function checkNum(f, v, integer) {
  if (typeof v !== "number" || !Number.isFinite(v) || (integer && !Number.isInteger(v))) return integer ? "정수여야 합니다" : "숫자여야 합니다";
  const lo = f.min, hi = f.max;
  if ((lo != null && v < lo) || (hi != null && v > hi)) {
    if (lo != null && hi != null) return `${lo} ~ ${hi} 사이여야 합니다`;
    return lo != null ? `${lo} 이상이어야 합니다` : `${hi} 이하여야 합니다`;
  }
  return null;
}

function checkStr(f, v) {
  if (typeof v !== "string") return "글자여야 합니다";
  if (f.format === "ipv4" && !IPV4_RE.test(v)) return "IPv4 주소여야 합니다(예: 192.168.1.10)";
  if (f.format === "host_port") {
    const i = v.lastIndexOf(":"), host = v.slice(0, i), port = v.slice(i + 1);
    if (i <= 0 || !/^\d+$/.test(port) || +port < 1 || +port > 65535 || !HOST_RE.test(host)) return "호스트:포트 형식이어야 합니다(예: 127.0.0.1:9000)";
  }
  if (f.format === "stream_url" && !STREAM_URL_RE.test(v)) return "rtsp://호스트:포트/경로 같은 주소여야 합니다";
  if (f.format === "worker_id" && !/^[a-z0-9][a-z0-9_-]{0,63}$/.test(v)) return "워커 id여야 합니다(영문 소문자·숫자·-·_)";
  if (f.pattern && !new RegExp(f.pattern).test(v)) return "형식이 맞지 않습니다";
  return null;
}

function checkValue(f, v, path, errs) {
  let m = null;
  if (f.type === "string") m = checkStr(f, v);
  else if (f.type === "int" || f.type === "number") m = checkNum(f, v, f.type === "int");
  else if (f.type === "bool") m = typeof v === "boolean" ? null : "켜기·끄기여야 합니다";
  else if (f.type === "enum") m = (f.options || []).some((o) => optVal(o) === v) ? null : "선택지 중 하나여야 합니다";
  else if (f.type === "list") {
    if (!Array.isArray(v)) m = "목록이어야 합니다";
    else {
      const seen = new Set();
      v.forEach((item, i) => {
        const p = `${path}[${i}]`;
        if (!item || typeof item !== "object" || Array.isArray(item)) { errs[p] = "항목은 객체여야 합니다"; return; }
        checkObject(f.fields || [], item, p + ".", errs);
        const k = f.item_key && item[f.item_key];
        if (typeof k === "string") {
          if (seen.has(k)) errs[`${p}.${f.item_key}`] = "같은 값이 이미 있습니다: " + k;
          seen.add(k);
        }
      });
    }
  } else if (f.type === "map") {
    if (!v || typeof v !== "object" || Array.isArray(v)) m = "키·값 목록이어야 합니다";
    else for (const [k, x] of Object.entries(v)) {
      const p = `${path}.${k}`;
      const num = f.value_type === "int" || f.value_type === "number";
      const e = f.key_pattern && !new RegExp(f.key_pattern).test(k) ? "이름 형식이 맞지 않습니다: " + (k || "(빈 이름)")
        : num ? checkNum(f, x, f.value_type === "int") : typeof x === "string" ? null : "글자여야 합니다";
      if (e) errs[p] = e;
    }
  }
  if (m) errs[path] = m;
}

function checkObject(fields, obj, prefix, errs) {
  for (const f of fields) {
    const v = obj[f.key];
    if (v === undefined || v === null) { if (f.required) errs[prefix + f.key] = "비울 수 없습니다"; continue; }
    checkValue(f, v, prefix + f.key, errs);
  }
  return errs;
}

// 입력 글자 → 값. 숫자 칸에 숫자가 아닌 글자는 그대로 두어 오류로 보여 준다
function parseInput(raw, numType) {
  const s = raw.trim();
  if (s === "") return undefined;
  if (numType === "int" || numType === "number") { const n = Number(s); return Number.isFinite(n) ? n : s; }
  return s;
}

export function workerSettings(ctx, id, { title = "설정", before = [], after = [] } = {}) {
  if (PRODUCT) return { show() {}, update() {}, dispose() {} };  // 제품 모드: 설정 잠김(창 없음, 스키마도 안 읽음)
  const st = { schema: null, liveKeys: [], saved: {}, current: {}, draft: {}, version: 0, worker: null,
               preset: null, presets: [], temp: false, files: null,
               rows: {}, top: {}, serverErrs: {}, openItems: new Set() };
  const auto = autostartRow(ctx, id);
  const form = el("div", "cfg");
  const extraErr = el("div", "cfg-err");
  const status = el("span", "hint");
  // 프리셋 줄: 고르기(바꾸면 그 값으로 다시 시작) + 새 이름으로 저장 / 이름 바꾸기 / 삭제
  const presetSel = el("select", null);
  presetSel.addEventListener("change", () => usePreset(presetSel.value));
  const saveAs = iconButton({ icon: "plus", label: "새 이름으로 저장", title: "지금 칸의 값을 새 프리셋으로 저장하고 그것을 사용", onClick: () => savePresetAs() });
  const rename = iconButton({ icon: "wrench", title: "프리셋 이름 바꾸기", onClick: () => renamePreset() });
  const del = iconButton({ icon: "trash", title: "프리셋 삭제(사용 중인 것은 지울 수 없음)", onClick: () => deletePreset() });
  const presetInfo = el("div", "hint");
  const presetBox = section({ title: "프리셋", hint: "워커 폴더 presets/의 설정 묶음. 깃에 함께 올라갑니다" },
    el("div", "cfg-mrow", presetSel, saveAs, rename, del), presetInfo);
  const revert = iconButton({ icon: "undo", label: "되돌리기", title: "저장된 값으로(칸만. 적용하면 워커도)", onClick: () => {
    st.draft = clone(st.saved); st.serverErrs = {}; render();
  } });
  const applyBtn = iconButton({ icon: "play", label: "적용", title: "저장하지 않고 바로 반영(이번 부팅 동안만, 시험용)", onClick: () => save(false) });
  const saveBtn = iconButton({ icon: "check", label: "저장", kind: "primary", title: "사용 중 프리셋에 저장하고 반영", onClick: () => save(true) });
  const foot = el("div", "cfg-foot", status, el("span", "spacer"), revert, applyBtn, saveBtn);
  const d = dialog(title, section({ title: "워커" }, auto), presetBox, ...before, form, extraErr, foot, ...after);

  const sectionObj = (cfg, g) => (g.section ? cfg[g.section] || {} : cfg);
  const errors = () => {
    const errs = {};
    for (const g of groupsOf(st.schema)) {
      const obj = sectionObj(st.draft, g);
      if (typeof obj !== "object" || Array.isArray(obj)) { errs[g.section] = "객체여야 합니다"; continue; }
      checkObject(g.fields || [], obj, g.section ? g.section + "." : "", errs);
    }
    return Object.assign(errs, st.serverErrs);
  };
  // 바뀐 칸 경로("<키>" 또는 "<모듈>.<키>"). 기준: base(기본은 저장된 값)
  const changedKeys = (base = st.saved) => groupsOf(st.schema).flatMap((g) => (g.fields || [])
    .filter((f) => !same(sectionObj(base, g)[f.key], sectionObj(st.draft, g)[f.key]))
    .map((f) => (g.section ? g.section + "." : "") + f.key));

  function edited() { st.serverErrs = {}; refresh(); }

  function refresh() {
    const errs = errors();
    const used = new Set();
    for (const [path, r] of Object.entries(st.rows)) {
      const m = errs[path] || "";
      if (m) used.add(path);
      r.err.textContent = m;
      r.row.classList.toggle("bad", !!m);
      if (r.reset) r.reset.style.visibility = r.has() ? "visible" : "hidden";
    }
    // 칸이 없는 오류(목록 항목 전체 등)는 폼 아래 한 줄로
    extraErr.textContent = Object.entries(errs).filter(([p]) => !used.has(p)).map(([p, m]) => `${p}: ${m}`).join(" · ");
    const changed = changedKeys();
    for (const [k, row] of Object.entries(st.top)) row.classList.toggle("chg", changed.includes(k));
    const pending = changedKeys(st.current);  // 워커가 지금 쓰는 값과 다른 칸
    const live = pending.length && pending.every((k) => st.liveKeys.includes(k));
    const n = Object.keys(errs).length;
    const label = (st.presets.find((x) => x.name === st.preset) || {}).label || st.preset;
    status.textContent = n ? `고칠 값 ${n}개`
      : `${changed.length ? `저장값과 다른 칸 ${changed.length}개` : `프리셋 '${label}' 그대로`}`
        + (pending.length ? ` · 반영하면 ${live ? "바로 적용" : "워커가 다시 시작(영상·제어 잠깐 끊김)"}` : "")
        + (st.temp ? " · 지금 저장 안 한 값으로 동작 중" : "");
    status.className = n ? "hint bad" : st.temp ? "hint warn" : "hint";
  }

  // 칸 한 줄. holder(): 값을 가진 객체(설정 전체 또는 목록 항목)
  function scalarRow(f, holder, path, isTop) {
    const cur = holder()[f.key];
    let input, control;
    if (f.type === "bool") {
      input = el("input");
      input.type = "checkbox";
      input.checked = cur ?? f.default ?? false;
      input.addEventListener("change", () => { holder()[f.key] = input.checked; edited(); });
      control = el("label", "switch", input);
    } else if (f.type === "enum") {
      const opts = f.options || [];
      const def = opts.find((o) => optVal(o) === f.default);
      const none = el("option", null, def !== undefined ? `기본 (${optName(def)})` : f.empty_label || "비움 (워커·장치 기본값)");
      none.value = "";
      input = el("select", null, none, ...opts.map((o, i) => { const op = el("option", null, optName(o)); op.value = String(i); return op; }));
      const idx = opts.findIndex((o) => optVal(o) === cur);
      input.value = idx >= 0 ? String(idx) : "";
      input.addEventListener("change", () => {
        if (input.value === "") delete holder()[f.key]; else holder()[f.key] = optVal(opts[+input.value]);
        edited();
      });
      control = input;
    } else {
      input = el("input");
      input.type = "text";
      if (f.type === "int" || f.type === "number") input.inputMode = "decimal";
      input.value = cur === undefined || cur === null ? "" : String(cur);
      input.placeholder = f.default !== undefined ? String(f.default) : f.placeholder || f.empty_label || "";
      if (f.format === "worker_id") {
        // 워커 간 통신 상대: 이 보드의 워커 id를 고를 수 있게(직접 써도 됨)
        const dl = el("datalist", null, ...Object.keys((ctx.state() || {}).byId || {}).filter((x) => x !== id)
          .map((x) => { const o = el("option", null); o.value = x; return o; }));
        dl.id = `wid-${id}-${path}`.replace(/[^A-Za-z0-9_-]/g, "_");
        input.setAttribute("list", dl.id);
        control = el("span", null, input, dl);
      }
      input.addEventListener("input", () => {
        const v = parseInput(input.value, f.type);
        if (v === undefined) delete holder()[f.key]; else holder()[f.key] = v;
        edited();
      });
      control = control || input;
    }
    const reset = iconButton({ icon: "undo", title: f.default !== undefined ? `기본값으로 (${f.default})` : "비우기(워커 기본값)",
      onClick: () => { delete holder()[f.key]; render(); } });
    const err = el("div", "cfg-err");
    const row = el("div", "cfg-row",
      el("label", "cfg-k", f.label || f.key, f.required ? " *" : ""),
      el("div", "cfg-v", control, f.unit ? el("span", "frm-u", f.unit) : null, reset),
      f.hint ? el("div", "cfg-h", f.hint) : null, err);
    st.rows[path] = { row, err, reset, has: () => holder()[f.key] !== undefined };
    if (isTop) st.top[path] = row;
    return row;
  }

  function mapEditor(f, holder, path, isTop) {
    const map = () => (holder()[f.key] && typeof holder()[f.key] === "object" ? holder()[f.key] : null);
    const rows = Object.entries(map() || {}).map(([name, v]) => {
      const key = el("input");
      key.value = name;
      key.placeholder = f.key_label || "이름";
      const val = el("input");
      val.value = String(v);
      val.inputMode = "decimal";
      key.addEventListener("change", () => {  // 이름 바꾸기: 순서를 지키며 새 객체로
        const out = {};
        for (const [k, x] of Object.entries(map())) out[k === name ? key.value.trim() : k] = x;
        holder()[f.key] = out;
        render();
      });
      val.addEventListener("input", () => { map()[name] = parseInput(val.value, f.value_type) ?? ""; edited(); });
      const del = iconButton({ icon: "trash", title: "지우기", onClick: () => {
        delete map()[name];
        if (!Object.keys(map()).length) delete holder()[f.key];
        render();
      } });
      const err = el("div", "cfg-err");
      const r = el("div", "cfg-mrow", key, val, f.unit ? el("span", "frm-u", f.unit) : null, del, err);
      st.rows[`${path}.${name}`] = { row: r, err, has: () => true };
      return r;
    });
    const add = iconButton({ icon: "plus", label: "추가", onClick: () => {
      if (!map()) holder()[f.key] = {};
      if (!("" in map())) map()[""] = f.min ?? 0;
      render();
    } });
    const reset = iconButton({ icon: "undo", title: "비우기(워커 기본값)", onClick: () => { delete holder()[f.key]; render(); } });
    const err = el("div", "cfg-err");
    const row = el("div", "cfg-row",
      el("label", "cfg-k", f.label || f.key),
      el("div", "cfg-map", ...rows, el("div", "cfg-mrow", add, reset)),
      f.hint ? el("div", "cfg-h", f.hint) : null, err);
    st.rows[path] = { row, err, reset, has: () => holder()[f.key] !== undefined };
    if (isTop) st.top[path] = row;
    return row;
  }

  function listEditor(f, holder, path, isTop) {
    const arr = Array.isArray(holder()[f.key]) ? holder()[f.key] : [];
    const label = f.item_label || "항목";
    const second = (f.fields || []).find((x) => x.required && x.key !== f.item_key);
    const cards = arr.map((item, i) => {
      const p = `${path}[${i}]`;
      const del = iconButton({ icon: "trash", title: `${label} 지우기`, onClick: (e) => {
        e.preventDefault();
        arr.splice(i, 1);
        if (!arr.length) delete holder()[f.key];
        st.openItems.delete(p);
        render();
      } });
      const card = el("details", "cfg-item",
        el("summary", "cfg-sum", el("span", "cfg-sum-t", item[f.item_key] || `(${label} 이름 없음)`),
          el("span", "cfg-sum-d", second && item[second.key] ? String(item[second.key]) : ""), del),
        el("div", "cfg-in", ...fieldRows(f.fields || [], () => item, p + ".", false)));
      card.open = st.openItems.has(p);
      card.addEventListener("toggle", () => { if (card.open) st.openItems.add(p); else st.openItems.delete(p); });
      return card;
    });
    const add = iconButton({ icon: "plus", label: `${label} 추가`, onClick: () => {
      if (!Array.isArray(holder()[f.key])) holder()[f.key] = [];
      const list = holder()[f.key];
      const item = {};
      if (f.item_key) {
        let n = list.length + 1;
        while (list.some((x) => x[f.item_key] === `new${n}`)) n++;
        item[f.item_key] = `new${n}`;
      }
      list.push(item);
      st.openItems.add(`${path}[${list.length - 1}]`);
      render();
    } });
    const err = el("div", "cfg-err");
    const row = el("div", "cfg-list", ...cards, el("div", "cfg-mrow", add), f.hint ? el("div", "hint", f.hint) : null, err);
    st.rows[path] = { row, err, has: () => holder()[f.key] !== undefined };
    if (isTop) st.top[path] = row;
    return row;
  }

  function fieldRow(f, holder, path, isTop) {
    if (f.type === "list") return listEditor(f, holder, path, isTop);
    if (f.type === "map") return mapEditor(f, holder, path, isTop);
    return scalarRow(f, holder, path, isTop);
  }

  function folding(key, summary, rows) {
    const det = el("details", "cfg-adv", el("summary", null, summary), el("div", "cfg-in", ...rows));
    det.open = st.openItems.has(key);
    det.addEventListener("toggle", () => { if (det.open) st.openItems.add(key); else st.openItems.delete(key); });
    return det;
  }

  // 칸 목록 → 줄들. advanced 칸은 접힌 "고급" 안으로
  function fieldRows(fields, holder, prefix, isTop) {
    const basic = fields.filter((f) => !f.advanced), adv = fields.filter((f) => f.advanced);
    const out = basic.map((f) => fieldRow(f, holder, prefix + f.key, isTop));
    if (adv.length) out.push(folding("adv:" + prefix, `고급 (${adv.length})`, adv.map((f) => fieldRow(f, holder, prefix + f.key, isTop))));
    return out;
  }

  function render() {
    st.rows = {};
    st.top = {};
    if (!st.schema) {
      form.replaceChildren(section({ title: "설정", hint: "이 워커는 설정 스키마(config_schema)가 없어 화면에서 바꿀 설정이 없습니다" }));
      foot.style.display = "none";
      return;
    }
    foot.style.display = "";
    form.replaceChildren(...(st.schema.groups || []).map((g) => {
      const fields = g.fields || [];
      const holder = g.section ? () => st.draft[g.section] : () => st.draft;
      const prefix = g.section ? g.section + "." : "";
      if (g.advanced) {
        return section({ title: g.title, hint: g.hint },
          folding("grp:" + g.title, `펼치기 (${fields.length})`, fields.map((f) => fieldRow(f, holder, prefix + f.key, true))));
      }
      return section({ title: g.title, hint: g.hint }, ...fieldRows(fields, holder, prefix, true));
    }));
    refresh();
  }

  function renderPresets() {
    presetSel.replaceChildren(...st.presets.map((x) => { const o = el("option", null, x.label + (x.name !== x.label ? ` (${x.name})` : "")); o.value = x.name; return o; }));
    if (!st.presets.some((x) => x.name === st.preset)) {  // 파일이 아직 없는 프리셋(type 기본값으로 도는 중)
      const o = el("option", null, `${st.preset} (파일 없음: 저장하면 생김)`);
      o.value = st.preset;
      presetSel.prepend(o);
    }
    presetSel.value = st.preset;
    del.disabled = st.presets.length <= 1;
    presetInfo.textContent = st.files ? `파일: ${st.files}/presets/${st.preset}.json` : "";
  }

  async function load() {
    const [s, c] = await Promise.all([ctx.api("GET", `/workers/${id}/schema`), ctx.api("GET", `/workers/${id}/config`)]);
    st.schema = s.schema;
    st.liveKeys = s.live_keys || [];
    const fill = (cfg) => {
      // 모듈 칸이 아직 없으면 빈 객체로 둔다(입력하면 그 안에 들어간다. 바뀐 것으로 보지 않음)
      for (const g of groupsOf(st.schema)) if (g.section && (typeof cfg[g.section] !== "object" || cfg[g.section] === null)) cfg[g.section] = {};
      return cfg;
    };
    st.saved = fill(c.saved || c.config || {});
    st.current = fill(c.config || {});
    st.draft = clone(st.current);
    st.version = c.config_version;
    st.preset = c.preset || "default";
    st.presets = c.presets || [];
    st.temp = !!c.temp;
    st.files = c.files;
    st.serverErrs = c.config_errors || {};  // 파일을 직접 고쳐 스키마에 안 맞는 값
    renderPresets();
    render();
  }

  const running = () => st.worker && st.worker.actual_state === "running";

  async function usePreset(name) {
    if (name === st.preset) return;
    const label = (st.presets.find((x) => x.name === name) || {}).label || name;
    if (!confirm(`프리셋 '${label}'(으)로 바꿉니다.${running() ? " 워커가 그 값으로 다시 시작됩니다." : ""}${changedKeys().length ? " 저장하지 않은 칸 값은 버려집니다." : ""}`)) {
      presetSel.value = st.preset;
      return;
    }
    try {
      await ctx.api("POST", `/workers/${id}/presets/${name}/use`);
      ctx.toast(`프리셋 '${label}' 사용`, "ok");
      await load();
    } catch (e) { presetSel.value = st.preset; ctx.toast("프리셋 바꾸기 실패: " + e.message, "bad"); }
  }

  async function savePresetAs() {
    if (Object.keys(errors()).length) { ctx.toast("고칠 값이 있어 저장할 수 없습니다", "bad"); return; }
    const label = prompt("새 프리셋의 화면 이름(예: 야외, 저조도)");
    if (!label) return;
    let name = prompt("파일 이름(영문 소문자·숫자·-·_, 예: outdoor)", label.toLowerCase().replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "") || "preset");
    if (!name) return;
    name = name.trim();
    try {
      await ctx.api("POST", `/workers/${id}/presets`, { name, label, config: st.draft });
      ctx.toast(`프리셋 '${label}' 저장·사용`, "ok");
      await load();
    } catch (e) { ctx.toast("새 프리셋 저장 실패: " + e.message, "bad"); }
  }

  async function renamePreset() {
    const cur = st.presets.find((x) => x.name === st.preset);
    if (!cur) { ctx.toast("아직 파일이 없는 프리셋입니다. 먼저 저장하세요"); return; }
    const label = prompt("화면 이름", cur.label);
    if (!label) return;
    const name = prompt("파일 이름(영문 소문자·숫자·-·_)", cur.name);
    if (!name) return;
    try {
      await ctx.api("PUT", `/workers/${id}/presets/${cur.name}`, { name: name.trim(), label });
      ctx.toast("프리셋 이름 바꿈", "ok");
      await load();
    } catch (e) { ctx.toast("이름 바꾸기 실패: " + e.message, "bad"); }
  }

  async function deletePreset() {
    const others = st.presets.filter((x) => x.name !== st.preset);
    if (!others.length) { ctx.toast("지울 수 있는 프리셋이 없습니다(사용 중인 것은 못 지움)"); return; }
    const name = prompt(`지울 프리셋의 파일 이름(사용 중 '${st.preset}' 제외): ${others.map((x) => `${x.name}(${x.label})`).join(", ")}`);
    if (!name) return;
    if (!confirm(`프리셋 '${name}'을(를) 지웁니다. 깃에 올리면 다른 곳에서도 사라집니다.`)) return;
    try {
      await ctx.api("DELETE", `/workers/${id}/presets/${name.trim()}`);
      ctx.toast("프리셋 지움", "ok");
      await load();
    } catch (e) { ctx.toast("프리셋 지우기 실패: " + e.message, "bad"); }
  }

  async function save(persist) {
    const errs = errors();
    const n = Object.keys(errs).length;
    if (n) {
      ctx.toast(`고칠 값이 ${n}개 있습니다`, "bad");
      const first = Object.keys(errs).find((p) => st.rows[p]);
      if (first) st.rows[first].row.scrollIntoView({ block: "center" });
      return;
    }
    const pending = changedKeys(st.current);
    if (!persist && !pending.length && same(st.current, st.draft)) { ctx.toast("지금 값과 같습니다"); return; }
    if (persist && !st.temp && !changedKeys().length && same(st.saved, st.draft)
        && st.presets.some((x) => x.name === st.preset)) { ctx.toast("저장된 값과 같습니다"); return; }
    const live = pending.every((k) => st.liveKeys.includes(k));
    if (running() && pending.length && !live
        && !confirm(`설정 ${pending.length}개를 ${persist ? "저장하고 " : ""}반영합니다. 워커가 다시 시작됩니다(영상·제어가 잠깐 끊김).`)) return;
    try {
      if (persist) await ctx.api("PUT", `/workers/${id}/config`, st.draft);
      else await ctx.api("POST", `/workers/${id}/config/apply`, st.draft);
      ctx.toast(persist ? "프리셋에 저장·반영" : "반영(저장 안 함, 재부팅하면 저장값으로)", "ok");
      await load();
    } catch (e) {
      st.serverErrs = (e.detail && e.detail.fields) || {};
      refresh();
      ctx.toast((persist ? "저장" : "적용") + " 실패: " + e.message, "bad");
    }
  }

  const showDialog = d.show;
  d.show = () => {
    showDialog();
    if (!st.schema) { form.replaceChildren(el("div", "hint", "설정 불러오는 중…")); foot.style.display = "none"; }
    load().catch((e) => {
      form.replaceChildren(section({ title: "설정", hint: "설정을 읽지 못했습니다: " + e.message }));
      foot.style.display = "none";
    });
  };
  d.update = (w) => { if (!w) return; st.worker = w; auto.update(w); };
  return d;
}
