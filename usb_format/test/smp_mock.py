"""Attrappe der ShadowMountPlus-API (1.7) für die Host-Tests: smp_mock.py <port> <games.json> <protokoll>

games.json: [{"title_id": "...", "path": "...", "busy": false}, ...] - "busy" lässt delete/uninstall mit 409 scheitern.
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
installed = {g["title_id"] for g in games}   # Kacheln: bleiben nach dem Löschen der Quelle, bis deinstalliert
jobs = {}


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

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        tid = req.get("title_id", "")
        g = next((x for x in games if x["title_id"] == tid), None)
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
            return self.reply(200, {"status": 0, "job_id": jid, "operation": "delete",
                                    "state": "completed" if done else "deleting",
                                    "progress_percent": 100 if done else 50, "result_status": 0, "result_error": ""})
        self.reply(404, {"status": 2, "error": "route"})


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
