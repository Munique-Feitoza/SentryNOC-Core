#!/usr/bin/env python3
"""SentryNOC Core collector: HTTP ingest, SQLite retention and SSE dashboard feed."""
import json
import os
import sqlite3
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = os.getenv("SENTRYNOC_HOST", "127.0.0.1")
PORT = int(os.getenv("SENTRYNOC_PORT", "8787"))
DB_PATH = os.getenv("SENTRYNOC_DB", "sentrynoc.db")
TOKEN = os.getenv("SENTRYNOC_TOKEN", "")
WEBHOOK = os.getenv("SENTRYNOC_WEBHOOK", "")
MAX_ROWS = int(os.getenv("SENTRYNOC_RETENTION", "100000"))
PSI_ALERT = float(os.getenv("SENTRYNOC_PSI_ALERT", "20"))
ALERT_COOLDOWN = int(os.getenv("SENTRYNOC_ALERT_COOLDOWN", "300"))

DB_LOCK = threading.Lock()
SUBSCRIBERS = []
SUBSCRIBERS_LOCK = threading.Lock()
LAST_ALERT = {}


def db():
    connection = sqlite3.connect(DB_PATH, timeout=5)
    connection.execute(
        """CREATE TABLE IF NOT EXISTS telemetry (
             id INTEGER PRIMARY KEY AUTOINCREMENT,
             received_at INTEGER NOT NULL,
             sequence INTEGER NOT NULL,
             timestamp_ms INTEGER NOT NULL,
             payload TEXT NOT NULL
           )"""
    )
    connection.execute("CREATE INDEX IF NOT EXISTS telemetry_ts ON telemetry(timestamp_ms)")
    connection.commit()
    return connection


def save(payload):
    with DB_LOCK:
        connection = db()
        connection.execute(
            "INSERT INTO telemetry(received_at, sequence, timestamp_ms, payload) VALUES (?, ?, ?, ?)",
            (int(time.time() * 1000), int(payload.get("seq", 0)), int(payload.get("ts", 0)), json.dumps(payload, separators=(",", ":"))),
        )
        connection.execute(
            "DELETE FROM telemetry WHERE id NOT IN (SELECT id FROM telemetry ORDER BY id DESC LIMIT ?)",
            (MAX_ROWS,),
        )
        connection.commit()
        connection.close()
    notify(payload)


def latest(limit=120):
    with DB_LOCK:
        connection = db()
        rows = connection.execute("SELECT payload FROM telemetry ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        connection.close()
    return [json.loads(row[0]) for row in reversed(rows)]


def notify(payload):
    message = "data: " + json.dumps(payload, separators=(",", ":")) + "\n\n"
    with SUBSCRIBERS_LOCK:
        subscribers = list(SUBSCRIBERS)
    for stream in subscribers:
        try:
            stream.write(message.encode())
            stream.flush()
        except (BrokenPipeError, ConnectionResetError):
            with SUBSCRIBERS_LOCK:
                if stream in SUBSCRIBERS:
                    SUBSCRIBERS.remove(stream)
    send_alert_if_needed(payload)


def send_alert_if_needed(payload):
    if not WEBHOOK:
        return
    checks = (
        ("memory_psi_full", payload.get("psi", {}).get("memory", {}).get("full_avg10", 0)),
        ("io_psi_full", payload.get("psi", {}).get("io", {}).get("full_avg10", 0)),
    )
    now = time.time()
    for name, value in checks:
        if value < PSI_ALERT or now - LAST_ALERT.get(name, 0) < ALERT_COOLDOWN:
            continue
        alert = {
            "source": "sentrynoc",
            "severity": "critical",
            "rule": name,
            "value": value,
            "threshold": PSI_ALERT,
            "sequence": payload.get("seq"),
            "timestamp": payload.get("ts"),
        }
        request = urllib.request.Request(
            WEBHOOK,
            data=json.dumps(alert).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=5):
                pass
            LAST_ALERT[name] = now
        except Exception as error:
            print("alert webhook failed: %s" % error)


def authorized(handler):
    return not TOKEN or handler.headers.get("Authorization", "") == "Bearer " + TOKEN


class Handler(BaseHTTPRequestHandler):
    server_version = "SentryNOC/0.1"

    def send_json(self, status, value):
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/healthz":
            self.send_json(200, {"ok": True})
        elif self.path == "/api/v1/latest":
            if not authorized(self):
                self.send_json(401, {"error": "unauthorized"})
            else:
                self.send_json(200, {"data": latest()})
        elif self.path == "/events":
            if not authorized(self):
                self.send_json(401, {"error": "unauthorized"})
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            with SUBSCRIBERS_LOCK:
                SUBSCRIBERS.append(self.wfile)
            try:
                self.wfile.write(b": connected\n\n")
                self.wfile.flush()
                while True:
                    time.sleep(30)
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                with SUBSCRIBERS_LOCK:
                    if self.wfile in SUBSCRIBERS:
                        SUBSCRIBERS.remove(self.wfile)
        elif self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            body = DASHBOARD.encode()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_json(404, {"error": "not_found"})

    def do_POST(self):
        if self.path != "/api/v1/ingest":
            self.send_json(404, {"error": "not_found"})
            return
        if not authorized(self):
            self.send_json(401, {"error": "unauthorized"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode()
            records = [json.loads(line) for line in raw.splitlines() if line.strip()]
            if not records:
                records = [json.loads(raw)]
            for payload in records:
                if payload.get("v") != 1 or payload.get("type") != "telemetry":
                    raise ValueError("unsupported telemetry protocol")
                save(payload)
            self.send_json(202, {"accepted": len(records)})
        except (ValueError, json.JSONDecodeError) as error:
            self.send_json(400, {"error": str(error)})

    def log_message(self, format, *args):
        print("%s - %s" % (self.address_string(), format % args))


DASHBOARD = """<!doctype html>
<html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>SentryNOC Core</title>
<style>
body{background:#0b1220;color:#e5e7eb;font:15px system-ui;margin:0;padding:24px}
main{max-width:1100px;margin:auto}h1{margin:0 0 4px}.muted{color:#94a3b8}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px;margin:24px 0}
.card{background:#111c30;border:1px solid #263653;border-radius:12px;padding:16px}.value{font-size:27px;font-weight:700;margin-top:8px}
table{width:100%;border-collapse:collapse;background:#111c30}td,th{padding:10px;text-align:left;border-bottom:1px solid #263653}
.ok{color:#4ade80}.warn{color:#fbbf24}
</style><main><h1>SentryNOC Core</h1><div class="muted">telemetria local em tempo real</div>
<section class="grid"><div class="card">Memória disponível<div id="mem" class="value">—</div></div>
<div class="card">Pressão de memória<div id="psi" class="value">—</div></div>
<div class="card">Recebido<div id="ts" class="value">—</div></div>
<div class="card">Rede RX / TX<div id="net" class="value">—</div></div></section>
<table><thead><tr><th>Seq</th><th>CPU total (jiffies)</th><th>Disco I/O ms</th><th>PSI I/O</th></tr></thead><tbody id="rows"></tbody></table>
<script>
const fmt=n=>new Intl.NumberFormat('pt-BR').format(n);
function render(x){if(!x)return;document.querySelector('#mem').textContent=fmt(x.memory_kb.available)+' KB';
document.querySelector('#psi').textContent=x.psi.memory.some_avg10.toFixed(2)+'%';
document.querySelector('#ts').textContent=new Date(x.ts).toLocaleTimeString();
document.querySelector('#net').textContent=fmt(x.network.rx_bytes)+' / '+fmt(x.network.tx_bytes);
const cpu=Object.values(x.cpu).reduce((a,b)=>a+b,0);const row='<tr><td>'+x.seq+'</td><td>'+fmt(cpu)+'</td><td>'+fmt(x.disk.io_ms)+'</td><td>'+x.psi.io.some_avg10.toFixed(2)+'%</td></tr>';
document.querySelector('#rows').insertAdjacentHTML('afterbegin',row);while(document.querySelectorAll('#rows tr').length>20)document.querySelector('#rows tr:last-child').remove();}
fetch('/api/v1/latest').then(r=>r.json()).then(x=>x.data.forEach(render));new EventSource('/events').onmessage=e=>render(JSON.parse(e.data));
</script></main></html>"""


if __name__ == "__main__":
    db().close()
    print("SentryNOC collector listening on http://%s:%s" % (HOST, PORT))
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
