// 매니저 주제 받기(ui/system-panel.md 2장). 건강 띠(shell.js)와 패널 "시스템"이 함께 쓴다(ES 모듈 하나 = 화면에 하나).
//
//   찾기: GET /workers에서 kind "manager"인 워커마다 계약 GET /workers/{id}/io를 한 번 읽어, 내보내는 역할
//         (system·fault·mission·payload·config)의 매니저를 안다. record는 역할이 없어 type "record-manager"로
//   받기: 그 매니저가 떠 있으면 스트림 /workers/{id}/stream?hz=2(SSE)를 열어 주제를 합친다. 꺼지면 닫는다
//   get(role) → null(설치 안 됨) | {worker, state: "running"|…, up, got, topic, events}
//   브라우저는 출처마다 HTTP/1.1 연결이 6개라, 늘 여는 것은 띠에 쓰는 system·fault 둘. 나머지는 want()한 동안만(패널)

export const ROLES = ["system", "fault", "mission", "payload", "config"];
const HZ = 2;
const MAX_EVENTS = 20;
const ALWAYS = new Set(["system", "fault"]);
const wanted = {};     // 역할 → want() 횟수
let lastState = null, lastApi = null;

const roles = {};      // 역할 → 워커 id
const probed = {};     // 워커 id → "pending" | "done"
const feeds = {};      // 워커 id → {es, up, topics, events}
let workers = {};      // 워커 id → /workers 한 줄

function closeFeed(id) {
  const f = feeds[id];
  if (f && f.es) f.es.close();
  delete feeds[id];
}

function openFeed(id) {
  if (feeds[id] || typeof EventSource !== "function") return;
  const f = { es: null, up: false, got: false, topics: null, events: [] };
  feeds[id] = f;
  const es = new EventSource(`/api/board/workers/${encodeURIComponent(id)}/stream?hz=${HZ}`);
  f.es = es;
  es.onmessage = (m) => {
    if (feeds[id] !== f) return;
    let msg;
    try { msg = JSON.parse(m.data); } catch { return; }
    if (msg.stream === "down") { f.up = false; f.topics = null; return; }
    if (msg.kf || !f.topics) f.topics = { ...(msg.topics || {}) };
    else Object.assign(f.topics, msg.topics || {});
    f.up = true;
    f.got = true;
    if (Array.isArray(msg.events) && msg.events.length) {
      const seen = new Set(f.events.map((e) => `${e.source}|${e.seq}`));
      for (const e of msg.events) if (!seen.has(`${e.source}|${e.seq}`)) f.events.push(e);
      if (f.events.length > MAX_EVENTS) f.events.splice(0, f.events.length - MAX_EVENTS);
    }
  };
  es.onerror = () => { if (feeds[id] === f) f.up = false; };  // EventSource가 알아서 다시 붙는다
}

async function probe(id, api) {
  probed[id] = "pending";
  try {
    const io = await api("GET", `/workers/${encodeURIComponent(id)}/io`);
    const provides = io && typeof io.provides === "object" && io.provides ? Object.keys(io.provides) : [];
    for (const r of provides) if (ROLES.includes(r)) roles[r] = id;
    probed[id] = "done";
  } catch {
    delete probed[id];  // 다음 갱신 때 다시
  }
}

// 셸 갱신마다 부름: 새 매니저는 계약을 읽고, 떠 있는 것만 스트림을 연다
export function sync(state, api) {
  lastState = state;
  lastApi = api;
  workers = state.byId || {};
  const managers = (state.workers || []).filter((w) => w.kind === "manager");
  for (const w of managers) if (!probed[w.id]) probe(w.id, api);
  for (const [r, id] of Object.entries(roles)) if (!workers[id]) delete roles[r];  // 지워진 매니저
  const want = new Set(Object.entries(roles).filter(([r, id]) => (ALWAYS.has(r) || wanted[r] > 0) &&
    workers[id] && workers[id].actual_state === "running").map(([, id]) => id));
  for (const id of Object.keys(feeds)) if (!want.has(id)) closeFeed(id);
  for (const id of want) openFeed(id);
}

// 패널이 열려 있는 동안 더 받을 역할(띠에 안 쓰는 것). 돌려준 함수로 그만 받음
export function want(list) {
  for (const r of list) wanted[r] = (wanted[r] || 0) + 1;
  if (lastState) sync(lastState, lastApi);
  return () => {
    for (const r of list) wanted[r] = Math.max(0, (wanted[r] || 0) - 1);
    if (lastState) sync(lastState, lastApi);
  };
}

export function get(role) {
  const id = roles[role];
  if (!id || !workers[id]) return null;
  const w = workers[id];
  const f = feeds[id];
  return { worker: id, state: w.actual_state, up: !!(f && f.up), got: !!(f && f.got), topic: f && f.topics ? f.topics[role] || null : null,
           events: f ? f.events : [] };
}

// record-manager: 주제가 없어 /workers 한 줄(지표)
export function record() {
  return Object.values(workers).find((w) => w.kind === "manager" && w.type === "record-manager") || null;
}

// 띠·패널 공용 글자
export const HEALTH = {
  ok: ["check", "정상", "ok"], degraded: ["alert", "주의", "warn"], fault: ["alert", "고장", "bad"],
  starting: ["refresh", "시작 중", "info"], off: ["stop", "꺼짐", "idle"],
};
export const SEVERITY = { warning: ["주의", "warn"], error: ["오류", "bad"], critical: ["심각", "bad"], none: ["없음", "ok"] };

// 매니저 카드의 상태 글자: null(정상 = 주제 있음) 또는 [글자, kind]
export function feedState(f, what) {
  if (!f) return [`${what} 설치 안 됨`, "idle"];
  if (f.state !== "running") return [`${f.worker} 꺼짐 (${f.state || "—"})`, "idle"];
  if (!f.got) return ["받는 중", "idle"];
  if (!f.up || !f.topic) return [`${f.worker} 스트림 끊김`, "warn"];
  return null;
}
