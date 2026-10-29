#!/usr/bin/env python3
"""Zero-dependency WebSocket signaling server for the ubuild WebRTC pusher.

Model (master / callee = device; viewer / caller = browser or phone):

    ws://<host>:<port>/ws?room=<name>&role=caller|callee

JSON text frames:
    server -> client  {"type":"joined","room":"...","role":"...","peer_id":"..."}
                       (peer_id only for callers)
    caller -> server  {"type":"offer","sdp":"..."}
                       (forwarded to the room callee with peer_id injected)
    callee -> server  {"type":"answer","sdp":"...","peer_id":"..."}
                       (routed to the caller that owns peer_id)
    caller -> server  {"type":"candidate","candidate":"...","sdpMid":"...",
                       "sdpMLineIndex":N}
                       (forwarded to the room callee with peer_id injected)
    callee -> server  {"type":"candidate","candidate":"...","sdpMid":"...",
                       "sdpMLineIndex":N,"peer_id":"..."}
                       (routed to the caller that owns peer_id)
    any    -> server  {"type":"list"}
    server -> client  {"type":"callers","rooms":[{"room":"room1"}, ...]}
    server -> callee  {"type":"bye","peer_id":"..."} when a viewer leaves
    server -> caller  {"type":"bye"} when the device leaves

Plain HTTP on the same port:
    GET /signal/callers   JSON snapshot of rooms with a live device (callee)
    GET /  /viewer.html  /js/viewer.html   bundled viewer page
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import socket
import struct
import threading
import time
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PING_INTERVAL_S = 25.0
# How long a caller's offer is held waiting for the device (callee) to come
# online.  If no callee joins within this window the caller gets an
# offer_timeout message.
OFFER_WAIT_S = 20.0
WEB_ROOT = Path(__file__).resolve().parent / "web"


# --------------------------------------------------------------------------
# Room registry
# --------------------------------------------------------------------------
class Conn:
    __slots__ = ("sock", "send_lock", "alive", "peer_id",
                 "pending_sdp", "pending_deadline")

    def __init__(self, sock) -> None:
        self.sock = sock
        self.send_lock = threading.Lock()
        self.alive = True
        self.peer_id: str | None = None
        # Offer waiting for a callee to join this room, plus its absolute
        # expiry time (time.time()).  None when nothing is pending.
        self.pending_sdp: str | None = None
        self.pending_deadline: float = 0.0


class Room:
    def __init__(self) -> None:
        self.callee: Conn | None = None  # the device (master), single per room
        self.callers: dict[str, Conn] = {}  # peer_id -> viewer connection


STATE_LOCK = threading.Lock()
ROOMS: dict[str, Room] = {}


def room_of(name: str) -> Room:
    return ROOMS.setdefault(name, Room())


def send_json(conn: Conn, obj) -> bool:
    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    return send_frame(conn, 0x1, payload)


def send_frame(conn: Conn, opcode: int, payload: bytes) -> bool:
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length < 65536:
        header.append(126)
        header += struct.pack("!H", length)
    else:
        header.append(127)
        header += struct.pack("!Q", length)
    try:
        with conn.send_lock:
            conn.sock.sendall(bytes(header) + payload)
        return True
    except OSError:
        conn.alive = False
        return False


def drop_conn(room_name: str, role: str, conn: Conn) -> None:
    """Notify the other side that this connection left."""
    bye_targets: list[Conn] = []
    bye_obj: dict = {"type": "bye"}
    with STATE_LOCK:
        room = ROOMS.get(room_name)
        if not room:
            return
        if role == "callee" and room.callee is conn:
            # Device left: tell every viewer.
            room.callee = None
            bye_targets = list(room.callers.values())
        elif role == "caller" and conn.peer_id and conn.peer_id in room.callers:
            # A viewer left: tell the device which peer_id is gone.
            del room.callers[conn.peer_id]
            bye_obj["peer_id"] = conn.peer_id
            if room.callee is not None:
                bye_targets = [room.callee]
    for t in bye_targets:
        send_json(t, bye_obj)


# --------------------------------------------------------------------------
# WebSocket framing
# --------------------------------------------------------------------------
def recv_exact(sock, n: int) -> bytes | None:
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def ws_read_frame(sock) -> tuple[int, bytes] | None:
    """Returns (opcode, payload); opcode 0x8 means closed."""
    head = recv_exact(sock, 2)
    if head is None:
        return None
    b1, b2 = head[0], head[1]
    masked = bool(b2 & 0x80)
    length = b2 & 0x7F
    if length == 126:
        ext = recv_exact(sock, 2)
        if ext is None:
            return None
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = recv_exact(sock, 8)
        if ext is None:
            return None
        length = struct.unpack("!Q", ext)[0]
    mask = recv_exact(sock, 4) if masked else b""
    if masked and mask is None:
        return None
    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return b1 & 0x0F, payload


def ws_handshake(handler, key: str) -> bool:
    accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    ).decode("ascii")
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    try:
        handler.connection.sendall(response.encode("ascii"))
        return True
    except OSError:
        return False


# --------------------------------------------------------------------------
# Per-connection worker
# --------------------------------------------------------------------------
def run_ws(conn: Conn, room_name: str, role: str) -> None:
    sock = conn.sock
    # Timeout doubles as the keepalive ping cadence.
    sock.settimeout(PING_INTERVAL_S)

    with STATE_LOCK:
        room = room_of(room_name)
        stale_callee = None
        pending_deliveries: list[tuple[Conn, str]] = []
        if role == "callee":
            # Device (master). Kick any stale device from a previous run.
            stale_callee = room.callee
            room.callee = conn
            room.callers.pop(conn, None)
            print(f"[WS] callee connected, room={room_name}, "
                  f"pending_callers={len(room.callers)}")
            # Flush any offers that callers sent while the device was
            # offline.  Collect them under the lock, send outside.
            for caller in list(room.callers.values()):
                if caller.pending_sdp is not None:
                    pending_deliveries.append((caller, caller.pending_sdp))
                    caller.pending_sdp = None
                    caller.pending_deadline = 0.0
        else:
            # Viewer (caller). Assign a unique peer_id for answer routing.
            conn.peer_id = uuid.uuid4().hex[:12]
            room.callers[conn.peer_id] = conn

    # A device restart replaces the previous callee; kick the stale socket so
    # its thread exits.
    if stale_callee is not None and stale_callee is not conn:
        stale_callee.alive = False
        try:
            stale_callee.sock.close()
        except OSError:
            pass

    joined_obj = {"type": "joined", "room": room_name, "role": role}
    if role == "caller":
        joined_obj["peer_id"] = conn.peer_id
    if not send_json(conn, joined_obj):
        drop_conn(room_name, role, conn)
        return

    # Deliver held offers to the newly-arrived device.  IMPORTANT: this must
    # happen AFTER the "joined" frame, otherwise the device ignores the offer
    # (it discards every message until it sees "joined").
    for caller, sdp in pending_deliveries:
        send_json(conn, {"type": "offer", "sdp": sdp,
                         "peer_id": caller.peer_id})
    if pending_deliveries:
        print(f"[WS] callee joined, flushed {len(pending_deliveries)} "
              f"held offer(s)")

    missed_pings = 0
    while True:
        try:
            frame = ws_read_frame(sock)
        except socket.timeout:
            # Idle: keepalive ping; drop clients that miss two in a row.
            missed_pings += 1
            if missed_pings > 2 or not send_frame(conn, 0x9, b"hb"):
                break
            continue
        except OSError:
            frame = None
        if frame is None:
            break
        missed_pings = 0
        opcode, payload = frame
        if opcode == 0x8:  # close
            send_frame(conn, 0x8, payload[:2] + b"")
            break
        if opcode == 0x9:  # ping -> pong
            if not send_frame(conn, 0xA, payload):
                break
            continue
        if opcode == 0xA:  # pong
            continue
        if opcode != 0x1:
            continue
        try:
            msg = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        mtype = msg.get("type")

        if mtype == "list":
            with STATE_LOCK:
                callers = sorted(
                    name for name, r in ROOMS.items() if r.callee is not None
                )
            send_json(conn, {"type": "callers",
                             "rooms": [{"room": n} for n in callers]})

        elif mtype == "offer" and role == "caller":
            # Viewer sends an offer; forward to the device with peer_id.
            sdp = msg.get("sdp")
            if not isinstance(sdp, str):
                continue
            with STATE_LOCK:
                callee = room_of(room_name).callee
            if callee is not None:
                send_json(callee, {"type": "offer", "sdp": sdp,
                                   "peer_id": conn.peer_id})
                print(f"[WS] offer from caller {conn.peer_id} forwarded to callee")
            else:
                # No device online yet: hold the offer and wait up to
                # OFFER_WAIT_S for a callee to join.
                conn.pending_sdp = sdp
                conn.pending_deadline = time.time() + OFFER_WAIT_S
                print(f"[WS] offer from caller {conn.peer_id} held "
                      f"(no callee), waiting {OFFER_WAIT_S:.0f}s")

        elif mtype == "answer" and role == "callee":
            # Device answers; route to the viewer that owns peer_id.
            sdp = msg.get("sdp")
            peer_id = msg.get("peer_id")
            if not isinstance(sdp, str) or not isinstance(peer_id, str):
                continue
            with STATE_LOCK:
                target = room_of(room_name).callers.get(peer_id)
            if target is not None:
                send_json(target, {"type": "answer", "sdp": sdp,
                                   "peer_id": peer_id})

        elif mtype == "candidate":
            # Trickle ICE: forward ICE candidates between caller and callee.
            cand = msg.get("candidate")
            mid = msg.get("sdpMid", "")
            mline = msg.get("sdpMLineIndex", 0)
            if not isinstance(cand, str):
                continue
            if role == "caller":
                # Browser candidate -> forward to the device with peer_id.
                with STATE_LOCK:
                    callee = room_of(room_name).callee
                if callee is not None:
                    send_json(callee, {"type": "candidate", "candidate": cand,
                                       "sdpMid": mid, "sdpMLineIndex": mline,
                                       "peer_id": conn.peer_id})
            else:
                # Device candidate -> route to the caller that owns peer_id.
                peer_id = msg.get("peer_id")
                if not isinstance(peer_id, str):
                    continue
                with STATE_LOCK:
                    target = room_of(room_name).callers.get(peer_id)
                if target is not None:
                    send_json(target, {"type": "candidate", "candidate": cand,
                                       "sdpMid": mid, "sdpMLineIndex": mline})
        # unknown messages are ignored

    conn.alive = False
    try:
        sock.close()
    except OSError:
        pass
    drop_conn(room_name, role, conn)


# --------------------------------------------------------------------------
# HTTP + upgrade handler
# --------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    server_version = "ubuild-ws-signaling/2.0"

    def _send(self, status, body: bytes = b"", content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _serve_viewer(self):
        path = WEB_ROOT / "js" / "viewer.html"
        try:
            body = path.read_bytes()
        except OSError:
            self._send(HTTPStatus.NOT_FOUND, b"viewer.html missing", "text/plain")
            return
        self._send(HTTPStatus.OK, body, "text/html; charset=utf-8")

    def do_GET(self):
        parsed = urlsplit(self.path)
        if parsed.path == "/ws":
            self._handle_ws(parsed)
            return
        if parsed.path == "/signal/callers":
            with STATE_LOCK:
                callers = sorted(
                    name for name, room in ROOMS.items()
                    if room.callee is not None
                )
            body = json.dumps(
                [{"room": n} for n in callers], separators=(",", ":")
            ).encode("utf-8")
            self._send(HTTPStatus.OK, body, "application/json; charset=utf-8")
            return
        if parsed.path in ("/", "/viewer.html", "/js/viewer.html"):
            self._serve_viewer()
            return
        self._send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")

    def _handle_ws(self, parsed):
        query = parse_qs(parsed.query)
        room_name = (query.get("room") or ["default"])[0]
        role = (query.get("role") or ["callee"])[0]
        if role not in ("caller", "callee"):
            self._send(HTTPStatus.BAD_REQUEST, b"bad role", "text/plain")
            return
        key = self.headers.get("Sec-WebSocket-Key", "")
        if self.headers.get("Upgrade", "").lower() != "websocket" or not key:
            self._send(HTTPStatus.BAD_REQUEST, b"websocket upgrade required",
                       "text/plain")
            return
        self.close_connection = True
        # Hand the raw socket over: swallow the rest of the HTTP request line.
        if not ws_handshake(self, key):
            return
        conn = Conn(self.connection)
        run_ws(conn, room_name, role)

    def log_message(self, fmt, *args):
        print(f"[WS] {self.address_string()} - {fmt % args}")


def pending_offer_sweeper(stop_event: threading.Event) -> None:
    """Periodically expire offers that waited longer than OFFER_WAIT_S."""
    while not stop_event.is_set():
        now = time.time()
        expired: list[Conn] = []
        with STATE_LOCK:
            for room in ROOMS.values():
                for caller in list(room.callers.values()):
                    if (caller.pending_sdp is not None and
                            caller.pending_deadline <= now):
                        expired.append(caller)
                        caller.pending_sdp = None
                        caller.pending_deadline = 0.0
        for caller in expired:
            send_json(caller, {"type": "offer_timeout",
                               "reason": "device not online"})
            print(f"[WS] caller {caller.peer_id} offer timed out "
                  f"(no callee in {OFFER_WAIT_S:.0f}s)")
        stop_event.wait(1.0)


def main() -> None:
    parser = argparse.ArgumentParser(description="ubuild WebSocket signaling server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    stop_event = threading.Event()
    sweep_thread = threading.Thread(target=pending_offer_sweeper,
                                    args=(stop_event,), daemon=True)
    sweep_thread.start()
    print(f"[WS] signaling listening on ws://{args.host}:{args.port}/ws?room=<n>&role=caller|callee")
    print(f"[WS] device role=callee, viewer role=caller")
    print(f"[WS] viewer: http://<this-host>:{args.port}/js/viewer.html?room=<n>")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        server.server_close()


if __name__ == "__main__":
    main()
