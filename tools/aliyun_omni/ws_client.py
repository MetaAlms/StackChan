"""Minimal WebSocket (RFC 6455) client built on the Python standard library.

Why hand-rolled instead of `websockets` / `websocket-client`:
  * zero install friction on any machine that has Python 3.8+
  * full visibility into framing, which is the exact layer that will bite us
    again in C++ on the ESP32
  * the masked-client-frame logic mirrors what the firmware must implement

Only the subset needed by the Aliyun Realtime API is implemented: text and
binary frames, ping/pong, close, and continuation reassembly. No compression
extensions, no permessage-deflate.
"""

from __future__ import annotations

import base64
import os
import socket
import ssl
import struct
from typing import Iterator, Optional, Tuple
from urllib.parse import urlsplit

__all__ = ["WebSocketError", "HandshakeError", "WebSocketClient"]

# RFC 6455 opcodes
OP_CONT = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA


class WebSocketError(Exception):
    """Raised on protocol-level failures."""


class HandshakeError(WebSocketError):
    """Raised when the HTTP upgrade is rejected."""

    def __init__(self, status: str, headers: dict, body: bytes):
        self.status = status
        self.headers = headers
        self.body = body
        self.status_code = status.split()[1] if len(status.split()) > 1 else "?"
        super().__init__(f"handshake failed: {status} {body[:200].decode('utf-8', 'replace')}")


def _recv_exactly(sock: socket.socket, count: int) -> bytes:
    """Read exactly `count` bytes or raise if the peer closes early."""
    chunks = []
    remaining = count
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise WebSocketError(f"connection closed while reading {count} bytes")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


class WebSocketClient:
    """Blocking WebSocket client.

    Usage:
        ws = WebSocketClient("wss://host/path?x=1", headers={"Authorization": "Bearer k"})
        ws.connect()
        ws.send_text('{"type":"session.update", ...}')
        for opcode, payload in ws.messages(timeout=10):
            ...
    """

    def __init__(
        self,
        url: str,
        headers: Optional[dict] = None,
        connect_timeout: float = 15.0,
        read_timeout: float = 30.0,
    ):
        parts = urlsplit(url)
        if parts.scheme not in ("ws", "wss"):
            raise ValueError(f"unsupported scheme: {parts.scheme!r}")
        if not parts.hostname:
            raise ValueError(f"no host in url: {url!r}")

        self.url = url
        self.secure = parts.scheme == "wss"
        self.host = parts.hostname
        self.port = parts.port or (443 if self.secure else 80)
        # urlsplit keeps the query inside `path` when there is no fragment;
        # rebuild the request target including query string.
        self.target = parts.path or "/"
        if parts.query:
            self.target += "?" + parts.query
        self.headers = dict(headers or {})
        self.connect_timeout = connect_timeout
        self.read_timeout = read_timeout

        self._sock: Optional[socket.socket] = None
        self._buffer = b""  # leftover bytes from the handshake read
        self.negotiated_subprotocol: Optional[str] = None
        self.response_headers: dict = {}

    # ---------------------------------------------------------------- connect

    def connect(self) -> None:
        key = base64.b64encode(os.urandom(16)).decode()

        raw = socket.create_connection((self.host, self.port), timeout=self.connect_timeout)
        if self.secure:
            ctx = ssl.create_default_context()
            raw = ctx.wrap_socket(raw, server_hostname=self.host)
        self._sock = raw
        self._sock.settimeout(self.read_timeout)

        lines = [
            f"GET {self.target} HTTP/1.1",
            f"Host: {self.host}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        for name, value in self.headers.items():
            lines.append(f"{name}: {value}")
        request = ("\r\n".join(lines) + "\r\n\r\n").encode()

        self._sock.sendall(request)

        # Read just the response head.
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise WebSocketError("connection closed during handshake")
            buf += chunk
            if len(buf) > 65536:
                raise WebSocketError("handshake response too large")

        head, _, rest = buf.partition(b"\r\n\r\n")
        self._buffer = rest

        head_lines = head.decode("latin-1").split("\r\n")
        status = head_lines[0]
        headers: dict = {}
        for line in head_lines[1:]:
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        self.response_headers = headers

        if not status.startswith("HTTP/1.1 101"):
            self.close()
            raise HandshakeError(status, headers, rest)

        # Verify the accept token so we notice proxies that mangle the upgrade.
        expected = base64.b64encode(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).decode()
        got = headers.get("sec-websocket-accept", "")
        if got != expected:
            raise WebSocketError(f"bad Sec-WebSocket-Accept: {got!r} != {expected!r}")

        self.negotiated_subprotocol = headers.get("sec-websocket-protocol")

    # ------------------------------------------------------------------ send

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        if self._sock is None:
            raise WebSocketError("not connected")

        # Client-to-server frames MUST be masked (RFC 6455 section 5.3).
        length = len(payload)
        header = bytearray()
        header.append(0x80 | opcode)  # FIN set, no RSV bits

        if length < 126:
            header.append(0x80 | length)
        elif length < (1 << 16):
            header.append(0x80 | 126)
            header += struct.pack("!H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", length)

        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self._sock.sendall(bytes(header) + masked)

    def send_text(self, text: str) -> None:
        self._send_frame(OP_TEXT, text.encode("utf-8"))

    def send_binary(self, data: bytes) -> None:
        self._send_frame(OP_BINARY, data)

    def send_pong(self, payload: bytes = b"") -> None:
        self._send_frame(OP_PONG, payload)

    # ------------------------------------------------------------------ recv

    def messages(self, timeout: Optional[float] = None) -> Iterator[Tuple[int, bytes]]:
        """Yield (opcode, payload) for complete text/binary messages.

        Control frames (ping/pong/close) are handled internally: pings are
        answered automatically, and a close frame terminates iteration.
        `socket.timeout` propagates to the caller so it can interleave work.
        """
        if self._sock is None:
            raise WebSocketError("not connected")
        if timeout is not None:
            self._sock.settimeout(timeout)

        buffer = self._buffer
        self._buffer = b""

        fragment_opcode: Optional[int] = None
        fragments: list = []

        while True:
            while len(buffer) < 2:
                buffer += self._sock.recv(4096)
                if not buffer:
                    return

            b0, b1 = buffer[0], buffer[1]
            fin = bool(b0 & 0x80)
            opcode = b0 & 0x0F
            masked = bool(b1 & 0x80)
            length = b1 & 0x7F
            offset = 2

            if length == 126:
                while len(buffer) < offset + 2:
                    buffer += self._sock.recv(4096)
                length = struct.unpack("!H", buffer[offset:offset + 2])[0]
                offset += 2
            elif length == 127:
                while len(buffer) < offset + 8:
                    buffer += self._sock.recv(4096)
                length = struct.unpack("!Q", buffer[offset:offset + 8])[0]
                offset += 8

            if masked:
                while len(buffer) < offset + 4:
                    buffer += self._sock.recv(4096)
                mask = buffer[offset:offset + 4]
                offset += 4
            else:
                mask = None

            while len(buffer) < offset + length:
                buffer += self._sock.recv(65536)

            payload = buffer[offset:offset + length]
            buffer = buffer[offset + length:]

            if mask:
                payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))

            if opcode == OP_PING:
                self.send_pong(payload)
                continue
            if opcode == OP_PONG:
                continue
            if opcode == OP_CLOSE:
                return

            if opcode == OP_CONT:
                if fragment_opcode is None:
                    raise WebSocketError("continuation frame without a start frame")
                fragments.append(payload)
                if fin:
                    yield fragment_opcode, b"".join(fragments)
                    fragment_opcode, fragments = None, []
            else:
                if fin:
                    yield opcode, payload
                else:
                    fragment_opcode = opcode
                    fragments = [payload]

    # ----------------------------------------------------------------- close

    def close(self, code: int = 1000) -> None:
        sock, self._sock = self._sock, None
        if sock is None:
            return
        try:
            self._send_frame(OP_CLOSE, struct.pack("!H", code))
        except Exception:
            pass
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()

    def __enter__(self) -> "WebSocketClient":
        self.connect()
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()
