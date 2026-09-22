/**
 * ui.js - the smallest control builder that keeps the scenes declarative.
 *
 * A scene returns a list of descriptors and never touches the DOM. That is not tidiness for its
 * own sake: the headless check drives scenes through the same state a control writes, so a scene
 * whose logic lived in an event handler could only be tested by synthesizing clicks.
 */

/** @param {HTMLElement} root */
export function renderControls(root, items) {
  root.textContent = "";
  for (const it of items) root.appendChild(build(it));
}

function row(label, node, hint) {
  const d = document.createElement("div");
  d.className = "ctl";
  if (label) {
    const l = document.createElement("label");
    l.textContent = label;
    d.appendChild(l);
  }
  d.appendChild(node);
  if (hint) {
    const h = document.createElement("p");
    h.className = "hint";
    h.textContent = hint;
    d.appendChild(h);
  }
  return d;
}

function build(it) {
  switch (it.kind) {
    case "toggle": {
      const w = document.createElement("label");
      w.className = "toggle";
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.checked = !!it.get();
      cb.addEventListener("change", () => it.set(cb.checked));
      const s = document.createElement("span");
      s.textContent = it.label;
      w.append(cb, s);
      return row(null, w, it.hint);
    }
    case "slider": {
      const w = document.createElement("div");
      w.className = "slider";
      const r = document.createElement("input");
      r.type = "range";
      r.min = String(it.min);
      r.max = String(it.max);
      r.step = String(it.step ?? (it.max - it.min) / 100);
      r.value = String(it.get());
      const v = document.createElement("output");
      const show = () => { v.textContent = it.format ? it.format(Number(r.value)) : r.value; };
      show();
      r.addEventListener("input", () => { it.set(Number(r.value)); show(); });
      w.append(r, v);
      return row(it.label, w, it.hint);
    }
    case "select": {
      const s = document.createElement("select");
      it.options.forEach((o, i) => {
        const opt = document.createElement("option");
        opt.value = String(i);
        opt.textContent = o;
        s.appendChild(opt);
      });
      s.value = String(it.get());
      s.addEventListener("change", () => it.set(Number(s.value)));
      return row(it.label, s, it.hint);
    }
    case "buttons": {
      const w = document.createElement("div");
      w.className = "buttons";
      for (const b of it.items) {
        const el = document.createElement("button");
        el.textContent = b.label;
        el.addEventListener("click", () => b.onClick());
        w.appendChild(el);
      }
      return row(it.label, w, it.hint);
    }
    case "file": {
      /* The value is the FILE's text, read here, so a scene or the page never touches a FileReader.
       * The input is reset after every pick, or choosing the same file twice fires nothing. */
      const i = document.createElement("input");
      i.type = "file";
      if (it.accept) i.accept = it.accept;
      i.addEventListener("change", async () => {
        const f = i.files && i.files[0];
        i.value = "";
        if (f) await it.set(await f.text(), f.name);
      });
      return row(it.label, i, it.hint);
    }
    case "note": {
      const p = document.createElement("p");
      p.className = "note";
      p.textContent = it.text;
      return p;
    }
    default:
      throw new Error(`ui: unknown control kind "${it.kind}"`);
  }
}

/** Fill a two-column readout table from [[label, value], ...]. */
export function renderReadout(table, rows) {
  let html = "";
  for (const [k, v] of rows) html += `<tr><th>${esc(k)}</th><td>${esc(String(v))}</td></tr>`;
  table.innerHTML = html;
}

function esc(s) {
  return s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}
