// 워크스페이스 › 설정(ui/workspace-screen.md 2장): 워크스페이스 기능 설정을 기능별 폼으로 보고 바꾼다.
// 칸 정의는 install/config/workspace-settings.json(게이트웨이 GET /workspace/settings가 지금 값과 함께 줌).
// 저장 = PUT /workspace/settings(바뀐 칸만). apply "live"는 바로, "restart"는 워크스페이스를 다시 시작하면 적용
let els = {};

function inputFor(ctx, f) {
  const { h } = ctx;
  const cur = f.value ?? "";
  let inp;
  if (f.type === "enum") {
    inp = h("select", {}, h("option", { value: "" }, `기본 (${f.default ?? "—"})`),
      ...(f.enum || []).map((v) => h("option", { value: String(v) }, String(v))));
    inp.value = String(cur);
  } else {
    const num = f.type === "number" || f.type === "integer";
    inp = h("input", { type: num ? "number" : "text", placeholder: f.default === "" || f.default == null ? "" : `기본 ${f.default}`,
                       min: f.min, max: f.max, step: f.type === "integer" ? 1 : num ? "any" : undefined,
                       style: num ? "width:120px" : "width:min(420px,100%)" });
    inp.value = String(cur);
  }
  inp.disabled = !ctx.ui.isDevMode();
  inp.addEventListener("input", () => mark(ctx));
  inp.addEventListener("change", () => mark(ctx));
  return inp;
}

// 입력 → 보낼 값(빈 칸 = null = 기본으로)
function read(f, inp) {
  const t = inp.value.trim();
  if (t === "") return null;
  if (f.type === "integer") return Number.isInteger(Number(t)) ? Number(t) : t;
  if (f.type === "number") return Number.isFinite(Number(t)) ? Number(t) : t;
  return t;
}

function changes() {
  const out = {};
  for (const r of els.rows || []) {
    const v = read(r.f, r.inp);
    if (v !== (r.f.value ?? null)) out[r.f.key] = v;
  }
  return out;
}

function mark(ctx) {
  const n = Object.keys(changes()).length;
  els.save.disabled = !n || !ctx.ui.isDevMode();
  els.undo.disabled = !n;
  els.status.textContent = n ? `바뀐 칸 ${n}개` : "";
  for (const r of els.rows || []) r.row.classList.toggle("changed", read(r.f, r.inp) !== (r.f.value ?? null));
}

function render(ctx, d) {
  const { h, ui } = ctx;
  els.rows = [];
  const pending = d.pending || [];
  els.pending.replaceChildren(...(pending.length ? [h("div", { class: "notice warn" },
    ui.icon("alert"), ` 다시 시작해야 적용되는 값: ${pending.join(", ")} — 보드에서 workspace-start.sh로 다시 시작`)] : []));
  els.form.replaceChildren(...(d.groups || []).map((g) => ui.section({ title: g.title },
    ...g.fields.map((f) => {
      const inp = inputFor(ctx, f);
      // 모든 칸이 바로 적용(docs/live-settings.md)이라 알릴 것이 있을 때만 배지
      const tag = f.pending ? ui.pill("refresh", "다시 시작 기다림", "warn")
        : f.apply !== "live" ? ui.pill("refresh", "다시 시작하면 적용", "info") : null;
      const err = h("div", { class: "field-err" });
      const row = h("div", { class: "set-row" },
        h("div", { class: "set-k" }, h("div", {}, f.label), h("div", { class: "mono faint" }, f.key)),
        h("div", { class: "set-v" }, h("div", { class: "btnrow", style: "align-items:center" }, inp, f.unit ? h("span", { class: "muted" }, f.unit) : null, tag),
          f.description ? h("div", { class: "hint" }, f.description) : null, err));
      els.rows.push({ f, inp, row, err });
      return row;
    }))));
  els.file.textContent = d.file ? `파일 ${d.file}` : "";
  mark(ctx);
}

async function load(ctx) {
  try {
    const d = await ctx.api("GET", "/workspace/settings");
    if (!els.form) return;
    els.err.textContent = "";
    render(ctx, d);
  } catch (e) {
    if (els.err) els.err.textContent = `설정을 읽지 못했습니다: ${e.message || e}`;
  }
}

async function save(ctx) {
  const body = changes();
  if (!Object.keys(body).length) return;
  for (const r of els.rows) r.err.textContent = "";
  try {
    const res = await ctx.api("PUT", "/workspace/settings", body);
    const live = (res.live || []).length, restart = (res.restart || []).length;
    ctx.toast(`설정 저장: ${[live ? `바로 적용 ${live}` : "", restart ? `다시 시작하면 적용 ${restart}` : ""].filter(Boolean).join(" · ")}`, "ok");
    await load(ctx);
  } catch (e) {
    const fields = (e.detail && e.detail.fields) || {};
    for (const r of els.rows) if (fields[r.f.key]) r.err.textContent = fields[r.f.key];
    ctx.toast("설정 저장 실패: " + e.message, "bad");
  }
}

export default {
  mount(el, ctx) {
    const { h, ui } = ctx;
    els.err = h("div", { class: "muted" });
    els.pending = h("div");
    els.form = h("div");
    els.status = h("span", { class: "hint" });
    els.file = h("div", { class: "hint", style: "margin-top:10px" });
    els.save = ui.iconButton({ icon: "check", label: "저장", kind: "primary", onClick: () => save(ctx) });
    els.undo = ui.iconButton({ icon: "refresh", label: "되돌리기", onClick: () => load(ctx) });
    el.append(
      ui.panelHead({ title: "설정", badges: [els.status], actions: [els.undo, els.save] }),
      els.err, els.pending, els.form, els.file);
    load(ctx);
  },
  unmount() { els = {}; },
};
