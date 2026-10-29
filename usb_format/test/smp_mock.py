"""Attrappe der ShadowMountPlus-API (1.7) für die Host-Tests: smp_mock.py <port> <games.json> <protokoll> [<mounts.json>]

games.json: [{"title_id": "...", "path": "...", "busy": false}, ...] - "busy" lässt delete/uninstall mit 409 scheitern;
"icon": true - GET /api/v1/games/icon liefert ein (winziges) PNG.
mounts.json: Laufwerksliste für /api/v1/storage, [{"source", "mount_point", "ejectable", "mount_id", "fail": "<Code>"}];
storage/eject prüft source und mount_id wie ShadowMountPlus und meldet "fail" als result_error des Auftrags.
SMP_MOCK_NOVERSION=1: /api/v1/version fehlt (frühe 1.7-Fassungen).
"version" ist die angemeldete contentVersion, "rescan_version" die nach einem neuen Anmelden (scan nach uninstall);
"noreg": true - scan meldet diesen Titel nicht wieder an.
delete löscht den Pfad wirklich (wie ShadowMountPlus) und meldet über storage/status erst "deleting", dann
"completed". Jeder Aufruf landet als Zeile "<route> <title_id>" im Protokoll."""
import http.server
import json
import os
import shutil
import sys

PORT, GAMES, LOG = int(sys.argv[1]), sys.argv[2], sys.argv[3]
games = json.load(open(GAMES))
mounts = json.load(open(sys.argv[4])) if len(sys.argv) > 4 else []
ICON = b"\x89PNG\r\n\x1a\n" + b"\0" * 24   # genügt dem Formatierer (er prüft nur die Signatur)
installed = {g["title_id"] for g in games}   # Kacheln: bleiben nach dem Löschen der Quelle, bis deinstalliert
jobs = {}
ejects = {}   # Auftrag -> Einbindung (Auswerfen)


def log(line):
    with open(LOG, "a") as f:
        f.write(line + "\n")


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        from urllib.parse import parse_qs, urlparse
        u = urlparse(self.path)
        tid = parse_qs(u.query).get("title_id", [""])[0]
        g = next((x for x in games if x["title_id"] == tid), None)
        log(f"icon {tid}")
        if u.path != "/api/v1/games/icon" or not g or not g.get("icon"):
            return self.reply(404, {"status": 2, "error": "no icon"})
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(ICON)))
        self.end_headers()
        self.wfile.write(ICON)

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        tid = req.get("title_id", "")
        g = next((x for x in games if x["title_id"] == tid), None)
        if self.path == "/api/v1/version":
            if os.environ.get("SMP_MOCK_NOVERSION"):
                return self.reply(404, {"status": 2, "error": "route"})
            return self.reply(200, {"status": 0, "api_version": 1, "shadowmount_version": "1.7",
                                    "capabilities": ["list_games", "delete_game_source", "storage_eject", "game_icon"]})
        if self.path == "/api/v1/storage":
            return self.reply(200, {"status": 0, "mounts": [{k: v for k, v in m.items() if k != "fail"} for m in mounts],
                                    "destinations": []})
        if self.path == "/api/v1/storage/eject":
            log(f"eject {req.get('path')} {req.get('source')} {req.get('mount_id')}")
            m = next((x for x in mounts if x["mount_point"] == req.get("path")), None)
            if not m or not m.get("ejectable") or len(req.get("mount_id", "")) != 16:
                return self.reply(400, {"status": 22, "error": "invalid_request"})
            if m["source"] != req.get("source") or m["mount_id"] != req["mount_id"]:
                return self.reply(409, {"status": 16, "error": "usb_mount_changed"})
            jid = len(jobs) + 1
            jobs[jid] = 0
            ejects[jid] = m
            return self.reply(202, {"status": 0, "job_id": jid, "operation": "eject", "state": "preparing"})
        if self.path == "/api/v1/games":
            return self.reply(200, {"status": 0, "count": len(games), "games": [
                {"title_id": x["title_id"], "path": x["path"], "title_name": x["title_id"],
                 "version": x.get("version", "") if x["title_id"] in installed else "",
                 "installed": x["title_id"] in installed} for x in games]})
        if self.path == "/api/v1/scan":   # wie ShadowMountPlus: Fehlendes anmelden, Angemeldetes in Ruhe lassen
            log(f"scan reset_attempts={str(req.get('reset_attempts', False)).lower()}")
            for x in games:
                if x["title_id"] not in installed and not x.get("noreg"):
                    installed.add(x["title_id"])
                    x["version"] = x.get("rescan_version", x.get("version", ""))
            return self.reply(200, {"status": 0, "queued": True, "scan_queued": True})
        if self.path in ("/api/v1/games/delete", "/api/v1/games/uninstall"):
            log(f"{self.path.rsplit('/', 1)[1]} {tid}")
            if g and g.get("busy"):
                return self.reply(409, {"status": 16, "error": "EBUSY"})
            if self.path.endswith("uninstall"):
                if tid not in installed:
                    return self.reply(404, {"status": 2, "error": "not installed"})
                installed.discard(tid)
                return self.reply(200, {"status": 0})
            if not g:
                return self.reply(404, {"status": 2, "error": "unknown title"})
            if not req.get("confirm"):
                return self.reply(400, {"status": 22, "error": "confirm required"})
            p = g["path"]
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
            games.remove(g)
            jid = len(jobs) + 1
            jobs[jid] = 0
            return self.reply(202, {"status": 0, "job_id": jid, "state": "preparing"})
        if self.path == "/api/v1/games/storage/status":
            jid = req.get("job_id")
            if jid not in jobs:
                return self.reply(404, {"status": 2, "error": "no job"})
            jobs[jid] += 1
            done = jobs[jid] > 1
            m = ejects.get(jid)
            if m and done and m.get("fail"):
                return self.reply(200, {"status": 0, "job_id": jid, "operation": "eject", "state": "failed",
                                        "progress_percent": 50, "result_status": 16, "result_error": m["fail"]})
            if m and done:
                mounts.remove(m)
            return self.reply(200, {"status": 0, "job_id": jid, "operation": "eject" if m else "delete",
                                    "state": "completed" if done else "ejecting" if m else "deleting",
                                    "progress_percent": 100 if done else 50, "result_status": 0, "result_error": ""})
        self.reply(404, {"status": 2, "error": "route"})


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
