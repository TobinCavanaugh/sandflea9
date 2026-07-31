#!/usr/bin/env python3
"""
sandfleaOS — Real-Time Serial Log Dashboard (server side)
=========================================================
Multi-channel SSE server. The HTML UI lives at dashboard/index.html
so it can be edited independently of this file.

Usage:
    python log_server.py                 # default: port 8079
    python log_server.py --port 9000     # custom port

Then open http://localhost:8079 in your browser.

This module also exports handle_log_routes(), setup_log_watchers(),
and start_watcher_thread() so harness.py can mount log routes on
the same port as the harness without merging the two source files.
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
INDEX_HTML_PATH = os.path.join(BASE_DIR, "index.html")

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


def _load_index_html():
    """Read the dashboard HTML once at startup from index.html.
    If the file is missing, return a small explanatory stub so the
    server still responds cleanly instead of crashing."""
    if not os.path.isfile(INDEX_HTML_PATH):
        return (
            "<!DOCTYPE html><html><head><title>sandfleaOS Log Dashboard"
            " — index.html missing</title></head>"
            "<body style=\"font-family:monospace;background:#0d1117;color:#c9d1d9;padding:24px\">"
            "<h1>dashboard/index.html missing</h1>"
            "<p>The dashboard server expected to find <code>dashboard/index.html</code> "
            "sibling to this script but did not. Restore it (or recompile from source).</p>"
            "</body></html>"
        )
    with open(INDEX_HTML_PATH, "r", encoding="utf-8") as f:
        return f.read()


INDEX_HTML = _load_index_html()
INDEX_HTML_BYTES = INDEX_HTML.encode("utf-8")


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


# ── Module-level shared state (populated by setup_log_watchers) ────────────────

_log_watchers = {}


# ── Exported helpers (used by both SSEHandler and harness.py) ─────────────────

def _send_static(handler, status=200, content_type="text/html", extra=None):
    """Send headers + end_headers on any BaseHTTPRequestHandler subclass."""
    handler.send_response(status)
    handler.send_header("Content-Type", content_type)
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
    if extra:
        for k, v in extra.items():
            handler.send_header(k, v)
    handler.end_headers()


def handle_log_routes(handler):
    """Try to handle the request as a log-dashboard route.

    Returns True if the route was handled (the response was written),
    False if the caller should fall through to harness routes or 404.

    Designed to be called from any BaseHTTPRequestHandler subclass
    (both SSEHandler and HarnessHandler)."""
    parsed = urlparse(handler.path)
    path = parsed.path.rstrip("/")

    # ── Static dashboard page ────────────────────────────────────────────
    if path == "" or path == "/":
        handler.send_response(200)
        handler.send_header("Content-Type", "text/html; charset=utf-8")
        handler.send_header("Content-Length", str(len(INDEX_HTML_BYTES)))
        handler.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        handler.end_headers()
        handler.wfile.write(INDEX_HTML_BYTES)
        return True

    # ── SSE stream endpoints ─────────────────────────────────────────────
    elif path.startswith("/stream/"):
        channel = path.split("/")[-1]
        if channel not in CHANNELS:
            _send_static(handler, 404, "text/plain")
            handler.wfile.write(b"Unknown channel")
            return True

        _send_static(handler, 200, "text/event-stream",
                     extra={"Connection": "keep-alive",
                            "X-Accel-Buffering": "no"})
        q = _log_watchers[channel].subscribe()
        try:
            # Send initial connection event
            handler.wfile.write(b"event: status\ndata: connected\n\n")
            handler.wfile.flush()

            while True:
                try:
                    event, data = q.get(timeout=1.0)
                    payload = data.replace("\n", "\ndata: ")
                    handler.wfile.write(
                        f"event: {event}\ndata: {payload}\n\n".encode("utf-8"))
                    handler.wfile.flush()
                except queue.Empty:
                    # Keepalive comment to prevent proxy timeouts
                    try:
                        handler.wfile.write(b": ping\n\n")
                        handler.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError,
                            ConnectionAbortedError, OSError):
                        return True
        except (BrokenPipeError, ConnectionResetError,
                ConnectionAbortedError):
            pass
        finally:
            _log_watchers[channel].unsubscribe(q)
        return True

    # ── Flame graph data API ─────────────────────────────────────────────
    elif path == "/flame-data":
        _send_static(handler, 200, "application/json",
                     extra={"Access-Control-Allow-Origin": "*"})
        ch = CHANNELS.get("profile", {})
        json_str = to_speedscope(ch.get("file", "profile.log"))
        handler.wfile.write(json_str.encode("utf-8"))
        return True

    # ── Status API ───────────────────────────────────────────────────────
    elif path == "/api/status":
        status = {}
        for name, ch in CHANNELS.items():
            exists = os.path.exists(ch["file"])
            size = os.path.getsize(ch["file"]) if exists else 0
            status[name] = {"exists": exists, "size": size}
        _send_static(handler, 200, "application/json")
        handler.wfile.write(json.dumps(status).encode("utf-8"))
        return True

    # ── Static files (node_modules) ──────────────────────────────────────
    elif path.startswith("/static/"):
        rel = path[len("/static/"):]
        if ".." in rel or rel.startswith("/"):
            _send_static(handler, 403, "text/plain")
            handler.wfile.write(b"Forbidden")
            return True
        filepath = os.path.join(BASE_DIR, "node_modules", rel)
        if not os.path.isfile(filepath):
            _send_static(handler, 404, "text/plain")
            handler.wfile.write(b"Not found")
            return True
        ct = "application/javascript"
        if filepath.endswith(".css"):
            ct = "text/css"
        elif filepath.endswith(".js"):
            ct = "application/javascript"
        _send_static(handler, 200, ct)
        with open(filepath, "rb") as f:
            handler.wfile.write(f.read())
        return True

    # Not a log route — let the caller handle it
    return False


def setup_log_watchers():
    """Create LogWatcher instances for all configured channels.
    Call once at startup before any SSE connections are established."""
    global _log_watchers
    _log_watchers = {name: LogWatcher(ch["file"]) for name, ch in CHANNELS.items()}


def _watcher_loop():
    """Background thread: poll all log files and push to SSE queues."""
    while True:
        for watcher in _log_watchers.values():
            watcher.poll()
        time.sleep(POLL_INTERVAL)


def start_watcher_thread():
    """Spawn a daemon thread that polls log files for the SSE streams.
    Returns the thread (caller doesn't need to do anything with it)."""
    t = threading.Thread(target=_watcher_loop, daemon=True)
    t.start()
    return t


# ── Standalone SSE Handler (backward-compatible) ──────────────────────────────

class SSEHandler(BaseHTTPRequestHandler):
    """Thin wrapper around handle_log_routes() for standalone use."""

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if not handle_log_routes(self):
            _send_static(self, 404, "text/plain")
            self.wfile.write(b"Not found")


# ── Main (standalone) ─────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="sandfleaOS Log Dashboard")
    parser.add_argument("--port", type=int, default=8079, help="HTTP port (default: 8079)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Bind address")
    args = parser.parse_args()

    # Validate log file paths
    for name, ch in CHANNELS.items():
        if not os.path.exists(ch["file"]):
            print(f"[dashboard] Note: '{ch['file']}' not found — will wait for QEMU to create it")

    if not INDEX_HTML or "<!DOCTYPE" not in INDEX_HTML[:64]:
        print("[dashboard] Note: index.html missing or invalid — fallback stub will be served.")

    # Initialize watchers
    setup_log_watchers()

    # Start background watcher thread
    start_watcher_thread()

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
