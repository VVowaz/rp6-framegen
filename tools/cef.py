#!/usr/bin/env python3
"""Evaluate JavaScript in Steam's SharedJSContext through the CEF debug port (8080).

Usage: cef.py '<js expression>'   -> prints the JSON result (promises are awaited).
Stdlib only: minimal RFC 6455 client (masked text frames, no fragmentation).
"""
import base64, json, os, socket, struct, sys, urllib.request

def target():
    tabs = json.load(urllib.request.urlopen("http://127.0.0.1:8080/json"))
    for t in tabs:
        if t.get("title") == "SharedJSContext":
            return t["webSocketDebuggerUrl"]
    raise SystemExit("SharedJSContext not found")

def ws_connect(url):
    rest = url.split("://", 1)[1]
    hostport, path = rest.split("/", 1)
    host, port = hostport.split(":")
    s = socket.create_connection((host, int(port)), timeout=30)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(4096)
    if b" 101 " not in resp.split(b"\r\n")[0]:
        raise SystemExit(resp.decode(errors="replace"))
    return s

def ws_send(s, text):
    data = text.encode()
    hdr = bytearray([0x81])
    n = len(data)
    if n < 126: hdr.append(0x80 | n)
    elif n < 65536: hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
    else: hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
    mask = os.urandom(4)
    s.sendall(bytes(hdr) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk: raise SystemExit("connection closed")
        buf += chunk
    return buf

def ws_recv(s):
    b0, b1 = recv_exact(s, 2)
    n = b1 & 0x7F
    if n == 126: n = struct.unpack(">H", recv_exact(s, 2))[0]
    elif n == 127: n = struct.unpack(">Q", recv_exact(s, 8))[0]
    return recv_exact(s, n).decode(errors="replace")

def evaluate(expr):
    s = ws_connect(target())
    ws_send(s, json.dumps({"id": 1, "method": "Runtime.evaluate",
                           "params": {"expression": expr, "awaitPromise": True, "returnByValue": True}}))
    while True:
        msg = json.loads(ws_recv(s))
        if msg.get("id") == 1:
            return msg["result"]

if __name__ == "__main__":
    r = evaluate(sys.argv[1])
    print(json.dumps(r.get("result", {}).get("value", r), ensure_ascii=False))
