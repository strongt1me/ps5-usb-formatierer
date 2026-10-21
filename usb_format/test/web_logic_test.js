// Logik der Weboberfläche ohne Browser (node): Update-Hinweis (verTuple, newerVersion, checkUpdate mit gestellter
// GitHub-Antwort) und Auftragsverlauf (historyList). Die Funktionen werden aus web/index.html gelesen - getestet wird
// also genau der ausgelieferte Code, nur fetch/DOM sind gestellt.
//   node test/web_logic_test.js web/index.html
"use strict";
const fs = require("fs");
const vm = require("vm");

const page = fs.readFileSync(process.argv[2], "utf8");
let fails = 0;
const ok = (name, cond, extra) => {
  console.log((cond ? "OK      " : "FEHLER  ") + name + (cond || extra === undefined ? "" : "  [" + JSON.stringify(extra) + "]"));
  if (!cond) fails++;
};

// Quelltext einer Funktion bzw. Konstante (bis zur passenden Klammer) aus der Seite
function grab(head) {
  const at = page.indexOf(head);
  if (at < 0) throw new Error("nicht gefunden: " + head);
  let i = page.indexOf(head.startsWith("const") ? (page[page.indexOf("=", at) + 2] === "{" ? "{" : ";") : "{", at);
  if (page[i] === ";") return page.slice(at, i + 1);
  let depth = 0, str = null;
  for (; i < page.length; i++) {
    const ch = page[i];
    if (str) { if (ch === "\\") i++; else if (ch === str) str = null; continue; }
    if (ch === '"' || ch === "'" || ch === "`") str = ch;
    else if (ch === "{") depth++;
    else if (ch === "}" && --depth === 0) return page.slice(at, i + 1) + (head.startsWith("const") ? ";" : "");
  }
  throw new Error("kein Ende: " + head);
}

// --------------------------------------------------------------- gestellte Umgebung
function makeEl(tag) {
  const e = { tag, className: "", hidden: false, title: "", href: "", tabIndex: -1, children: [], _text: "",
              get textContent() { return this._text + this.children.map(c => c.textContent).join(""); },
              set textContent(v) { this._text = String(v); this.children = []; },
              get firstChild() { return this.children[0]; },
              appendChild(c) { this.children.push(c); return c; },
              append(...cs) { cs.forEach(c => this.children.push(c)); } };
  return e;
}
const els = {};
const newver = makeEl("a");
newver.hidden = true;
newver.appendChild(makeEl("span"));
els.newver = newver;

let fetchCalls = [], fetchImpl = null;
const ctx = {
  console, setTimeout: (f) => 0, AbortController: class { constructor() { this.signal = {}; } abort() {} },
  LANG: "de", LOCALE: { de: "de-DE", en: "en-US" }, isTv: false,
  $: id => els[id],
  tr: (de, v) => v ? de.replace(/\{(\w+)\}/g, (m, k) => k in v ? String(v[k]) : m) : de,
  N_: de => de,
  q: s => "„" + s + "“",
  fmtSize: b => (b / 1e9).toFixed(1) + " GB",
  el: (tag, cls, text) => { const e = makeEl(tag); if (cls) e.className = cls; if (text !== undefined) e._text = String(text); return e; },
  fetch: (url, opt) => { fetchCalls.push({ url, opt }); return fetchImpl(url, opt); },
};
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext("var jobHistory = [];", ctx);
for (const src of [grab("const REPO ="), grab("function verTuple("), grab("function newerVersion("),
                   grab("function checkUpdate("), grab("const HIST_KIND ="), grab("function historyList("),
                   grab("const DRIVE_KINDS ="), grab("function lastJobOf("), grab("function lastJobText(")])
  vm.runInContext(src, ctx);
const run = code => vm.runInContext(code, ctx);
const flush = () => new Promise(r => setImmediate(r));

(async () => {
  // --------------------------------------------------------------- Versionen
  ok("verTuple: v2.6.2", JSON.stringify(run('verTuple("v2.6.2")')) === "[2,6,2]");
  ok("verTuple: ungültig", run('verTuple("2.6") === null && verTuple("") === null && verTuple(null) === null && verTuple("v2.6.2-rc1") === null'));
  const nv = (d, c) => run("newerVersion(" + JSON.stringify(d) + "," + JSON.stringify(c) + ")");
  ok("neuer erkannt", nv({ tag_name: "v2.6.3" }, "2.6.2") === "2.6.3");
  ok("zweistellig (2.10.0 > 2.9.9)", nv({ tag_name: "v2.10.0" }, "2.9.9") === "2.10.0");
  ok("gleich: nichts", nv({ tag_name: "v2.6.2" }, "2.6.2") === "");
  ok("älter: nichts", nv({ tag_name: "v2.5.9" }, "2.6.2") === "");
  ok("Vorabversion/Entwurf: nichts", nv({ tag_name: "v2.7.0", prerelease: true }, "2.6.2") === ""
     && nv({ tag_name: "v2.7.0", draft: true }, "2.6.2") === "");
  ok("Unsinn: nichts", nv(null, "2.6.2") === "" && nv([], "2.6.2") === "" && nv({ tag_name: "neu" }, "2.6.2") === ""
     && nv({ tag_name: "v2.7.0" }, "kaputt") === "");

  // --------------------------------------------------------------- checkUpdate
  const reset = () => { newver.hidden = true; newver.href = ""; newver.title = ""; newver.firstChild._text = ""; fetchCalls = []; };
  const answer = (status, body) => () => Promise.resolve({ ok: status === 200, json: () => Promise.resolve(body) });
  reset();
  fetchImpl = answer(200, { tag_name: "v2.7.0", html_url: "https://boese.example/x" });
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  ok("neuere Version: Hinweis sichtbar", !newver.hidden && newver.firstChild.textContent.includes("2.7.0"), newver.firstChild.textContent);
  ok("Link führt immer ins Projekt (nicht html_url)",
     newver.href === "https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.7.0", newver.href);
  ok("Abfrage: GitHub-API, ohne Cookies, ohne Referrer",
     fetchCalls.length === 1 && fetchCalls[0].url === "https://api.github.com/repos/strongt1me/ps5-usb-formatierer/releases/latest"
     && fetchCalls[0].opt.credentials === "omit" && fetchCalls[0].opt.referrerPolicy === "no-referrer", fetchCalls);
  reset();
  ctx.isTv = true;
  fetchImpl = answer(200, { tag_name: "v2.7.0" });
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  ok("PS5-Browser: Hinweis ohne Link", !newver.hidden && newver.href === "", newver.href);
  ctx.isTv = false;
  reset();
  fetchImpl = answer(200, { tag_name: "v2.6.2" });
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  ok("gleiche Version: kein Hinweis", newver.hidden);
  reset();
  fetchImpl = answer(403, { message: "rate limit" });
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  ok("Ratenbegrenzung: still", newver.hidden);
  reset();
  fetchImpl = () => Promise.reject(new Error("offline"));
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  ok("ohne Internet: still, kein Fehler", newver.hidden);
  reset();
  run('checkUpdate("unbekannt")');
  ok("Version des Formatierers unbekannt: keine Abfrage", fetchCalls.length === 0);

  // --------------------------------------------------------------- Verlauf
  const box = run(`historyList([
    {t: 1792593328, kind: "format", ok: true, device: "da2", model: "VendorCo", size: 62914560000, fs: "exFAT", label: "PS5", secs: 12, msg: "formatiert"},
    {t: 1792593000, kind: "speed", ok: false, cancelled: true, msg: "Abgebrochen."},
    {t: 1792592000, kind: "verify", ok: false, file: "a.img.zst", msg: "kaputt"},
    {t: 1792591000, kind: "neu-unbekannt", ok: true}])`);
  const rows = box.children;
  ok("Verlauf: vier Zeilen mit Status", rows.length === 4 && rows.map(r => r.className).join("|")
     === "hrow ok|hrow cancel|hrow fail|hrow ok", rows.map(r => r.className));
  ok("Verlauf: Auftrag, Laufwerk, Dateisystem, Bezeichnung, Dauer",
     rows[0].textContent.includes("Formatieren – erfolgreich") && rows[0].textContent.includes("VendorCo (da2)")
     && rows[0].textContent.includes("exFAT") && rows[0].textContent.includes("„PS5“") && rows[0].textContent.includes("12 s"),
     rows[0].textContent);
  ok("Verlauf: abgebrochen / fehlgeschlagen / Datei", rows[1].textContent.includes("abgebrochen")
     && rows[2].textContent.includes("fehlgeschlagen") && rows[2].textContent.includes("a.img.zst"));
  ok("Verlauf: unbekannte Auftragsart (neuerer Formatierer) bleibt lesbar", rows[3].textContent.includes("neu-unbekannt"));
  ok("Verlauf: Zeilen per Controller erreichbar", rows.every(r => r.tabIndex === 0));
  ok("Verlauf leer: Hinweis", run("historyList([])").children[0].className === "empty");

  // --------------------------------------------------------------- "Zuletzt: …" am Laufwerk
  run(`jobHistory = [
    {t: 1792593328, kind: "tile", ok: true},
    {t: 1792593000, kind: "speed", ok: false, cancelled: true, model: "General UDisk", size: 1000},
    {t: 1792592000, kind: "format", ok: true, model: "VendorCo", size: 64, ident: "AAA", fs: "exFAT"},
    {t: 1792591000, kind: "label", ok: true, model: "VendorCo", size: 64, ident: "BBB"}]`);
  const last = d => run("lastJobOf(" + JSON.stringify(d) + ")");
  ok("Zuletzt: über die Seriennummer, nicht den Namen",
     last({ name: "da5", model: "VendorCo", size: 64, ident: "BBB" }).kind === "label"
     && last({ name: "da2", model: "VendorCo", size: 64, ident: "AAA" }).kind === "format");
  ok("Zuletzt: anderer Stick mit Seriennummer: nichts", last({ model: "VendorCo", size: 64, ident: "CCC" }) === null);
  ok("Zuletzt: ohne Seriennummer über Modell und Größe", last({ model: "General UDisk", size: 1000, ident: "" }).kind === "speed");
  ok("Zuletzt: Kachel-Auftrag gehört zu keinem Laufwerk", last({ model: "", size: 0, ident: "" }) === null);
  const lt = run("lastJobText(jobHistory[2]) + '|' + lastJobText(jobHistory[1])");
  ok("Zuletzt: Text mit Dateisystem / abgebrochen", /^Zuletzt: Formatieren \(exFAT\), .+\|Zuletzt: Tempo messen – abgebrochen, /.test(lt), lt);

  console.log(fails ? `${fails} Test(s) fehlgeschlagen.` : "Alle Web-Logik-Tests bestanden.");
  process.exit(fails ? 1 : 0);
})();
