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
              attrs: {}, setAttribute(k, v) { this.attrs[k] = String(v); },
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
  dialog: o => { dialogs.push(o); return Promise.resolve(dialogAnswer); },
  open: (...a) => { opened.push(a); },
};
let dialogs = [], dialogAnswer = "yes", opened = [];
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext("var jobHistory = [], newVer = \"\";", ctx);
for (const src of [grab("const REPO ="), grab("function verTuple("), grab("function newerVersion("),
                   grab("function checkUpdate("), grab("function renderNewVer("), grab("function changesSince("),
                   grab("function mdText("), grab("function showChanges("), grab("const HIST_KIND ="), grab("function historyList("),
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
  const reset = () => { newver.hidden = true; newver.href = ""; newver.title = ""; newver.firstChild._text = ""; fetchCalls = [];
                        newver.tabIndex = -1; newver.attrs = {}; newver.onkeydown = null; };
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
  ok("PS5-Browser: Hinweis per Controller erreichbar (fokussierbar, Knopf)", newver.tabIndex === 0 && newver.attrs.role === "button",
     [newver.tabIndex, newver.attrs]);
  {
    let prevented = false;
    dialogs = []; fetchCalls = []; fetchImpl = () => Promise.resolve({ ok: false, text: () => Promise.resolve("") });
    newver.onkeydown({ key: "Enter", preventDefault: () => { prevented = true; } });
    for (let i = 0; i < 6; i++) await flush();
    ok("PS5-Browser: Enter bzw. X öffnet Was ist neu", prevented && dialogs.length === 1 && dialogs[0].title.includes("2.7.0"), dialogs);
    dialogs = []; newver.onkeydown({ key: "ArrowDown", preventDefault: () => {} });
    for (let i = 0; i < 6; i++) await flush();
    ok("PS5-Browser: andere Tasten öffnen nichts", dialogs.length === 0);
  }
  ctx.isTv = false;
  reset();
  fetchImpl = answer(200, { tag_name: "v2.6.2" });
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  ok("gleiche Version: kein Hinweis", newver.hidden);
  reset();
  fetchImpl = answer(200, { tag_name: "v2.7.0" });
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  ok("PC-Browser: Hinweis ist ein Link, kein zusätzlicher Knopf", newver.href !== "" && newver.tabIndex === -1 && !newver.attrs.role);

  // --------------------------------------------------------------- Was ist neu (Changelog seit der laufenden Version)
  const log = "# Changelog\n\nText\n\n## [Unveröffentlicht]\n\n### Hinzugefügt\n\n- noch nicht\n\n" +
    "## [2.7.1] – 2099-01-02\n\n### Behoben\n\n- **neuester** mit [Link](https://x) und `code`\n  weiter\n\n" +
    "## [2.7.0] – 2099-01-01\n\n### Hinzugefügt\n\n- dazwischen\n\n### Entfernt\n\n- weg\n\n" +
    "## [2.6.2] – 2026-01-01\n\n### Hinzugefügt\n\n- installiert\n\n[Unveröffentlicht]: https://x\n[2.7.1]: https://y\n";
  const cs = run("changesSince(" + JSON.stringify(log) + ', "2.6.2", "2.7.1")');
  ok("Changelog: Versionen nach der laufenden bis zur neuen, neueste zuerst",
     cs.length === 2 && cs[0].head === "2.7.1 – 2099-01-02" && cs[1].head === "2.7.0 – 2099-01-01", cs);
  ok("Changelog: Rubriken und Punkte, Fortsetzungszeile angehängt",
     cs[1].kinds.length === 2 && cs[1].kinds[1].kind === "Entfernt" && cs[1].kinds[1].items[0] === "weg"
     && cs[0].kinds[0].items[0].endsWith(" weiter"), cs);
  ok("Changelog: nur bis zur angebotenen Version", run("changesSince(" + JSON.stringify(log) + ', "2.6.2", "2.7.0")').length === 1);
  ok("Changelog: Unsinn ergibt nichts", run('changesSince("kaputt", "2.6.2", "2.7.1").length === 0 && ' +
     'changesSince(null, "2.6.2", "2.7.1").length === 0 && changesSince("## [2.7.0]", "neu", "2.7.1").length === 0'));
  ok("Markdown ohne Formatierung", run('mdText("**fett** [Text](https://x) `c`")') === "fett Text c");
  const textAnswer = (status, body) => () => Promise.resolve({ ok: status === 200, text: () => Promise.resolve(body) });
  dialogs = []; fetchCalls = []; opened = []; dialogAnswer = "alt";
  fetchImpl = textAnswer(200, log);
  run('showChanges("2.6.2", "2.7.1")');
  for (let i = 0; i < 6; i++) await flush();
  const d = dialogs[0] || {};
  ok("Was ist neu: deutsches Changelog im Stand des neuen Releases",
     fetchCalls[0] && fetchCalls[0].url === "https://api.github.com/repos/strongt1me/ps5-usb-formatierer/contents/CHANGELOG.de.md?ref=v2.7.1"
     && fetchCalls[0].opt.credentials === "omit" && fetchCalls[0].opt.headers.Accept === "application/vnd.github.raw", fetchCalls);
  ok("Was ist neu: Dialog mit beiden Versionen als Text",
     d.extra && d.extra.textContent.includes("2.7.1 – 2099-01-02") && d.extra.textContent.includes("neuester mit Link und code")
     && d.extra.textContent.includes("dazwischen") && !d.extra.textContent.includes("installiert") && d.text.includes("2.6.2"),
     d.extra && d.extra.textContent);
  ok("Was ist neu: Release-Seite öffnen (nicht am PS5-Browser)", d.alt && opened.length === 1
     && opened[0][0] === "https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.7.1", opened);
  dialogs = []; fetchCalls = []; ctx.LANG = "it"; ctx.isTv = true; dialogAnswer = "yes";
  fetchImpl = textAnswer(404, "");
  run('showChanges("2.6.2", "2.7.1")');
  for (let i = 0; i < 6; i++) await flush();
  ok("andere Sprache: englisches Changelog", fetchCalls[0] && fetchCalls[0].url.includes("/contents/CHANGELOG.md?ref=v2.7.1"), fetchCalls);
  ok("nicht ladbar: Hinweis statt Liste, am PS5-Browser ohne Release-Knopf", dialogs[0] && !dialogs[0].extra
     && dialogs[0].text.includes("nicht laden") && !dialogs[0].alt, dialogs[0]);
  ctx.LANG = "de"; ctx.isTv = false;
  reset();
  fetchImpl = answer(200, { tag_name: "v2.7.0" });
  run('checkUpdate("2.6.2")');
  await flush(); await flush();
  let prevented = false;
  dialogs = []; fetchImpl = textAnswer(200, log);
  newver.onclick({ preventDefault: () => { prevented = true; } });
  for (let i = 0; i < 6; i++) await flush();
  ok("Hinweis öffnet Was ist neu beim Klick", prevented && dialogs.length === 1 && dialogs[0].title.includes("2.7.0"), dialogs);

  // --------------------------------------------------------------- Sprache aus /api/settings bzw. /api/status (applySettings)
  run(grab("function applySettings("));
  const langCase = async (settings, tv, lang) => {
    let set = null;
    Object.assign(ctx, { quit: false, isTv: tv, LANG: lang, SCALE_KEY: "scale_pc", scaleChangedAt: 0, langChangedAt: 0,
      applyTheme: () => {}, applyScale: () => {}, setLang: (l, save) => { set = [l, save]; }, document: { activeElement: null } });
    els.theme = els.scale = els.lang = makeEl("select");
    run("applySettings(" + JSON.stringify(settings) + ")");
    return set;
  };
  let r = await langCase({ lang: "", system: "it" }, true, "de");
  ok("PS5-Browser ohne Wahl: Systemsprache der PS5", r && r[0] === "it" && r[1] === false, r);
  r = await langCase({ lang: "fr", system: "it" }, true, "de");
  ok("gewählte Sprache vor der Systemsprache", r && r[0] === "fr", r);
  r = await langCase({ lang: "", system: "" }, true, "de");
  ok("Systemsprache keine der fünf: Englisch (nicht die alte Sprache)", r && r[0] === "en", r);
  r = await langCase({ lang: "", system: "it" }, false, "es");
  ok("PC-Browser ohne Wahl: eigene Sprache bleibt", r === null, r);
  ctx.LANG = "de"; ctx.isTv = false;

  // --------------------------------------------------------------- Sprachwechsel: Warteschlange, ShadowMountPlus-Version, Update-Hinweis
  {
    const trDe = ctx.tr, calls = [];
    ctx.tr = (de, v) => (ctx.LANG === "de" ? "" : "[" + ctx.LANG + "] ") + trDe(de, v);
    els.smpver = makeEl("span");
    els.lang = makeEl("select");
    const noop = name => () => calls.push(name);
    Object.assign(ctx, { LANGS: ["de", "en", "it", "es", "fr"], localStorage: { setItem() {}, getItem() { return null; } },
      document: { activeElement: null, documentElement: {} }, applyStatic: noop("static"), showHost: noop("host"),
      renderFs: noop("fs"), update: noop("update"), renderDevices: noop("devices"), renderTile: noop("tile"),
      renderSavesUsers: noop("users"), renderSavesList: noop("saves"), renderQueue: noop("queue"), sysInfo: { smp: null } });
    run(grab("function renderSmpVer(") + grab("function setLang("));
    run('newVer = "2.7.0"; renderSmpVer(); renderNewVer();');
    const before = els.smpver.textContent;
    run('setLang("en", false)');
    ok("Sprachwechsel: Warteschlange neu gezeichnet", calls.includes("queue"), calls);
    ok("Sprachwechsel: ShadowMountPlus „nicht gefunden“ übersetzt",
       before === "ShadowMountPlus nicht gefunden" && els.smpver.textContent === "ShadowMountPlus [en] nicht gefunden", els.smpver.textContent);
    ok("Sprachwechsel: Update-Hinweis übersetzt", newver.firstChild.textContent === "· [en] Neue Version 2.7.0 verfügbar"
       && newver.title.startsWith("[en] "), newver.firstChild.textContent);
    ctx.tr = trDe; ctx.LANG = "de";
  }

  // --------------------------------------------------------------- Abfragen bei verborgener Seite, Controller
  const idle = page.slice(page.indexOf("/* Aufträge und Einstellungen aus anderen Browsern erkennen"),
                          page.indexOf("/* Ergebnis eines Auftrags zeigen */"));
  ok("Leerlauf-Abfrage pausiert bei verborgener Seite und gleicht Einstellungen aus dem Status ab",
     /if \([^)]*document\.hidden\) return;/.test(idle) && idle.includes("applySettings(st.settings)")
     && !page.includes("setInterval(syncSettings"));
  ok("Controller: pausiert bei verborgener Seite", grab("function poll()").includes("if (document.hidden)"));
  ok("Controller: im PS5-Browser sofort, sonst erst nach gamepadconnected",
     page.includes('if (isTv) startPad(); else window.addEventListener("gamepadconnected", startPad);'));
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

  // --------------------------------------------------------------- Diagnose-Bericht: Stopps, unerwartete Enden, Kernel-Log
  run(grab("async function showDiagReport("));
  const diagText = async diag => {
    const elOld = ctx.el;
    Object.assign(ctx, { navigator: { userAgent: "Test" }, el: (...a) => Object.assign(elOld(...a), { style: {} }),
      api: async p => p === "/api/diag" ? diag : null });
    dialogs = []; dialogAnswer = "no";
    await run("showDiagReport()");
    ctx.el = elOld; dialogAnswer = "yes";
    return dialogs.length ? dialogs[0].extra.value : "";
  };
  let dt = await diagText({ quits: [], ends: ["2026-11-13 01:53:36  2.6.67  ended: PS5 restarted or switched off  (noticed x)"],
                            klog: null });
  ok("Diagnose: unerwartete Enden im Bericht",
     dt.includes("\nUnexpected ends:\n  2026-11-13 01:53:36  2.6.67  ended: PS5 restarted or switched off  (noticed x)"), dt);
  dt = await diagText({ quits: [], ends: [], klog: [] });
  ok("Diagnose: keine unerwarteten Enden", dt.includes("\nUnexpected ends:\n  none logged"), dt);

  // =============================================================== Seite mit kleinem DOM: Fokus beim Neuaufbau, Doppelt-Hinweise, Warteschlange
  await domTests();

  console.log(fails ? `${fails} Test(s) fehlgeschlagen.` : "Alle Web-Logik-Tests bestanden.");
  process.exit(fails ? 1 : 0);
})();

// --------------------------------------------------------------- kleines DOM (nur, was die getesteten Funktionen brauchen)
function makeDom() {
  const doc = { activeElement: null };
  const camel = a => a.replace(/-(\w)/g, (m, c) => c.toUpperCase());
  function matchOne(n, comp) {
    const m = /^([a-zA-Z]*)((?:\.[\w-]+)*)(?:\[([\w-]+)\])?$/.exec(comp);
    if (!m || n.tagName === "#TEXT") return false;
    if (m[1] && n.tagName !== m[1].toUpperCase()) return false;
    for (const c of m[2].split(".").filter(Boolean)) if (!n.cls().includes(c)) return false;
    if (m[3]) return m[3].startsWith("data-") ? camel(m[3].slice(5)) in n.dataset : m[3] in n.attrs;
    return true;
  }
  function matches(n, parts) {
    if (!matchOne(n, parts[parts.length - 1])) return false;
    let i = parts.length - 2;
    for (let p = n.parentNode; p && i >= 0; p = p.parentNode) if (matchOne(p, parts[i])) i--;
    return i < 0;
  }
  class Node {
    constructor(tag) {
      Object.assign(this, { tagName: tag.toUpperCase(), children: [], parentNode: null, dataset: {}, style: {}, attrs: {},
                            hidden: false, disabled: false, className: "", listeners: {}, scrollTop: 0, tabIndex: -1, _text: "", options: [] });
    }
    cls() { return this.className.split(/\s+/).filter(Boolean); }
    get classList() {
      const self = this, set = (c, on) => { const l = self.cls().filter(x => x !== c); if (on) l.push(c); self.className = l.join(" "); };
      return { add: c => set(c, true), remove: c => set(c, false), contains: c => self.cls().includes(c),
               toggle: (c, on) => { set(c, on === undefined ? !self.cls().includes(c) : !!on); } };
    }
    get textContent() { return this._text + this.children.map(c => c.textContent).join(""); }
    set textContent(v) {
      const lost = doc.activeElement && doc.activeElement !== this && this.contains(doc.activeElement);
      for (const c of this.children) c.parentNode = null;
      this.children = [];
      this._text = String(v);
      if (lost) doc.activeElement = doc.body;   /* wie im Browser: entfernt -> Fokus auf body */
    }
    set innerHTML(v) { this.textContent = ""; }
    get firstChild() { return this.children[0]; }
    get lastChild() { return this.children[this.children.length - 1]; }
    appendChild(c) {
      if (c.parentNode) c.parentNode.children.splice(c.parentNode.children.indexOf(c), 1);
      c.parentNode = this;
      this.children.push(c);
      return c;
    }
    append(...cs) { for (const c of cs) this.appendChild(typeof c === "string" ? textNode(c) : c); }
    replaceWith(n) { const p = this.parentNode; p.children[p.children.indexOf(this)] = n; n.parentNode = p; this.parentNode = null; }
    contains(x) { for (let n = x; n; n = n.parentNode) if (n === this) return true; return false; }
    focus() { doc.activeElement = this; }
    addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
    click() { for (const f of this.listeners.click || []) f({ preventDefault() {} }); }
    setAttribute(k, v) { this.attrs[k] = String(v); }
    getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
    get offsetParent() {
      for (let n = this; n; n = n.parentNode) { if (n.hidden) return null; if (n === doc.body) return doc.body; }
      return null;
    }
    querySelectorAll(sel) {
      const parts = sel.trim().split(/\s+/), out = [];
      const walk = n => { for (const c of n.children) { if (matches(c, parts)) out.push(c); walk(c); } };
      walk(this);
      return out;
    }
    querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
    add(o) { this.options.push(o); }
    setSelectionRange() {}
  }
  const textNode = t => { const n = new Node("#text"); n._text = String(t); return n; };
  doc.createElement = tag => new Node(tag);
  doc.createTextNode = textNode;
  doc.body = new Node("body");
  doc.activeElement = doc.body;
  doc.querySelector = sel => doc.body.querySelector(sel);
  doc.querySelectorAll = sel => doc.body.querySelectorAll(sel);
  const byId = {};
  const add = (parent, tag, id, hidden) => { const e = new Node(tag); e.id = id; e.hidden = !!hidden; byId[id] = e; parent.appendChild(e); return e; };
  const main = add(doc.body, "main", "main");
  for (const id of ["content", "more", "queue", "status"]) add(main, "div", id, id === "more" || id === "queue");
  add(main, "button", "cancel", true);
  const dlg = add(doc.body, "div", "dlg", true);
  for (const [tag, id] of [["h2", "dtitle"], ["p", "dtext"], ["div", "dextra"], ["p", "dtext2"], ["label", "dack"],
                           ["input", "dackbox"], ["div", "dacktext"], ["button", "dno"], ["button", "dalt"], ["button", "dyes"]])
    add(dlg, tag, id);
  return { doc, byId };
}

async function domTests() {
  const { doc, byId } = makeDom();
  let timers = [];
  const dc = {
    console, document: doc, Option: class { constructor(text, value) { this.text = text; this.value = value; } },
    setTimeout: f => { timers.push(f); return timers.length; }, clearTimeout: () => {},
    localStorage: { store: {}, getItem(k) { return k in this.store ? this.store[k] : null; }, setItem(k, v) { this.store[k] = String(v); } },
    LANG: "de", $: id => byId[id],
    tr: (de, v) => v ? de.replace(/\{(\w+)\}/g, (m, k) => k in v ? String(v[k]) : m) : de,
    fmtSize: b => String(b) + " B", q: s => "„" + s + "“",
    loadGameSizes: () => {}, libScanNote: () => doc.createElement("div"), gameInfo: () => {},
  };
  dc.window = dc;
  vm.createContext(dc);
  const drun = code => vm.runInContext(code, dc);
  const runTimers = () => { const t = timers; timers = []; for (const f of t) f(); };
  drun(`var lib = { titles: [], roots: [], duplicates: [], partials: [], pkgfiles: [] }, devices = [], selected = null, selectedPlace = null,
        busy = false, starting = false, pollSettling = false, pickMode = false, pickAction = "delete", picked = new Set(), gamesQuery = "",
        onlyMounted = false, mountedKeys = new Set(), gamesSort = "name", gameSizes = {}, noIcon = new Set(), showAll = false,
        MAX_DELETE = 256, dlgResolve = null, dlgReturn = null, dlgLastReturn = null, moveSource = null,
        copyQueue = [], queueRunning = false, queueLog = [], queueOk = true, queueDropped = [];`);
  for (const h of ["function el(", "const knownIdent =", "const identOf =", "const sameStick =", "function places(",
                   "function titlesOf(", "function rootsOf(", "function dupsOf(", "function pkgsOf(", "function usageOf(",
                   "function countText(", "function shortPath(", "function stat(", "function note(", "function typeChip(",
                   "function gameTypeChip(", "const gameSize =", "function gameVisible(", "function verCmp(", "function sortGames(",
                   "function moveTargets(", "function renderContent(", "function preselectGames(", "function setPickMode(",
                   "function dialog(", "function closeDialog(", "function refind(",
                   "function restoredQueue(", "function renderQueue(", "function queueAdd(", "function queueDrive(",
                   "function startQueue(", "async function startNextQueued(", "function finishQueue(", "function queueBlocks(",
                   "async function runJob("])
    drun(grab(h));

  // ------------------------------------------------- Fokus beim Neuaufbau der Spieleliste (Controller)
  const D = { name: "da0", size: 64000, ident: "S1", format: "exFAT", mounts: [{ path: "/mnt/usb0", total: 100000, free: 50000 }] };
  const A = { key: "k1", name: "Alpha", path: "/mnt/usb0/homebrew/A", device: "da0", kind: "Ordner", id: "PPSA00001", version: "01.000.000" };
  const A2 = { key: "k2", name: "Alpha", path: "/mnt/usb0/homebrew/A2", device: "da0", kind: "Ordner", id: "PPSA00001", version: "01.000.000" };
  const B = { key: "k3", name: "Beta", path: "/mnt/usb0/homebrew/B.ffpkg", device: "da0", kind: "UFS2", ext: "ffpkg", size: 500, id: "PPSA00002" };
  Object.assign(dc, { currentView: () => D, currentDevice: () => D });
  dc.devices = [D];
  dc.lib = { titles: [A, A2, B], roots: [{ device: "da0", root: "/mnt/usb0", pkgs: 0, usb: true }],
             duplicates: [{ id: "PPSA00001", name: "Alpha", paths: [A.path, A2.path] }], partials: [{ device: "da0", bytes: 1000 }], pkgfiles: [] };
  dc.gameSizes = { k1: 1000, k2: 1000 };
  dc.mountedKeys = new Set(["k3"]);
  const content = byId.content;
  const byKey = k => content.querySelectorAll("[data-key]").find(x => x.dataset.key === k);
  drun("renderContent()");
  const dupKey = "|" + A.path + "|" + A2.path;
  const keys = ["k3", "__mounted", "__dupsel" + dupKey, "__dupcmp" + dupKey, "__partial", "__copy", "__find"];
  ok("Spieleliste: Zeilen und Knöpfe haben Schlüssel", keys.every(k => byKey(k)), keys.filter(k => !byKey(k)));
  for (const k of keys) {
    const old = byKey(k);
    if (!old) continue;
    old.focus();
    drun("renderContent()");
    const now = doc.activeElement;
    ok("Neuaufbau: Fokus bleibt auf " + k, now !== old && now.dataset && now.dataset.key === k && content.contains(now),
       now && now.tagName + " " + JSON.stringify(now.dataset));
  }
  ok("Spielzeile ist eine Spielzeile (game info, tabIndex 0)", byKey("k3").cls().includes("info") && byKey("k3").tabIndex === 0);

  // Einzelheiten-Dialog: während er offen ist, wird die Liste neu aufgebaut - danach zurück auf die neue Zeile desselben Spiels
  byKey("k1").focus();
  const oldRow = doc.activeElement;
  drun('dialog({ title: "Alpha", text: "", yes: "Schließen", no: false })');
  ok("Dialog: Fokus im Dialog", byId.dlg.contains(doc.activeElement));
  drun("renderContent()");   /* z. B. pollGameSizes alle 1,5 s */
  ok("Dialog: alte Zeile ist ersetzt", !doc.body.contains(oldRow));
  drun('closeDialog("yes")');
  runTimers();
  ok("Dialog geschlossen: Fokus auf der neuen Zeile desselben Spiels", doc.activeElement !== oldRow && doc.activeElement.dataset.key === "k1"
     && content.contains(doc.activeElement), doc.activeElement && doc.activeElement.dataset);
  ok("refind: Element noch da -> unverändert", drun("refind")(byKey("k3")) === byKey("k3"));

  // ------------------------------------------------- Doppelt-Hinweise wählen nur Sichtbares vor (Suche wird zurückgesetzt)
  dc.gamesQuery = "beta";
  dc.onlyMounted = true;
  drun("renderContent()");
  ok("Filter aktiv: Alpha ausgeblendet", !content.querySelectorAll(".game").some(g => g.dataset.key === "k2"));
  byKey("__dupsel" + dupKey).click();
  const boxes = content.querySelectorAll(".game.pick input");
  ok("Überzählige Kopie auswählen: Filter zurückgesetzt, Kopie vorgewählt und zu sehen",
     dc.gamesQuery === "" && dc.onlyMounted === false && dc.pickMode && dc.pickAction === "delete" && dc.picked.size === 1 && dc.picked.has("k2")
     && boxes.some(b => b.dataset.key === "k2" && b.checked), [dc.gamesQuery, dc.onlyMounted, Array.from(dc.picked)]);
  ok("Auswahlleiste zählt richtig (1 von 3 sichtbar: „Alle“)", content.querySelector(".pickbar").textContent.includes("1 ausgewählt")
     && byKey("__all").textContent === "Alle", content.querySelector(".pickbar").textContent);
  drun("setPickMode(false)");

  // ------------------------------------------------- Warteschlange: Wiederherstellen nach dem Neuladen
  const it = o => Object.assign({ path: "/api/move", body: { keys: "k1", device: "da1", verify: true }, label: "L" }, o);
  const rq = drun("restoredQueue")([
    it({ label: "gut", ident: "S2", source: { dev: "da0", keys: ["k1"], ident: "S1" } }),
    it({ label: "Ziel ohne Nummer", ident: "", tsize: 5, source: { dev: "da0", ident: "S1" } }),
    it({ label: "alt (ohne Quelle)", ident: "S2", source: { dev: "da0", keys: ["k1"], copyOnly: true } }),
    it({ label: "Quelle ohne Nummer", ident: "S2", source: { dev: "da0", ident: "", size: 5 } }),
    it({ label: "nach intern", ident: "", tplace: true, body: { keys: "k1", device: "intern" }, source: { dev: "da0", ident: "S1" } }),
    it({ label: "von intern", ident: "S2", source: { dev: "intern", place: true, ident: "" } }),
    null, { path: "/api/move", body: {} }]);
  ok("Wiederherstellen: nur Aufträge mit Seriennummer bzw. festem Ort", rq.items.map(x => x.label).join("|") === "gut|nach intern|von intern"
     && rq.dropped.join("|") === "Ziel ohne Nummer|alt (ohne Quelle)|Quelle ohne Nummer", rq);
  ok("Wiederherstellen: Unsinn ergibt nichts", drun("restoredQueue")(null).items.length === 0 && drun("restoredQueue")({}).dropped.length === 0);
  dc.queueDropped = rq.dropped;
  dc.copyQueue = [];
  drun("renderQueue()");
  const qb = byId.queue;
  ok("Hinweis auf verworfene Aufträge sichtbar, mit Namen", !qb.hidden && qb.textContent.includes("nicht übernommen")
     && qb.textContent.includes("Ziel ohne Nummer") && qb.textContent.includes("Quelle ohne Nummer"), qb.textContent);
  const closeBtn = qb.querySelectorAll("button").find(b => b.textContent === "Schließen");
  ok("Nur der Hinweis: Knopf „Schließen“, kein Start", !!closeBtn && !qb.textContent.includes("Warteschlange starten"));
  closeBtn.click();
  ok("Schließen blendet den Hinweis aus", qb.hidden && dc.queueDropped.length === 0);

  // ------------------------------------------------- Warteschlange: Einstellen merkt Quelle und Ziel
  const T = { name: "da1", size: 32000, ident: "S2", format: "exFAT", mounts: [{ path: "/mnt/usb1", total: 1, free: 1 }] };
  dc.devices = [D, T];
  let apiCalls = [], refused = [], results = [];
  Object.assign(dc, { showMsg: () => {}, renderTabs: () => {}, setProgress: () => {}, startPolling: () => {}, revealProgress: () => {},
                      api: async (path, body) => { apiCalls.push({ path, body }); return {}; },
                      jobRefused: t => { refused.push(t); }, showResult: (t, text, good) => { results.push({ t, text, ok: good }); } });
  drun('queueAdd("/api/move", { keys: "k1,k3", device: "da1", verify: true }, { dev: "da0", keys: ["k1", "k3"], copyOnly: true }, ["Alpha", "Beta"], "da1")');
  drun('queueAdd("/api/move", { keys: "k3", device: "intern", verify: true }, { dev: "da0", keys: ["k3"], copyOnly: true }, ["Beta"], "intern")');
  const [q1, q2] = dc.copyQueue;
  ok("Einstellen: Ziel und Quelle mit Seriennummer und Größe", q1.ident === "S2" && q1.tsize === 32000 && !q1.tplace
     && q1.source.ident === "S1" && q1.source.size === 64000 && !q1.source.place && q1.source.copyOnly, q1);
  ok("Einstellen: fester Ort als Ziel", q2.tplace === true && q2.ident === "", q2);
  ok("Eingestellt übersteht ein Neuladen", drun("restoredQueue")(JSON.parse(dc.localStorage.getItem("usbfmt-queue"))).items.length === 2);

  // Warteschlange abarbeiten; jeder Lauf endet mit dem Bericht (showResult) bzw. dem gestarteten Auftrag
  const runQueue = async items => {
    dc.queueRunning = false; dc.busy = false; dc.starting = false;
    dc.copyQueue = items.map(x => Object.assign({}, x, { body: Object.assign({}, x.body) }));
    apiCalls = []; results = [];
    drun("startQueue()");
    for (let i = 0; i < 4; i++) { await flush(); runTimers(); }
  };
  // beide Sticks neu angemeldet (andere daN): Ziel und Quelle über die Seriennummer
  const D2 = Object.assign({}, D, { name: "da4" }), T2 = Object.assign({}, T, { name: "da3" });
  dc.devices = [T2, D2];
  dc.lib = Object.assign({}, dc.lib, { titles: [A, A2, B].map(t => Object.assign({}, t, { device: "da4" })) });
  await runQueue([q1]);
  ok("Umgesteckt: über die Seriennummer gefunden, Auftrag geht an da3", apiCalls.length === 1 && apiCalls[0].body.device === "da3"
     && apiCalls[0].body.keys === "k1,k3", apiCalls);
  // ein anderer Stick steckt jetzt unter dem Namen der Quelle (gleiche Pfade), der eigentliche fehlt: die Schlüssel hängen am Pfad
  dc.devices = [Object.assign({}, D, { ident: "FREMD" }), T];
  dc.lib = Object.assign({}, dc.lib, { titles: [A, A2, B] });
  await runQueue([q1]);
  ok("Quelle durch anderen Stick ersetzt: nicht gestartet, im Bericht", apiCalls.length === 0 && results.length === 1
     && results[0].text.includes("Quell-Laufwerk bzw. Auswahl nicht gefunden") && results[0].ok === false, [apiCalls, results]);
  // Quelle da, aber die Spiele sind nicht mehr auf ihr
  dc.devices = [D, T];
  dc.lib = Object.assign({}, dc.lib, { titles: [B] });
  await runQueue([q1]);
  ok("Spiele nicht mehr auf der Quelle: nicht gestartet", apiCalls.length === 0 && results.length === 1
     && results[0].text.includes("Auswahl nicht gefunden"), results);
  // Ziel fehlt
  dc.devices = [D];
  dc.lib = Object.assign({}, dc.lib, { titles: [A, A2, B] });
  await runQueue([q1]);
  ok("Ziel fehlt: nicht umgeleitet", apiCalls.length === 0 && results.length === 1 && results[0].text.includes("Ziel-Laufwerk nicht gefunden"), results);
  // in derselben Sitzung eingestellt, Ziel ohne Seriennummer: nur derselbe Name und dieselbe Größe, ohne Seriennummer
  const N = { name: "da1", size: 32000, ident: "", format: "exFAT", mounts: [{ path: "/mnt/usb1", total: 1, free: 1 }] };
  dc.devices = [D, N];
  dc.copyQueue = [];
  drun('queueAdd("/api/move", { keys: "k1", device: "da1", verify: true }, { dev: "da0", keys: ["k1"], copyOnly: true }, ["Alpha"], "da1")');
  const q3 = dc.copyQueue[0];
  await runQueue([q3]);
  ok("Ziel ohne Seriennummer (gleiche Sitzung, derselbe Stick): gestartet", q3.ident === "" && apiCalls.length === 1
     && apiCalls[0].body.device === "da1", apiCalls);
  dc.devices = [D, Object.assign({}, N, { size: 16000 })];
  await runQueue([q3]);
  ok("Ziel ohne Seriennummer, anderer Stick unter dem Namen: nicht gestartet", apiCalls.length === 0 && results.length === 1, results);
  dc.devices = [D, Object.assign({}, N, { ident: "NEU" })];
  await runQueue([q3]);
  ok("Ziel ohne Seriennummer, dort steckt jetzt ein Stick mit Nummer: nicht gestartet", apiCalls.length === 0 && results.length === 1, results);

  // ------------------------------------------------- eigene Aufträge, während die Warteschlange läuft (auch in der Pause dazwischen)
  dc.queueRunning = true; dc.busy = false; dc.starting = false;
  apiCalls = []; refused = [];
  const started = await drun('runJob("/api/move", { keys: "k3", device: "da1" })');
  ok("Warteschlange läuft: eigener Auftrag abgelehnt", started === false && apiCalls.length === 0 && refused.length === 1
     && refused[0].includes("Warteschlange läuft"), [started, refused]);
  const startedQ = await drun('runJob("/api/move", { keys: "k3", device: "da1" }, true)');
  ok("Warteschlange läuft: ihr eigener Auftrag startet", startedQ === true && apiCalls.length === 1);
  dc.queueRunning = false;
  apiCalls = []; refused = [];
  ok("ohne Warteschlange: eigener Auftrag startet", (await drun('runJob("/api/move", { keys: "k3", device: "da1" })')) === true
     && apiCalls.length === 1 && refused.length === 0);
  ok("nächster Auftrag der Warteschlange wartet, solange ein Ergebnis noch verarbeitet wird (pollSettling)",
     /if \(busy \|\| starting \|\| pollSettling\) \{ setTimeout\(startNextQueued/.test(grab("async function startNextQueued(")));
}
