#!/usr/bin/env python3
"""
sandfleaOS — Real-Time Serial Log Dashboard
============================================
Single-file, zero-dependency (Python ≥3.6) SSE server that streams
QEMU serial output to a browser with tabbed channels.

Usage:
    python log_server.py                 # default: port 8079
    python log_server.py --port 9000     # custom port

Then open http://localhost:8079 in your browser.
"""

import os
import sys
import socket
import time
import json
import queue
import argparse
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.parse import urlparse

from profile_to_flame import to_speedscope


class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    """Handle each request in its own thread so multiple SSE streams
    can serve concurrently."""
    daemon_threads = True

# ── Configuration ─────────────────────────────────────────────────────────────

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(BASE_DIR)

CHANNELS = {
    "primary": {
        "file": os.path.join(PROJECT_DIR, "serial_output.log"),
        "label": "Primary (COM1)",
        "color": "#4ade80",
        "color_rgb": "74, 222, 128",
    },
    "test": {
        "file": os.path.join(PROJECT_DIR, "test_log.txt"),
        "label": "Test (COM2)",
        "color": "#60a5fa",
        "color_rgb": "96, 165, 250",
    },
    "profile": {
        "file": os.path.join(PROJECT_DIR, "profile.log"),
        "label": "Profile (COM3)",
        "color": "#f472b6",
        "color_rgb": "244, 114, 182",
    },
}

POLL_INTERVAL = 0.3   # seconds between file checks
MAX_LINES_INITIAL = 300  # lines sent on first connect

# ── Log Watcher (per-channel file tail) ───────────────────────────────────────

class LogWatcher:
    """Tails a single log file, tracks position, detects truncation."""

    def __init__(self, path):
        self.path = path
        self._lock = threading.Lock()  # protects _pos & _listeners
        self._pos = 0       # byte offset (written by watcher + handler threads)
        self._listeners = []  # list of queue.Queue (guarded by _lock)

    def subscribe(self):
        """Return a queue that will receive (event_type, data) tuples."""
        q = queue.Queue()
        with self._lock:
            self._listeners.append(q)
        # Send initial backlog
        backlog = self._read_backlog()
        for line in backlog:
            q.put(("append", line))
        return q

    def unsubscribe(self, q):
        with self._lock:
            if q in self._listeners:
                self._listeners.remove(q)

    def _read_backlog(self):
        """Read the last N lines from current file state (for new connections).
        Seeks near end to avoid reading the entire file."""
        lines = []
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(0, 2)  # end
                size = f.tell()
                # Read last ~64KB — faster than reading whole file
                start = max(0, size - 65536)
                f.seek(start)
                if start > 0:
                    f.readline()  # discard partial first line
                all_lines = f.readlines()
                lines = all_lines[-MAX_LINES_INITIAL:]
            with self._lock:
                self._pos = size
        except FileNotFoundError:
            pass
        return [l.rstrip("\n") for l in lines]

    def _broadcast(self, event, data):
        with self._lock:
            # Unbounded Queue; put_nowait never raises Full.
            # Copy list to avoid mutation-during-iteration if unsubscribe
            # runs concurrently (though unsubscribe holds _lock too).
            for q in list(self._listeners):
                q.put_nowait((event, data))

    def poll(self):
        """Check for new content or truncation. Call from watcher thread."""
        try:
            if not os.path.exists(self.path):
                return  # file doesn't exist yet, wait

            # Detect truncation (file smaller than our position → was cleared)
            size = os.path.getsize(self.path)
            if size < self._pos:
                self._pos = 0
                self._broadcast("clear", "")

            if size <= self._pos:
                return  # no new data

            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(self._pos)
                new_data = f.read()
                self._pos = f.tell()

            if new_data:
                lines = new_data.split("\n")
                # The last element may be incomplete (no trailing newline yet)
                for line in lines[:-1]:
                    self._broadcast("append", line)
                # Keep the incomplete tail for next poll
                if lines[-1] != "":
                    self._pos -= len(lines[-1].encode("utf-8", errors="replace"))
        except (IOError, OSError):
            pass  # transient error, retry next poll


# ── SSE Request Handler ───────────────────────────────────────────────────────

class SSEHandler(BaseHTTPRequestHandler):
    """Handles HTTP: / → dashboard, /stream/<channel> → SSE, /api/status → JSON."""

    # Class-level shared state (set by server before starting)
    watchers = {}
    server_start_time = 0

    def log_message(self, fmt, *args):
        """Suppress default stderr logging; we write structured output."""
        pass

    def _send_headers(self, status=200, content_type="text/html", extra=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/")

        # ── Static dashboard page ─────────────────────────────────────────
        if path == "" or path == "/":
            self._send_headers(200, "text/html; charset=utf-8")
            self.wfile.write(DASHBOARD_HTML.encode("utf-8"))

        # ── SSE stream endpoints ──────────────────────────────────────────
        elif path.startswith("/stream/"):
            channel = path.split("/")[-1]
            if channel not in CHANNELS:
                self._send_headers(404, "text/plain")
                self.wfile.write(b"Unknown channel")
                return

            self._send_headers(200, "text/event-stream",
                               extra={"Connection": "keep-alive",
                                      "X-Accel-Buffering": "no"})
            q = self.watchers[channel].subscribe()
            try:
                # Send initial connection event
                self.wfile.write(f"event: status\ndata: connected\n\n".encode("utf-8"))
                self.wfile.flush()

                while True:
                    try:
                        event, data = q.get(timeout=1.0)
                        # Sanitize: SSE doesn't allow \n in data without prefixing
                        payload = data.replace("\n", "\ndata: ")
                        self.wfile.write(f"event: {event}\ndata: {payload}\n\n".encode("utf-8"))
                        self.wfile.flush()
                    except queue.Empty:
                        # Send keepalive comment to prevent proxy timeouts.
                        # If the client disconnected, the write will fail —
                        # catch that and let the outer handler clean up.
                        try:
                            self.wfile.write(b": ping\n\n")
                            self.wfile.flush()
                        except (BrokenPipeError, ConnectionResetError,
                                ConnectionAbortedError, OSError):
                            return
            except (BrokenPipeError, ConnectionResetError,
                    ConnectionAbortedError):
                pass
            finally:
                self.watchers[channel].unsubscribe(q)

        # ── Flame graph data API ──────────────────────────────────────────
        elif path == "/flame-data":
            self._send_headers(200, "application/json",
                               extra={"Access-Control-Allow-Origin": "*"})
            ch = CHANNELS.get("profile", {})
            json_str = to_speedscope(ch.get("file", "profile.log"))
            self.wfile.write(json_str.encode("utf-8"))

        # ── Status API ────────────────────────────────────────────────────
        elif path == "/api/status":
            status = {}
            for name, ch in CHANNELS.items():
                exists = os.path.exists(ch["file"])
                size = os.path.getsize(ch["file"]) if exists else 0
                status[name] = {"exists": exists, "size": size}
            self._send_headers(200, "application/json")
            self.wfile.write(json.dumps(status).encode("utf-8"))

        # ── Static files (node_modules) ──────────────────────────────────
        elif path.startswith("/static/"):
            # Serve files from dashboard/node_modules/
            rel = path[len("/static/"):]
            # Security: prevent directory traversal
            if ".." in rel or rel.startswith("/"):
                self._send_headers(403, "text/plain")
                self.wfile.write(b"Forbidden")
                return
            filepath = os.path.join(BASE_DIR, "node_modules", rel)
            if not os.path.isfile(filepath):
                self._send_headers(404, "text/plain")
                self.wfile.write(b"Not found")
                return
            # Determine content type from extension
            ct = "application/javascript"
            if filepath.endswith(".css"):
                ct = "text/css"
            elif filepath.endswith(".js"):
                ct = "application/javascript"
            self._send_headers(200, ct)
            with open(filepath, "rb") as f:
                self.wfile.write(f.read())

        # ── 404 ───────────────────────────────────────────────────────────
        else:
            self._send_headers(404, "text/plain")
            self.wfile.write(b"Not found")


# ── Watcher Thread ────────────────────────────────────────────────────────────

def watcher_loop():
    """Background thread that polls all log files and pushes to SSE queues."""
    while True:
        for watcher in SSEHandler.watchers.values():
            watcher.poll()
        time.sleep(POLL_INTERVAL)


# ── Embedded Dashboard HTML ───────────────────────────────────────────────────

DASHBOARD_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>sandfleaOS — Serial Log Dashboard</title>
<style>
/* ── Reset & Variables ──────────────────────────────────────────────────── */
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

:root {
  --bg: #0d1117;
  --bg-card: #161b22;
  --border: #30363d;
  --text: #c9d1d9;
  --text-dim: #8b949e;
  --text-bright: #f0f6fc;
  --font-mono: 'Cascadia Code', 'Fira Code', 'JetBrains Mono', 'Consolas', 'Monaco', monospace;
  --font-sans: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif;
  --radius: 8px;
  --transition: 150ms ease;
}

html, body {
  height: 100%; background: var(--bg); color: var(--text);
  font-family: var(--font-sans); overflow: hidden;
}

/* ── Header ──────────────────────────────────────────────────────────────── */
.header {
  display: flex; align-items: center; justify-content: space-between;
  padding: 10px 20px; border-bottom: 1px solid var(--border);
  background: var(--bg-card); user-select: none; gap: 12px;
}
.header-left { display: flex; align-items: center; gap: 10px; }
.logo {
  font-weight: 700; font-size: 15px; color: var(--text-bright);
  letter-spacing: -0.3px;
}
.logo span { color: #f78166; }
.uptime { font-size: 11px; color: var(--text-dim); }

/* ── Tab Bar ─────────────────────────────────────────────────────────────── */
.tab-bar {
  display: flex; padding: 0 20px; background: var(--bg-card);
  border-bottom: 1px solid var(--border); gap: 2px;
}
.tab {
  padding: 10px 18px; font-size: 13px; font-weight: 500; cursor: pointer;
  border: none; background: transparent; color: var(--text-dim);
  border-bottom: 2px solid transparent; transition: all var(--transition);
  position: relative; font-family: var(--font-sans); letter-spacing: 0.2px;
  display: flex; align-items: center; gap: 7px;
}
.tab:hover { color: var(--text); background: rgba(255,255,255,0.03); }
.tab.active { color: var(--text-bright); }
.tab .dot {
  width: 6px; height: 6px; border-radius: 50%; display: inline-block;
  transition: background var(--transition), box-shadow var(--transition);
}
.tab .dot.live {
  box-shadow: 0 0 6px 2px var(--dot-color, #4ade80);
}
.tab .dot.dead {
  background: #6e7681 !important; box-shadow: none;
}

/* ── Toolbar ─────────────────────────────────────────────────────────────── */
.toolbar {
  display: flex; align-items: center; justify-content: space-between;
  padding: 6px 20px; background: var(--bg); border-bottom: 1px solid var(--border);
  font-size: 11px; color: var(--text-dim); gap: 10px;
}
.toolbar-left { display: flex; align-items: center; gap: 12px; }
.toolbar-right { display: flex; align-items: center; gap: 10px; }
.btn {
  padding: 4px 10px; font-size: 11px; border-radius: 5px;
  border: 1px solid var(--border); background: var(--bg-card);
  color: var(--text); cursor: pointer; transition: all var(--transition);
  font-family: var(--font-sans); display: flex; align-items: center; gap: 4px;
}
.btn:hover { border-color: #58a6ff; color: var(--text-bright); background: #1c2533; }
.btn:active { transform: scale(0.97); }
.line-count { font-variant-numeric: tabular-nums; }
.pause-indicator {
  padding: 2px 8px; border-radius: 10px; font-size: 10px; font-weight: 600;
  background: rgba(251, 191, 36, 0.15); color: #fbbf24; display: none;
}
.pause-indicator.visible { display: inline-block; }

/* ── Log View ───────────────────────────────────────────────────────────── */
.log-container {
  flex: 1; overflow: hidden; position: relative;
  height: calc(100vh - 110px);
}
.log-pane {
  display: none; height: 100%; overflow-y: auto; padding: 12px 0;
  font-family: var(--font-mono); font-size: 12.5px; line-height: 1.65;
  counter-reset: line;
}
.log-pane.active { display: block; }
.log-pane::-webkit-scrollbar { width: 8px; }
.log-pane::-webkit-scrollbar-track { background: transparent; }
.log-pane::-webkit-scrollbar-thumb {
  background: var(--border); border-radius: 4px;
}
.log-pane::-webkit-scrollbar-thumb:hover { background: #484f58; }

.log-line {
  display: flex; padding: 0 16px; transition: background 80ms ease;
  white-space: pre-wrap; word-break: break-all;
}
.log-line:hover { background: rgba(255,255,255,0.03); }
.line-num {
  flex-shrink: 0; width: 48px; text-align: right; padding-right: 14px;
  color: var(--text-dim); opacity: 0.55; font-size: 11px;
  user-select: none;
}
.line-text { flex: 1; }

/* ── Empty state ─────────────────────────────────────────────────────────── */
.empty-state {
  display: flex; flex-direction: column; align-items: center; justify-content: center;
  height: 100%; color: var(--text-dim); text-align: center; gap: 8px;
  font-size: 13px;
}
.empty-state .icon { font-size: 36px; opacity: 0.4; }
.empty-state .sub { font-size: 11px; opacity: 0.6; }

/* ── Toast ───────────────────────────────────────────────────────────────── */
.toast {
  position: fixed; bottom: 24px; right: 24px; padding: 10px 18px;
  border-radius: var(--radius); font-size: 12px; font-weight: 500;
  background: var(--bg-card); border: 1px solid var(--border);
  color: var(--text); z-index: 100; pointer-events: none;
  opacity: 0; transform: translateY(8px);
  transition: opacity 0.25s, transform 0.25s;
}
.toast.show { opacity: 1; transform: translateY(0); }

/* ── Responsive ──────────────────────────────────────────────────────────── */
@media (max-width: 600px) {
  .tab { padding: 8px 12px; font-size: 11px; }
  .toolbar { flex-wrap: wrap; }
  .log-container { height: calc(100vh - 130px); }
}

/* ── d3-flame-graph overrides ────────────────────────────────────────────── */
#flameChart {
  width: 100%; height: 100%; background: var(--bg);
}
/* Kill transition animations — we redraw on poll, no need for animation */
#flameChart g {
  transition: none !important;
}
#flameChart .d3-flame-graph-tip {
  background: rgba(22,27,34,0.95) !important;
  border: 1px solid var(--border) !important;
  border-radius: 6px !important;
  padding: 6px 10px !important;
  font-size: 11px !important;
  color: var(--text-bright) !important;
  font-family: var(--font-mono) !important;
  pointer-events: none;
  z-index: 100 !important;
}
#flameChart .d3-flame-graph-label {
  font-family: var(--font-mono);
  font-size: 10px;
  fill: #0d1117;
  pointer-events: none;
}
#flameChart rect {
  stroke: rgba(13,17,23,0.3);
  stroke-width: 0.5;
  transition: opacity 120ms ease;
}
#flameChart rect:hover {
  stroke: #58a6ff;
  stroke-width: 1.5;
  opacity: 0.9;
}
.flame-pane {
  display: none; height: 100%; overflow: hidden; position: relative;
}
.flame-pane.active { display: block; }
</style>
<link rel="stylesheet" href="/static/d3-flame-graph/dist/d3-flamegraph.css">
<script src="/static/d3/dist/d3.min.js"></script>
<script src="/static/d3-flame-graph/dist/d3-flamegraph.umd.min.js"></script>
</head>
<body>

<!-- ── Header ─────────────────────────────────────────────────────────────── -->
<div class="header">
  <div class="header-left">
    <div class="logo">sandflea<span>OS</span> &nbsp;Log Dashboard</div>
    <div class="uptime" id="uptime">up 0s</div>
  </div>
  <div style="display:flex;align-items:center;gap:8px;font-size:11px;color:var(--text-dim)">
    <span id="globalStatus" style="color:#f85149">●</span>
    <span id="globalStatusText">no connection</span>
  </div>
</div>

<!-- ── Tabs ───────────────────────────────────────────────────────────────── -->
<div class="tab-bar" id="tabBar"></div>

<!-- ── Toolbar ────────────────────────────────────────────────────────────── -->
<div class="toolbar">
  <div class="toolbar-left">
    <span class="line-count" id="lineCount">0 lines</span>
    <span class="pause-indicator" id="pauseIndicator">⏸ PAUSED</span>
  </div>
  <div class="toolbar-right">
    <button class="btn" onclick="clearActivePane()" title="Clear current tab">🗑 Clear</button>
    <button class="btn" onclick="togglePause()" id="pauseBtn" title="Pause/resume auto-scroll">⏯ Pause</button>
    <button class="btn" onclick="scrollToBottom()" title="Jump to bottom">⬇ Bottom</button>
  </div>
</div>

<!-- ── Log Panes ──────────────────────────────────────────────────────────── -->
<div class="log-container" id="logContainer"></div>

<!-- ── Toast ──────────────────────────────────────────────────────────────── -->
<div class="toast" id="toast"></div>

<script>
// ── State ───────────────────────────────────────────────────────────────────
const CHANNELS = {
  primary:  { label: "Primary (COM1)",  color: "#4ade80" },
  test:     { label: "Test (COM2)",     color: "#60a5fa" },
  profile:  { label: "Profile (COM3)",  color: "#f472b6" },
  flame:    { label: "🔥 Flame Graph",   color: "#fbbf24" },
};
let activeChannel = "primary";
let sources = {};        // { channel: EventSource }
let lineCounts = {};     // { channel: number }
let paused = false;
let pauseReason = null;   // 'auto' | 'manual' | null
let serverStart = Date.now();

// ── Build UI ─────────────────────────────────────────────────────────────────
function buildUI() {
  const tabBar = document.getElementById("tabBar");
  const logContainer = document.getElementById("logContainer");

  for (const [name, ch] of Object.entries(CHANNELS)) {
    // Tab
    const tab = document.createElement("button");
    tab.className = "tab" + (name === activeChannel ? " active" : "");
    tab.innerHTML = `<span class="dot" id="dot-${name}" style="background:${ch.color}"></span>${ch.label}`;
    tab.onclick = () => switchTab(name);
    tabBar.appendChild(tab);

    // Log pane
    const pane = document.createElement("div");
    pane.className = "log-pane" + (name === activeChannel ? " active" : "");
    pane.id = `pane-${name}`;
    pane.innerHTML = `<div class="empty-state">
      <div class="icon">📡</div>
      <div>Waiting for ${ch.label} data…</div>
      <div class="sub">Start QEMU (wr.bat) to populate this log</div>
    </div>`;
    if (name === "flame") {
      // d3-flame-graph — rendered into a dedicated div
      pane.className = "flame-pane" + (name === activeChannel ? " active" : "");
      pane.innerHTML = `<div id="flameChart"></div>`;
    } else {
      pane.addEventListener("scroll", onPaneScroll);
    }
    logContainer.appendChild(pane);

    lineCounts[name] = 0;
  }

  // Resize handler for flame graph
  window.addEventListener("resize", () => {
    if (activeChannel === "flame" && flameChart) flameChart.width(window.innerWidth - 40);
  });
}

// Don't open SSE connections for the flame tab
function connectChannelIfSSE(name) {
  if (name !== "flame") connectChannel(name);
}

// ── Tab Switching ────────────────────────────────────────────────────────────
function switchTab(name) {
  activeChannel = name;
  document.querySelectorAll(".tab").forEach(t => t.classList.remove("active"));
  document.querySelectorAll(".log-pane").forEach(p => p.classList.remove("active"));
  document.querySelectorAll(".flame-pane").forEach(p => p.classList.remove("active"));
  const tab = document.getElementById("dot-" + name).parentElement;
  tab.classList.add("active");
  document.getElementById("pane-" + name).classList.add("active");
  updateLineCount();
  if (name === "flame") {
    fetchFlameData();
  } else if (!paused) {
    scrollToBottom();
  }
}

// ── Clear ────────────────────────────────────────────────────────────────────
function clearActivePane() {
  if (activeChannel === "flame") return;  // d3 chart managed by renderFlameGraph
  const pane = document.getElementById("pane-" + activeChannel);
  pane.innerHTML = "";
  lineCounts[activeChannel] = 0;
  updateLineCount();
}

// ── Pause ────────────────────────────────────────────────────────────────────
function updatePauseUI() {
  document.getElementById("pauseIndicator").classList.toggle("visible", paused);
  document.getElementById("pauseBtn").textContent = paused ? "▶ Resume" : "⏯ Pause";
}

function togglePause() {
  paused = !paused;
  pauseReason = paused ? "manual" : null;
  updatePauseUI();
  if (!paused) {
    if (activeChannel === "flame") fetchFlameData();
    else scrollToBottom();
  }
}

// ── Scroll ───────────────────────────────────────────────────────────────────
function scrollToBottom() {
  if (paused) return;
  const pane = document.getElementById("pane-" + activeChannel);
  requestAnimationFrame(() => { pane.scrollTop = pane.scrollHeight; });
}

function onPaneScroll() {
  const pane = document.getElementById("pane-" + activeChannel);
  const atBottom = pane.scrollHeight - pane.scrollTop - pane.clientHeight < 50;

  if (!atBottom && !paused) {
    // User scrolled up — auto-pause
    paused = true;
    pauseReason = "auto";
    updatePauseUI();
  } else if (atBottom && paused && pauseReason === "auto") {
    // User scrolled back to bottom — auto-resume (only if auto-paused)
    paused = false;
    pauseReason = null;
    updatePauseUI();
  }
}

// ── Append Line ──────────────────────────────────────────────────────────────
function appendLine(channel, text) {
  const pane = document.getElementById("pane-" + channel);

  // Remove empty state on first line
  if (lineCounts[channel] === 0) {
    pane.innerHTML = "";
  }

  const line = document.createElement("div");
  line.className = "log-line";

  const num = document.createElement("span");
  num.className = "line-num";
  num.textContent = ++lineCounts[channel];

  const txt = document.createElement("span");
  txt.className = "line-text";
  txt.textContent = text;

  line.appendChild(num);
  line.appendChild(txt);
  pane.appendChild(line);

  if (channel === activeChannel) {
    updateLineCount();
    scrollToBottom();
  } else {
    // Flash the tab dot to indicate activity
    const dot = document.getElementById("dot-" + channel);
    dot.style.transform = "scale(1.6)";
    dot.style.opacity = "0.7";
    setTimeout(() => {
      dot.style.transform = "scale(1)";
      dot.style.opacity = "1";
    }, 200);
  }
}

function clearPane(channel) {
  const pane = document.getElementById("pane-" + channel);
  pane.innerHTML = `<div class="empty-state">
    <div class="icon">🔄</div>
    <div>Log truncated (QEMU restarted)</div>
    <div class="sub">New output will appear here</div>
  </div>`;
  lineCounts[channel] = 0;
  if (channel === activeChannel) updateLineCount();
}

// ── Update UI ────────────────────────────────────────────────────────────────
function updateLineCount() {
  document.getElementById("lineCount").textContent =
    lineCounts[activeChannel] + " line" + (lineCounts[activeChannel] !== 1 ? "s" : "");
}

// ── Connect SSE ──────────────────────────────────────────────────────────────
function connectChannel(name) {
  if (sources[name]) sources[name].close();

  const es = new EventSource("/stream/" + name);
  sources[name] = es;

  es.addEventListener("status", (e) => {
    setDot(name, true);
    updateGlobalStatus();
  });

  es.addEventListener("append", (e) => {
    appendLine(name, e.data);
  });

  es.addEventListener("clear", (e) => {
    clearPane(name);
  });

  es.addEventListener("error", () => {
    setDot(name, false);
    updateGlobalStatus();
    // EventSource auto-reconnects
  });

  es.onopen = () => {
    setDot(name, true);
    updateGlobalStatus();
  };
}

function setDot(name, live) {
  const dot = document.getElementById("dot-" + name);
  if (live) {
    dot.classList.add("live");
    dot.classList.remove("dead");
    dot.style.setProperty("--dot-color", CHANNELS[name].color);
    dot.style.background = CHANNELS[name].color;
  } else {
    dot.classList.remove("live");
    dot.classList.add("dead");
  }
}

function updateGlobalStatus() {
  let alive = 0;
  for (const name of Object.keys(CHANNELS)) {
    const dot = document.getElementById("dot-" + name);
    if (dot && dot.classList.contains("live")) alive++;
  }
  const el = document.getElementById("globalStatus");
  const text = document.getElementById("globalStatusText");
  if (alive >= Object.keys(CHANNELS).length) {
    el.style.color = "#4ade80"; text.textContent = "all connected";
  } else if (alive > 0) {
    el.style.color = "#fbbf24"; text.textContent = `${alive}/${Object.keys(CHANNELS).length} connected`;
  } else {
    el.style.color = "#f85149"; text.textContent = "no connection";
  }
}

// ── Uptime ────────────────────────────────────────────────────────────────────
// ── Flame Graph (d3-flame-graph) ───────────────────────────────────────────
let flameData = null;
let flameChart = null;

// Convert speedscope evented profile JSON → d3-flame-graph hierarchical format.
// d3-flame-graph expects: { name, value, children: [...] }
// where value is the total duration of that node in the same unit as children.
function speedscopeToTree(flameData) {
  const profile = flameData.profiles?.[0];
  if (!profile || !profile.events?.length) return null;

  const events = profile.events;

  // Build tree from O(pen)/C(lose) events using a stack.
  const stack = [];
  const roots = [];

  for (const ev of events) {
    if (ev.type === "O") {
      const node = { name: ev.frame, value: 0, children: [] };
      if (stack.length) {
        stack[stack.length - 1].children.push(node);
      } else {
        roots.push(node);
      }
      node._start = ev.at;
      stack.push(node);
    } else if (ev.type === "C") {
      // Pop the matching open. For interleaved events, find by name from top.
      for (let i = stack.length - 1; i >= 0; i--) {
        if (stack[i].name === ev.frame) {
          const node = stack.splice(i, 1)[0];
          node.value = ev.at - (node._start || ev.at);
          delete node._start;
          break;
        }
      }
    }
  }

  // Close any still-open nodes at the profile end.
  const endValue = profile.endValue || 0;
  for (const node of stack) {
    node.value = endValue - (node._start || endValue);
    delete node._start;
  }

  // Wrap in a single root so d3-flame-graph renders a unified flame graph.
  const totalVal = roots.reduce((s, r) => s + Math.max(r.value, 0), 0) || 1;
  return {
    name: "sandfleaOS profile",
    value: totalVal,
    children: roots,
  };
}

function fetchFlameData() {
  if (paused && activeChannel === "flame") return;
  fetch("/flame-data")
    .then(r => r.json())
    .then(data => {
      flameData = data;
      if (activeChannel === "flame") renderFlameGraph();
    })
    .catch(err => {
      console.error("flame-data fetch failed:", err);
      const el = document.getElementById("flameChart");
      if (el) el.innerHTML = `<div class="empty-state" style="position:absolute;inset:0">
        <div class="icon">⚠️</div>
        <div>Failed to load profile data</div>
        <div class="sub">${err.message || err}</div>
      </div>`;
    });
}

function renderFlameGraph() {
  try {
    const tree = speedscopeToTree(flameData);
    if (!tree || !tree.children || !tree.children.length) {
      const el = document.getElementById("flameChart");
      if (el) el.innerHTML = `<div class="empty-state" style="position:absolute;inset:0">
        <div class="icon">📊</div>
        <div>No profile data yet</div>
        <div class="sub">Add PROFILE_BEGIN/END pairs to your kernel code</div>
      </div>`;
      if (flameChart) { try { flameChart.destroy(); } catch(e) {} }
      flameChart = null;
      return;
    }

    if (typeof d3 === "undefined" || typeof flamegraph === "undefined") {
      const el = document.getElementById("flameChart");
      if (el) el.innerHTML = `<div class="empty-state" style="position:absolute;inset:0">
        <div class="icon">📦</div>
        <div>d3-flame-graph library not loaded</div>
        <div class="sub">d3=${typeof d3}, flamegraph=${typeof flamegraph}</div>
      </div>`;
      if (flameChart) { try { flameChart.destroy(); } catch(e) {} }
      flameChart = null;
      return;
    }

    // UMD exports {default: fn, tooltip: {...}} — use .default for the factory
    const flamegraphFactory = flamegraph.default || flamegraph;

    // Destroy old chart + tooltips before creating a new one.
    if (flameChart) {
      try { flameChart.destroy(); } catch(e) {}
      flameChart = null;
    }
    document.querySelectorAll(".d3-flame-graph-tip").forEach(el => el.remove());

    // Clear container of any previous content (empty-state HTML, old SVG)
    const containerEl = document.getElementById("flameChart");
    if (containerEl) containerEl.innerHTML = "";

    // Create fresh chart instance
    flameChart = flamegraphFactory()
      .width(window.innerWidth - 40)
      .cellHeight(20)
      .minFrameSize(1)
      .transitionDuration(0)
      .selfValue(false);

    // Color mapper
    flameChart.setColorMapper((d, originalColor) => {
      if (d.data && d.data.name === "sandfleaOS profile") return "#161b22";
      let h = 0;
      const n = (d.data && d.data.name) || (d.name) || "";
      for (let i = 0; i < n.length; i++) h = ((h << 5) - h + n.charCodeAt(i)) | 0;
      return `hsl(${Math.abs(h) % 360}, 55%, 50%)`;
    });

    // Tooltip
    if (flamegraph.tooltip) {
      flameChart.tooltip(
        flamegraph.tooltip.defaultFlamegraphTooltip()
          .text(d => {
            const name = (d.data && d.data.name) || (d.name) || "?";
            const val = (d.data && d.data.value) || (d.value) || 0;
            return `${name} — ${val.toLocaleString()} µs` +
              (val >= 1000 ? ` (${(val/1000).toFixed(1)}ms)` : '');
          })
      );
    }

    // Render into container
    d3.select("#flameChart").datum(tree).call(flameChart);
  } catch (err) {
    console.error("renderFlameGraph error:", err);
    const el = document.getElementById("flameChart");
    if (el) el.innerHTML = `<div class="empty-state" style="position:absolute;inset:0">
      <div class="icon">💥</div>
      <div>Rendering error</div>
      <div class="sub">${err.message || err}</div>
    </div>`;
    if (flameChart) { try { flameChart.destroy(); } catch(e) {} }
    flameChart = null;
  }
}

function updateUptime() {
  const sec = Math.floor((Date.now() - serverStart) / 1000);
  const m = Math.floor(sec / 60), s = sec % 60;
  document.getElementById("uptime").textContent =
    `up ${m}m ${s}s`;
}

// ── Init ─────────────────────────────────────────────────────────────────────
buildUI();
for (const name of Object.keys(CHANNELS)) {
  connectChannelIfSSE(name);
}
setInterval(updateUptime, 1000);
updateUptime();
// Poll flame graph data every 2 seconds
setInterval(fetchFlameData, 2000);
fetchFlameData();
</script>
</body>
</html>"""

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="sandfleaOS Log Dashboard")
    parser.add_argument("--port", type=int, default=8079, help="HTTP port (default: 8079)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Bind address")
    args = parser.parse_args()

    # Validate log file paths
    for name, ch in CHANNELS.items():
        if not os.path.exists(ch["file"]):
            print(f"[dashboard] Note: '{ch['file']}' not found — will wait for QEMU to create it")

    # Initialize watchers
    SSEHandler.watchers = {name: LogWatcher(ch["file"]) for name, ch in CHANNELS.items()}
    SSEHandler.server_start_time = time.time()

    # Start background watcher thread
    watcher_thread = threading.Thread(target=watcher_loop, daemon=True)
    watcher_thread.start()

    # Start HTTP server
    server = ThreadingHTTPServer((args.host, args.port), SSEHandler)
    server.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    print(f"\n  sandfleaOS Log Dashboard")
    print(f"  ────────────────────────")
    print(f"  → Open: http://{args.host}:{args.port}")
    print(f"  → Watching {len(CHANNELS)} channels:")
    for name, ch in CHANNELS.items():
        exists = "✓" if os.path.exists(ch["file"]) else "✗ (waiting)"
        print(f"      [{ch['label']}] {ch['file']} {exists}")
    print(f"  → Press Ctrl+C to stop\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[dashboard] Shutting down…")
        server.shutdown()


if __name__ == "__main__":
    main()
