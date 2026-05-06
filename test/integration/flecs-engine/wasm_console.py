#!/usr/bin/env python3
"""Connect to Chrome DevTools Protocol and stream console output. No pip deps."""

import json, sys, time, base64, os, socket, struct
from urllib.request import urlopen

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
timeout = int(sys.argv[2]) if len(sys.argv) > 2 else 15

# Get WebSocket URL from Chrome DevTools
resp = urlopen(f"http://localhost:{port}/json")
tabs = json.loads(resp.read())
ws_url = None
for t in tabs:
    if "flecs_engine" in t.get("url", ""):
        ws_url = t.get("webSocketDebuggerUrl")
        break
if not ws_url and tabs:
    ws_url = tabs[0].get("webSocketDebuggerUrl")
if not ws_url:
    print("No debugger WebSocket URL found", file=sys.stderr)
    sys.exit(1)

class MiniWS:
    """Minimal RFC 6455 WebSocket client using only stdlib."""
    def __init__(self, url):
        url = url.replace("ws://", "")
        hp, path = url.split("/", 1)
        self.path = "/" + path
        h, p = hp.split(":")
        self.host, self.port = h, int(p)
        self.sock = socket.create_connection((self.host, self.port))
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.sock.recv(4096)

    def send(self, data):
        payload = data.encode()
        mask = os.urandom(4)
        masked = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
        hdr = bytearray([0x81])
        length = len(payload)
        if length < 126:
            hdr.append(0x80 | length)
        elif length < 65536:
            hdr.append(0x80 | 126)
            hdr += struct.pack(">H", length)
        else:
            hdr.append(0x80 | 127)
            hdr += struct.pack(">Q", length)
        hdr += mask
        self.sock.sendall(bytes(hdr) + masked)

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("Connection lost")
            buf += chunk
        return buf

    def recv(self, timeout_s=1.0):
        self.sock.settimeout(timeout_s)
        try:
            header = self._recv_exact(2)
        except (socket.timeout, OSError):
            return None
        opcode = header[0] & 0x0F
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._recv_exact(8))[0]
        payload = self._recv_exact(length)
        if opcode == 0x08:
            raise ConnectionError("WebSocket closed")
        if opcode == 0x01:
            return payload.decode("utf-8", errors="replace")
        return ""

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass

# Connect
ws = MiniWS(ws_url)
msg_id = 0

def cdp_send(method, params=None):
    global msg_id
    msg_id += 1
    msg = {"id": msg_id, "method": method}
    if params:
        msg["params"] = params
    ws.send(json.dumps(msg))

cdp_send("Runtime.enable")
cdp_send("Console.enable")
cdp_send("Log.enable")

start = time.time()
while time.time() - start < timeout:
    raw = ws.recv(timeout_s=1.0)
    if not raw:
        continue
    try:
        msg = json.loads(raw)
    except json.JSONDecodeError:
        continue

    method = msg.get("method", "")

    if method == "Runtime.consoleAPICalled":
        params = msg["params"]
        level = params.get("type", "log")
        args = params.get("args", [])
        parts = []
        for arg in args:
            val = arg.get("value", arg.get("description", str(arg)))
            parts.append(str(val))
        text = " ".join(parts)
        tag = {"error": "ERROR", "warning": "WARN", "info": "INFO"}.get(level, "LOG")
        print(f"[{tag}] {text}", flush=True)

    elif method == "Runtime.exceptionThrown":
        exc = msg["params"].get("exceptionDetails", {})
        text = exc.get("text", "")
        desc = exc.get("exception", {}).get("description", "")
        print(f"[EXCEPTION] {text}", flush=True)
        if desc:
            for line in desc.split("\n"):
                print(f"  {line}", flush=True)

    elif method == "Log.entryAdded":
        entry = msg["params"].get("entry", {})
        level = entry.get("level", "info").upper()
        text = entry.get("text", "")
        print(f"[{level}] {text}", flush=True)

ws.close()
