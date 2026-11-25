// 스트림 묶기(docs/stream-mux.md): 화면 하나 = 스트림 연결 하나. GET /api/board/streams?ids=a:2,b:10 (SSE)
//
//   subscribe(id, hz, onLine) → 끊기 함수(여러 번 불러도 됨)
//   onLine(msg): 워커 줄 그대로 + "worker"(design.md 9.7: 첫 줄 기준 상태 kf, 이후 바뀐 주제만).
//                꺼진·없는 워커는 {worker, down: true, stream: "down"} (옛 /workers/{id}/stream의 down과 같게 stream도 붙임)
//   - 구독이 바뀌면 100 ms 모아 연결을 다시 연다(열린 연결에 더하는 길 없음). 다시 열면 워커마다 첫 줄이 기준 상태라 놓치는 것 없음
//   - 같은 워커를 여럿이 받으면 hz는 가장 큰 값. 새로 받는 쪽이 생기면 기준 상태를 받게 다시 연다
//   - 끊기면 EventSource가 다시 붙는다. 응답이 오류(401·502 …)면 EventSource가 멈추므로 2 s 뒤 직접 다시 연다
//   - 워커가 32개(게이트웨이 상한)를 넘으면 32개씩 연결을 나눈다
const DEBOUNCE_MS = 100;
const RETRY_MS = 2000;
const MAX_IDS = 32;

const subs = new Map();  // 워커 id → Set<{hz, onLine}>
let conns = [];          // [{es, ids}]
let openKey = "";        // 지금 연 주소들
let fresh = false;       // 새로 받는 쪽이 생김(주소가 같아도 다시 엶)
let timer = null, retry = null;

function deliver(id, msg) {
  const set = subs.get(id);
  if (!set) return;
  for (const s of [...set]) {
    try { s.onLine(msg); } catch (e) { console.error(e); }
  }
}

function groups() {
  const parts = [...subs.keys()].sort().map((id) => {
    let hz = 0;
    for (const s of subs.get(id)) hz = Math.max(hz, s.hz);
    return { id, part: `${encodeURIComponent(id)}:${hz}` };
  });
  const out = [];
  for (let i = 0; i < parts.length; i += MAX_IDS) {
    const g = parts.slice(i, i + MAX_IDS);
    out.push({ ids: g.map((p) => p.id), url: `/api/board/streams?ids=${g.map((p) => p.part).join(",")}` });
  }
  return out;
}

function closeAll() {
  for (const c of conns) c.es.close();
  conns = [];
  openKey = "";
}

function connect(g) {
  const c = { es: new EventSource(g.url), ids: g.ids };
  c.es.onmessage = (m) => {
    if (!conns.includes(c)) return;
    let msg;
    try { msg = JSON.parse(m.data); } catch { return; }
    if (!msg || typeof msg.worker !== "string") return;
    if (msg.down) msg.stream = "down";
    deliver(msg.worker, msg);
  };
  c.es.onerror = () => {
    if (!conns.includes(c)) return;
    for (const id of c.ids) deliver(id, { worker: id, down: true, stream: "down" });
    if (c.es.readyState === 2 && !retry) {  // CLOSED: 다시 붙지 않음
      retry = setTimeout(() => { retry = null; fresh = true; reopen(); }, RETRY_MS);
    }
  };
  return c;
}

function reopen() {
  timer = null;
  if (typeof EventSource !== "function") return;
  const gs = groups();
  const key = gs.map((g) => g.url).join(" ");
  if (!fresh && key === openKey) return;
  fresh = false;
  closeAll();
  if (retry) { clearTimeout(retry); retry = null; }
  conns = gs.map(connect);
  openKey = key;
}

function schedule() {
  if (!timer) timer = setTimeout(reopen, DEBOUNCE_MS);
}

export function subscribe(id, hz, onLine) {
  const s = { hz: Math.min(Math.max(Number(hz) || 10, 0.2), 50), onLine };
  if (!subs.has(id)) subs.set(id, new Set());
  subs.get(id).add(s);
  fresh = true;
  schedule();
  let done = false;
  return () => {
    if (done) return;
    done = true;
    const set = subs.get(id);
    if (!set) return;
    set.delete(s);
    if (!set.size) subs.delete(id);
    schedule();  // 주소가 그대로면(같은 워커를 다른 쪽이 계속 받음) 다시 열지 않는다
  };
}

// 확인용: 지금 연결 수와 워커 목록
export function stats() {
  return { connections: conns.length, workers: [...subs.keys()].sort() };
}
