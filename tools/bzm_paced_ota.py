#!/usr/bin/env python3
"""Upload a raw Bitaxe OTA artifact without an initial TCP-buffer burst."""

from __future__ import annotations

import argparse
import fcntl
from pathlib import Path
import socket
import struct
import time
import urllib.parse


def interface_address(interface: str) -> str:
    encoded = interface.encode("utf-8")
    if not encoded or len(encoded) > 15:
        raise ValueError(f"invalid network interface {interface!r}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as query:
        result = fcntl.ioctl(
            query.fileno(), 0x8915, struct.pack("256s", encoded))
    return socket.inet_ntoa(result[20:24])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--rate-kib", type=float, default=100.0)
    parser.add_argument("--slow-rate-kib", type=float, default=1.0)
    parser.add_argument("--slow-bytes", type=int, default=65536)
    parser.add_argument("--chunk-size", type=int, default=1024)
    parser.add_argument("--io-timeout", type=float, default=180.0)
    args = parser.parse_args()

    if (args.rate_kib <= 0 or args.slow_rate_kib <= 0 or
            args.slow_bytes < 0 or args.chunk_size <= 0 or
            args.io_timeout <= 0):
        parser.error("rates, chunk size, and timeout must be positive")
    parsed = urllib.parse.urlsplit(args.url)
    if parsed.scheme != "http" or parsed.hostname is None:
        parser.error("only explicit HTTP URLs are supported")

    size = args.artifact.stat().st_size
    source = interface_address(args.interface)
    target = (parsed.hostname, parsed.port or 80)
    path = urllib.parse.urlunsplit(("", "", parsed.path or "/", parsed.query, ""))
    headers = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: {parsed.hostname}\r\n"
        "Content-Type: application/octet-stream\r\n"
        f"Content-Length: {size}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii")

    rate = args.rate_kib * 1024.0
    slow_rate = args.slow_rate_kib * 1024.0
    slow_bytes = min(args.slow_bytes, size)
    slow_duration = slow_bytes / slow_rate
    response = bytearray()
    with socket.create_connection(target, timeout=15, source_address=(source, 0)) as connection:
        connection.settimeout(args.io_timeout)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
        connection.sendall(headers)
        started = time.monotonic()
        sent = 0
        next_progress = 10
        with args.artifact.open("rb") as artifact:
            while chunk := artifact.read(args.chunk_size):
                connection.sendall(chunk)
                sent += len(chunk)
                target_elapsed = (
                    sent / slow_rate if sent <= slow_bytes else
                    slow_duration + (sent - slow_bytes) / rate
                )
                delay = target_elapsed - (time.monotonic() - started)
                if delay > 0:
                    time.sleep(delay)
                progress = sent * 100 // size
                if progress >= next_progress:
                    print(f"uploaded {sent}/{size} bytes ({progress}%)", flush=True)
                    next_progress += 10

        while True:
            try:
                chunk = connection.recv(4096)
            except (ConnectionResetError, TimeoutError):
                break
            if not chunk:
                break
            response.extend(chunk)

    status_line, _, body = bytes(response).partition(b"\r\n")
    print(status_line.decode("ascii", errors="replace"))
    _, _, body = body.partition(b"\r\n\r\n")
    if body:
        print(body.decode("utf-8", errors="replace").strip())
    return 0 if b" 200 " in status_line else 1


if __name__ == "__main__":
    raise SystemExit(main())
