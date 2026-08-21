"""
dashboard/harness.py — sandfleaOS test harness.

Sibling server to log_server.py. Serves the harness UI on :8080 by
default, exposing a small set of routes that proxy to the running QEMU
process via QEMU's QMP protocol (JSON-RPC over TCP :4545).

Architecture: see media/writings/dashboard_harness_plan.md.

Run directly:
    python harness.py                 # default port 8080, qmp at localhost:4545
    python harness.py --port 9000     # custom port
    python harness.py --qmp-host 192.168.1.10 --qmp-port 4545

Open:
    http://localhost:8080/harness     # the harness UI
"""

import argparse
import asyncio
import io
import json
import os
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.parse import urlparse, parse_qs

from log_server import (
    handle_log_routes, setup_log_watchers, start_watcher_thread,
    CHANNELS as LOG_CHANNELS,
)

try:
    from PIL import Image
    _PIL_OK = True
except ImportError:
    Image = None
    _PIL_OK = False

BASE_DIR = os.path.dirname(os.path.abspath(__file__))


# ── HTTP Server ────────────────────────────────────────────────────────────────

class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def load_html(path):
    with open(os.path.join(BASE_DIR, path), "r", encoding="utf-8") as f:
        return f.read()


# ── QMP Client ────────────────────────────────────────────────────────────────
#
# Single TCP connection to QEMU's QMP port. We keep one connection shared
# across handler threads by serialising calls through a lock + a single
# asyncio event loop in a background thread.

class QmpClient:
    """Lightweight synchronous QMP wrapper around an asyncio TCP loop.

    Usage:
        q = QmpClient("127.0.0.1", 4545)
        q.start()
        try:
            result = q.execute("query-status")
            print(result)
        finally:
            q.stop()

    `execute()` is thread-safe; multiple HTTP handler threads can call it.
    """

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self._loop = None
        self._thread = None
        # _ready is set once we've completed the qmp_capabilities handshake.
        # Cleared on every disconnect/reconnect. Handlers gate on this so
        # they don't write to a closed writer mid-reconnect.
        self._ready = threading.Event()
        self._lock = threading.Lock()
        self._id_seq = 0
        # id → (Future, monotonic seconds when added). Tracking timestamps
        # lets us sweep stale pending requests that QEMU never replied to
        # (e.g. after a malformed line dropped on the floor).
        self._pending = {}
        # Diagnostics surfaced via /api/qmp/status:
        self._attempts = 0              # total connection attempts since start()
        self._last_error = None         # last exception (str) on a failed attempt
        self._stop_requested = False    # set by stop() so retry loop exits cleanly
        self._reader = None
        self._writer = None

    def is_ready(self):
        return self._ready.is_set()

    def start(self):
        """Kick off the background thread. Does NOT block on connection.

        The connection is attempted in a retry-with-exponential-backoff
        loop inside _connect_loop. Until a connection succeeds, is_ready()
        returns False and execute() raises immediately.
        """
        if self._thread is not None:
            return  # idempotent — already running
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        if self._loop is None:
            return
        self._stop_requested = True
        try:
            asyncio.run_coroutine_threadsafe(self._shutdown(), self._loop).result(timeout=2)
        except Exception:
            pass

    def execute(self, command, args=None, timeout=5):
        """Block until QEMU responds to one command. Raises if not ready."""
        if not self._ready.is_set():
            err = self._last_error or "QMP not yet connected"
            raise RuntimeError(f"QMP not ready (last error: {err})")
        fut = asyncio.run_coroutine_threadsafe(
            self._send(command, args or {}), self._loop
        )
        return fut.result(timeout=timeout)

    # ── private ─────────────────────────────────────────────────────────

    def _run(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._connect_loop())
        finally:
            self._loop.close()

    async def _connect_loop(self):
        """Retry-with-backoff wrapper around _read_loop.

        Each iteration tries to connect, complete the qmp_capabilities
        handshake, then dispatch into _read_loop until QEMU closes the
        socket. On disconnect we clear _ready, reap any in-flight
        pending commands with ConnectionError, and back off before
        the next attempt. Exits when stop() sets _stop_requested.
        """
        backoff = 1.0
        while not self._stop_requested:
            self._attempts += 1
            err = None
            try:
                reader, writer = await asyncio.open_connection(self.host, self.port)
                try:
                    self._reader, self._writer = reader, writer
                    # Drain QEMU's greeting line (`{"QMP": {…}}`).
                    greeting = await reader.readline()
                    if not greeting:
                        raise RuntimeError("QEMU closed before greeting")

                    # qmp_capabilities handshake. Inline rather than via
                    # _send() so we don't trip the _ready precondition
                    # (we only set _ready once handshake succeeds).
                    msg = json.dumps(
                        {"execute": "qmp_capabilities", "id": 0}
                    ).encode("utf-8") + b"\n"
                    writer.write(msg)
                    await writer.drain()
                    ack = await reader.readline()
                    if not ack:
                        raise RuntimeError("QEMU closed during handshake")
                    ack_msg = json.loads(ack.decode("utf-8"))
                    if "error" in ack_msg:
                        raise RuntimeError(
                            f"QMP capabilities error: {ack_msg['error']}"
                        )

                    # Fresh connection → reset id sequence so stale
                    # numbers never collide with QEMU's clean slate.
                    self._id_seq = 0
                    self._ready.set()
                    backoff = 1.0  # reset after successful handshake

                    # Drain events until QEMU closes the socket.
                    await self._read_loop()
                    err = "connection closed by QEMU"
                finally:
                    # Tear down this connection's state regardless of
                    # how we exited (exception, EOF, or stop()).
                    self._ready.clear()
                    if self._writer is not None:
                        try:
                            self._writer.close()
                        except Exception:
                            pass
                    self._writer = None
                    self._reader = None
                    for fut, _ in list(self._pending.values()):
                        if not fut.done():
                            fut.set_exception(
                                ConnectionError("QMP connection dropped")
                            )
                    self._pending.clear()
            except (ConnectionRefusedError, OSError) as e:
                err = str(e) or type(e).__name__
            except Exception as e:
                err = f"{type(e).__name__}: {e}"
            self._last_error = err
            if self._stop_requested:
                return
            # Exponential backoff capped at 5s — quick recovery when
            # QEMU re-launches, but doesn't flood the log if it just
            # isn't there yet.
            backoff = min(backoff * 1.5, 5.0)
            await asyncio.sleep(backoff)

    async def _read_loop(self):
        """Read QEMU responses and dispatch matching pending futures.

        Exits (returns) on EOF or read error so the caller can
        tear down the connection and our outer retry loop can
        reconnect.
        """
        while True:
            try:
                raw = await self._reader.readline()
            except Exception:
                return
            if not raw:
                return

            # Sweep entries older than 10s before dispatching — HTTP
            # wrapper already times out at 5s, so anything past 10s is
            # guaranteed stale (QEMU ate the message or never matched
            # an id). Snapshot items so a callback that .pop()s the
            # same entry during this loop doesn't blow up our dict.
            now = time.monotonic()
            stale = [
                (iid, fut) for iid, (fut, ts) in list(self._pending.items())
                if not fut.done() and (now - ts) > 10.0
            ]
            for iid, fut in stale:
                entry = self._pending.pop(iid, None)
                if entry is None:
                    continue
                fut.set_exception(TimeoutError(
                    "QMP request %d timed out after 10s" % iid
                ))

            try:
                msg = json.loads(raw.decode("utf-8"))
            except Exception:
                continue
            iid = msg.get("id")
            if iid is not None and iid in self._pending:
                fut, _ = self._pending.pop(iid)
                if not fut.done():
                    fut.get_loop().call_soon_threadsafe(fut.set_result, msg)
            else:
                # Server-initiated event (e.g. DEVICE_DELETED). Dropped
                # for now; future versions can update state.devices from
                # events so state stays in sync with QEMU's truth.
                ev = msg.get("event")
                if ev:
                    pass  # placeholder for future event listeners

    async def _send(self, command, args):
        # Race guard: execute() reads _ready.is_set() before dispatching,
        # but by the time this coroutine actually runs the loop may have
        # completed a reconnect's inner finally and nulled _writer. Verify
        # again here so we AttributeError-fail fast instead of crashing
        # the whole loop on a half-closed socket. We capture the writer
        # into a local so an in-flight nulling doesn't blow this send up
        # after the guard passes.
        if not self._ready.is_set():
            raise ConnectionError("QMP connection dropped before send")
        writer = self._writer
        if writer is None:
            raise ConnectionError("QMP writer is None before send")

        self._id_seq += 1
        iid = self._id_seq
        payload = {"execute": command, "arguments": args, "id": iid}
        line = (json.dumps(payload) + "\n").encode("utf-8")
        fut = asyncio.Future()
        self._pending[iid] = (fut, time.monotonic())
        # Defence in depth: if QEMU side closed the socket between the
        # guard above and the write below (CLOSE_WAIT, RST, half-close),
        # write/drain surfaces as ConnectionResetError /
        # BrokenPipeError / IncompleteReadError — all subclasses of
        # OSError. We catch OSError broadly so the same path handles a
        # half-close (IncompleteReadError) and a full close
        # (ConnectionResetError). Convert to ConnectionError and pop
        # our id from _pending so the 10s stale-sweep doesn't later
        # misreport this as a TimeoutError.
        try:
            writer.write(line)
            await writer.drain()
        except OSError as e:
            self._pending.pop(iid, None)
            raise ConnectionError(f"QMP write failed: {e}") from e
        return await fut

    async def _shutdown(self):
        # Cancel every in-flight request so HTTP handlers awaiting
        # `.result(timeout=…)` wake up promptly with an explicit error
        # instead of waiting out the full timeout window.
        for fut, _ in list(self._pending.values()):
            if not fut.done():
                fut.set_exception(RuntimeError("QMP client stopping"))
        self._pending.clear()
        # Close the TCP connection (best-effort; don't await drain).
        try:
            self._writer.close()
        except Exception:
            pass
        self._loop.stop()


# ── Shared state ──────────────────────────────────────────────────────────────

class HarnessState:
    def __init__(self, qmp_host, qmp_port):
        self.qmp_host = qmp_host
        self.qmp_port = qmp_port
        self.qmp = None
        self.qmp_error = None
        self.device_counter = 0   # for naming id= when device_add
        self.devices = {}          # logical_id → {"driver": ..., "qemu_id": ...}

    def connect(self):
        """Lazily build + start the QmpClient on first call.
        Idempotent: subsequent calls return immediately. The QmpClient
        connects asynchronously inside its own thread; handlers and
        the status endpoint observe state via is_ready() / _attempts
        / _last_error rather than blocking here.
        """
        if self.qmp is None:
            self.qmp = QmpClient(self.qmp_host, self.qmp_port)
            self.qmp.start()
        self.qmp_error = None  # legacy field; real status lives on qmp

    # Common JSON shape across the 200 status endpoint and the 503
    # "not ready" path. Same keys, same meaning — the dashboard JS can
    # render one widget, not two. `extra` (dict) is merged on top so
    # the 503 callers can add `error` + `retry_hint_s` without losing
    # the base fields.
    def status_payload(self, extra=None):
        if self.qmp is None:
            base = {
                "state": "starting",
                "connected": False,
                "attempts": 0,
                "last_error": None,
                "devices": dict(self.devices),
                "qmp_host": self.qmp_host,
                "qmp_port": self.qmp_port,
            }
        else:
            qmp = self.qmp
            base = {
                "state": "connected" if qmp.is_ready() else "waiting",
                "connected": qmp.is_ready(),
                "attempts": qmp._attempts,
                "last_error": qmp._last_error,
                "devices": dict(self.devices),
                "qmp_host": qmp.host,
                "qmp_port": qmp.port,
            }
        if self._is_refused_connection(base.get("last_error")):
            base["setup_hint"] = (
                "QEMU is missing the QMP flag. Run wr-harness.bat "
                "(not wr.bat) so QEMU binds -qmp tcp:127.0.0.1:4545,server,nowait."
            )
        if extra:
            base.update(extra)
        return base

    @staticmethod
    def _is_refused_connection(err):
        """True when the last_error string plausibly indicates QEMU
        isn't listening on the QMP port — i.e., the user is on
        wr.bat instead of wr-harness.bat, or QEMU isn't running at
        all. We match across Windows (WinError 1225), Linux
        (Connection refused / errno 111), and macOS variants."""
        if not err:
            return False
        s = str(err).lower()
        return (
            "refused" in s
            or "winerror 1225" in s
            or "errno 111" in s
            or "econnrefused" in s
        )


# ── Request Handler ───────────────────────────────────────────────────────────

class HarnessHandler(BaseHTTPRequestHandler):
    state = None       # set by main()

    def log_message(self, fmt, *args):
        pass  # silence the noise

    def _write(self, status, content_type, body, extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        if extra_headers:
            # Sent before Content-Length so all metadata lines up
            # before end_headers().
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _write_json(self, status, obj, extra_headers=None):
        body = json.dumps(obj).encode("utf-8")
        self._write(status, "application/json", body, extra_headers=extra_headers)

    def _write_503(self, payload, retry_after_s=2):
        """503 Service Unavailable + Retry-After header for transient
        QMP-drops. The handlers below map ConnectionError here so the
        dashboard can poll on the Retry-After Schedule."""
        self._write_json(503, payload,
            extra_headers={"Retry-After": str(retry_after_s)})

    def _read_body(self):
        n = int(self.headers.get("Content-Length", "0") or "0")
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        # Try log-dashboard routes first (/, /stream/*, /flame-data, etc.)
        if handle_log_routes(self):
            return
        path = urlparse(self.path).path.rstrip("/")
        if path in ("", "/harness"):
            try:
                html = load_html("harness.html")
            except FileNotFoundError:
                self._write(500, "text/plain", b"harness.html missing on disk")
                return
            self._write(200, "text/html; charset=utf-8", html.encode("utf-8"))
        elif path == "/api/qmp/status":
            self._handle_status()
        elif path == "/api/qmp/screendump":
            self._handle_screendump()
        elif path == "/api/qmp/frame":
            self._handle_frame()
        else:
            self._write(404, "text/plain", b"Not found")

    def do_POST(self):
        path = urlparse(self.path).path.rstrip("/")
        try:
            body = self._read_body()
            data = json.loads(body.decode("utf-8")) if body else {}
        except Exception as e:
            self._write_json(400, {"error": f"bad json: {e}"})
            return

        if path == "/api/qmp/sendkey":
            self._handle_sendkey(data)
        elif path == "/api/qmp/device":
            self._handle_device(data)
        else:
            self._write_json(404, {"error": "unknown endpoint"})

    # ── handlers ───────────────────────────────────────────────────────

    def _ensure_qmp(self):
        """Return the QmpClient if state.connect() has created one,
        else None. Does NOT gate on is_ready() so the status handler
        can introspect the in-progress reconnect state."""
        self.state.connect()
        return self.state.qmp

    def _ensure_qmp_ready(self):
        """Returns qmp iff handshake has completed (is_ready()). Else
        writes 503 with Retry-After: 2 and the unified status payload
        merged with an `error`/`retry_hint_s` overlay, returns None.
        Use this from any handler whose job is to actually run a QMP
        command; the status handler uses _ensure_qmp() instead so it
        stays informative even when QMP is bouncing."""
        self.state.connect()
        qmp = self.state.qmp
        if qmp is None or not qmp.is_ready():
            self._write_503(self.state.status_payload(extra={
                "error": "qmp not ready",
                "retry_hint_s": 2,
            }))
            return None
        return qmp

    def _handle_status(self):
        """Always returns 200 with the unified status payload so the
        dashboard can render 'starting / waiting / connected / lost'
        from one shape. When QMP reports ready, we probe via
        query-status to distinguish 'connected' (recent reply) from
        'lost' (just dropped). The single _write_json path makes the
        contract easy to test."""
        self.state.connect()
        qmp = self.state.qmp
        base = self.state.status_payload()
        if qmp is None or not qmp.is_ready():
            # starting or waiting — emit base as-is
            self._write_json(200, base)
            return
        # qmp.is_ready() is True. Probe to detect lost state.
        try:
            _ = qmp.execute("query-status")
            self._write_json(200, base)
        except Exception as e:
            err = str(e)
            base["state"] = "lost"
            base["connected"] = False
            base["last_error"] = err
            if self.state._is_refused_connection(err):
                base["setup_hint"] = (
                    "QEMU is missing the QMP flag. Run wr-harness.bat "
                    "(not wr.bat) so QEMU binds -qmp tcp:127.0.0.1:4545,server,nowait."
                )
            self._write_json(200, base)

    def _handle_screendump(self):
        """Capture a PNG from QEMU and stream it back."""
        qmp = self._ensure_qmp_ready()
        if qmp is None:
            return  # helper already wrote 503
        if not _PIL_OK:
            self._write_json(500, {
                "error": "Pillow not installed; run `pip install Pillow` to enable screendumps"
            })
            return
        tmp_path = os.path.abspath(os.path.join(BASE_DIR, "..", "sandbox_dashboard_dump.ppm"))
        try:
            qmp.execute("screendump", {"filename": tmp_path})
        except ConnectionError as e:
            # QMP dropped mid-screendump — transient, retry on the
            # harness's reconnect cadence (Retry-After: 2 ≈ 2 s).
            self._write_503({
                "error": "qmp connection dropped before screendump: %s" % e,
                "retry_hint_s": 2,
            })
            return
        except Exception as e:
            self._write_json(500, {"error": "screendump exec failed: %s" % e})
            return
        try:
            # QEMU writes a binary PPM (P6). Pillow's loader handles all
            # of: P6 magic, header comments (#...), arbitrary whitespace
            # between fields, and 16-bit maxval. We don't hand-roll the
            # parser because PPM edge cases (comment lines, mixed
            # delimiters) silently misalign by 1 byte and look like
            # random RGB.
            with Image.open(tmp_path) as img:
                img.load()  # force decode before deleting source file
                with io.BytesIO() as png_io:
                    img.save(png_io, format="PNG")
                    png_bytes = png_io.getvalue()
        except Exception as e:
            self._write_json(500, {"error": "PPM decode failed: %s" % e})
            return
        finally:
            try:
                os.remove(tmp_path)
            except OSError:
                pass
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(png_bytes)))
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.end_headers()
        self.wfile.write(png_bytes)

    def _handle_frame(self):
        """Same as /api/qmp/screendump but resized + JPEG for live
        streaming. Query parameters:
            max_w=N (default 1024; clamps to [64, 3840])
            fmt=jpeg|png (default jpeg, ~30 KB @ 1024w vs PNG ~250 KB)
        Returns image/jpeg or image/png bytes. The browser polls this
        on a ~30 ms loop and draws into a <canvas> via ImageBitmap.
        """
        qmp = self._ensure_qmp_ready()
        if qmp is None:
            return  # helper already wrote 503
        if not _PIL_OK:
            self._write_json(500, {
                "error": "Pillow required for /api/qmp/frame; install with `pip install Pillow`"
            })
            return
        from urllib.parse import parse_qs, urlparse
        params = parse_qs(urlparse(self.path).query)
        try:
            max_w = max(64, min(3840, int(params.get("max_w", ["1024"])[0])))
        except (TypeError, ValueError):
            max_w = 1024
        fmt = (params.get("fmt", ["jpeg"])[0] or "jpeg").lower()
        if fmt not in ("jpeg", "jpg", "png"):
            fmt = "jpeg"

        tmp_path = os.path.abspath(os.path.join(BASE_DIR, "..", "sandbox_dashboard_dump.ppm"))
        try:
            qmp.execute("screendump", {"filename": tmp_path})
        except ConnectionError as e:
            self._write_503({"error": str(e), "retry_hint_s": 2})
            return
        except Exception as e:
            self._write_json(500, {"error": "screendump exec failed: %s" % e})
            return
        try:
            with Image.open(tmp_path) as img:
                iw, ih = img.size
                if iw > max_w:
                    ratio = max_w / float(iw)
                    nw, nh = max_w, max(1, int(ih * ratio))
                    img = img.resize((nw, nh), Image.LANCZOS)
                buf = io.BytesIO()
                if fmt in ("jpeg", "jpg"):
                    img.convert("RGB").save(buf, format="JPEG",
                                            quality=80, optimize=True)
                    ct = "image/jpeg"
                else:
                    img.save(buf, format="PNG", optimize=True)
                    ct = "image/png"
                body = buf.getvalue()
        except Exception as e:
            self._write_json(500, {"error": "frame encode failed: %s" % e})
            return
        finally:
            try:
                os.remove(tmp_path)
            except OSError:
                pass

        self.send_response(200)
        self.send_header("Content-Type", ct)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        # Strong no-cache so successive frames don't get cached.
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    def _handle_sendkey(self, data):
        qmp = self._ensure_qmp_ready()
        if qmp is None:
            return  # helper already wrote 503

        # Helper: parse an arbitrarily-typed input as non-negative int
        # with safe fallbacks for string "50ms", None, floats, etc.
        def _safe_nonneg_int(value, default):
            try:
                return max(0, int(value))
            except (TypeError, ValueError):
                return default

        # Three request shapes accepted by POST:
        #   combo: list of qcodes held simultaneously (single send-key call)
        #   keys:  list of entries, one send-key call per entry
        #   text:  raw string, one send-key call per character
        combo = data.get("combo")
        keys = data.get("keys")
        if keys is None and "text" in data:
            keys = [_qemu_keyname(c) for c in data["text"]]

        hold_ms   = _safe_nonneg_int(data.get("hold_ms"), 50)
        gap_ms    = _safe_nonneg_int(data.get("gap_ms"), 10)
        wait_flag = bool(data.get("wait", False))

        results = []
        try:
            # Combo path: a single QMP send-key with all listed qcodes
            # pressed simultaneously, released after hold_ms.
            if isinstance(combo, list) and len(combo) > 0:
                key_array = [{"type": "qcode", "data": _qemu_qcode(q)} for q in combo]
                args = {"keys": key_array, "hold-time": hold_ms}
                if wait_flag:
                    args["wait"] = True
                res = qmp.execute("send-key", args)
                results.append(res)
                self._write_json(200, {
                    "ok":            True,
                    "results_count": 1,
                    "combo":         combo,
                    "hold_ms":       hold_ms,
                })
                return

            if not keys:
                self._write_json(400, {"error": "no keys/text/combo"})
                return

            # Per-entry send-key path (keys[] or text[]).
            for i, k in enumerate(keys):
                if "-" in k:
                    combos = [c.strip() for c in k.split("-")]
                    key_array = [
                        {"type": "qcode", "data": _qemu_qcode(c)} for c in combos
                    ]
                else:
                    key_array = [{"type": "qcode", "data": _qemu_qcode(k)}]
                args = {"keys": key_array, "hold-time": hold_ms}
                if wait_flag:
                    args["wait"] = True
                res = qmp.execute("send-key", args)
                results.append(res)
                if i + 1 < len(keys) and gap_ms > 0:
                    time.sleep(gap_ms / 1000.0)

            self._write_json(200, {
                "ok":            True,
                "results_count": len(results),
                "sent":          keys,
                "hold_ms":       hold_ms,
                "gap_ms":        gap_ms,
                "wait":          wait_flag,
            })
        except ConnectionError as e:
            self._write_503({
                "error":         str(e),
                "partial_count": len(results),
                "retry_hint_s":  2,
            })
        except Exception as e:
            self._write_json(500, {
                "error":         str(e),
                "partial_count": len(results),
            })

    def _handle_device(self, data):
        qmp = self._ensure_qmp_ready()
        if qmp is None:
            return  # helper already wrote 503

        action = data.get("action")     # "add" or "del"
        driver = data.get("driver")     # e.g. "usb-kbd"
        bus = data.get("bus", "xhci.0")
        qemu_id = data.get("id")        # for "del"

        if action == "add":
            if not driver:
                self._write_json(400, {"error": "missing driver"})
                return
            self.state.device_counter += 1
            full_id = f"{driver}-{self.state.device_counter}"
            # Pop any prior entry with this id (defensive) so a
            # successful add leaves an unambiguous single record.
            prev = self.state.devices.pop(full_id, None)
            try:
                res = qmp.execute("device_add", {"driver": driver, "bus": bus, "id": full_id})
                self.state.devices[full_id] = {"driver": driver, "bus": bus}
                self._write_json(200, {"ok": True, "id": full_id, "qemu_response": res})
            except ConnectionError as e:
                if prev is not None:
                    self.state.devices[full_id] = prev
                self._write_503({
                    "error": str(e),
                    "retry_hint_s": 2,
                })
            except Exception as e:
                # On failure, restore the entry we removed (if any) so
                # state.devices reflects QEMU's truth, not our intent.
                if prev is not None:
                    self.state.devices[full_id] = prev
                self._write_json(500, {"error": str(e)})
        elif action == "del":
            if not qemu_id:
                # If id not specified, try to find the most-recent matching driver
                for cand in list(self.state.devices.keys())[::-1]:
                    if self.state.devices[cand].get("driver") == driver:
                        qemu_id = cand
                        break
            if not qemu_id:
                self._write_json(400, {"error": "no id match"})
                return
            prev = self.state.devices.pop(qemu_id, None)
            try:
                res = qmp.execute("device_del", {"id": qemu_id})
                # pop already removed; success is final.
                self._write_json(200, {"ok": True, "id": qemu_id, "qemu_response": res})
            except ConnectionError as e:
                # Roll back: re-attach exactly the entry we removed, or
                # leave it removed if it was newly added (prev was None).
                if prev is not None:
                    self.state.devices[qemu_id] = prev
                self._write_503({
                    "error": str(e),
                    "retry_hint_s": 2,
                })
            except Exception as e:
                # Roll back: re-attach exactly the entry we removed, or
                # leave it removed if it was newly added (prev was None).
                if prev is not None:
                    self.state.devices[qemu_id] = prev
                self._write_json(500, {"error": str(e)})
        else:
            self._write_json(400, {"error": "action must be add or del"})


# ── QEMU key name → QCode translation ─────────────────────────────────────────
#
# QEMU expects QCode identifiers like "a", "caps_lock", "shift", "ctrl". For
# combos, use the dash syntax in the request; we'll map each piece here.
#
# The map below is intentionally minimal — we only model printable ASCII
# plus a handful of named keys. Add more as the harness's demands grow.

_QCODE_MAP = {
    # printable ASCII
    " ": "spc",
    "\n": "ret",
    "\t": "tab",
    "\b": "backspace",
    # modifiers
    "ctrl": "ctrl", "shift": "shift", "alt": "alt", "meta": "meta",
    "caps": "caps_lock", "esc": "esc",
    # arrows
    "up": "up", "down": "down", "left": "left", "right": "right",
    # nav
    "home": "home", "end": "end", "pgup": "pgup", "pgdn": "pgdn",
    "insert": "insert", "del": "delete",
    # function keys
    "f1": "f1", "f2": "f2", "f3": "f3", "f4": "f4",
    "f5": "f5", "f6": "f6", "f7": "f7", "f8": "f8",
    "f9": "f9", "f10": "f10", "f11": "f11", "f12": "f12",
    # pass-through for already-in-QCode form
    "ctrl-c": "ctrl-c",  # never hit since we split on '-'
}


def _qemu_qcode(name):
    n = name.lower().strip()
    if len(n) == 1 and n.isprintable():
        return n  # QEMU accepts single printable char as qcode
    if n in _QCODE_MAP:
        return _QCODE_MAP[n]
    if n.startswith("f") and n[1:].isdigit():
        return n
    return n  # best-effort pass-through


def _qemu_keyname(ch):
    """Map a single printable character or \n to a QEMU sendkey name."""
    if ch == "\n":
        return "ret"
    if ch == "\t":
        return "tab"
    if ch == "\b":
        return "backspace"
    if ch == " ":
        return "spc"
    return ch.lower()


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="sandfleaOS Test Harness")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Bind address")
    parser.add_argument("--qmp-host", type=str, default="127.0.0.1", help="QEMU QMP host")
    parser.add_argument("--qmp-port", type=int, default=4545, help="QEMU QMP port")
    args = parser.parse_args()

    HarnessHandler.state = HarnessState(args.qmp_host, args.qmp_port)
    HarnessHandler.state.connect()
    qmp = HarnessHandler.state.qmp

    # Initialize log watchers so SSE streams + /flame-data work
    setup_log_watchers()
    start_watcher_thread()

    server = ThreadingHTTPServer((args.host, args.port), HarnessHandler)
    print(f"\n  sandfleaOS Dashboard + Test Harness")
    print(f"  ────────────────────────────────────")
    print(f"  → Log dashboard: http://{args.host}:{args.port}")
    print(f"  → Harness:       http://{args.host}:{args.port}/harness")
    print(f"  → Watching {len(LOG_CHANNELS)} log channels")
    if qmp is not None and qmp.is_ready():
        print(f"  → QMP @ {args.qmp_host}:{args.qmp_port}: connected "
              f"(attempt #{qmp._attempts})")
    else:
        print(f"  → QMP @ {args.qmp_host}:{args.qmp_port}: waiting "
              f"(retrying every 1–5 s)")
        print(f"    Start QEMU with `-qmp tcp:{args.qmp_host}:{args.qmp_port}"
              f",server,nowait` to connect.")
    print(f"  → Press Ctrl+C to stop\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[harness] Shutting down…")
        server.shutdown()
        if HarnessHandler.state.qmp:
            HarnessHandler.state.qmp.stop()


if __name__ == "__main__":
    main()
