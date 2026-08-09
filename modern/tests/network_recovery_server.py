# SPDX-License-Identifier: LGPL-2.1-or-later

import argparse
import http.server
import socket
import subprocess
import threading
from pathlib import Path
from urllib.parse import urlsplit


class RecoveryServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, media_path: Path, audio_path: Path):
        super().__init__(address, RecoveryHandler)
        self.media_path = media_path
        self.audio_path = audio_path
        self.request_counts: dict[str, int] = {}
        self.count_lock = threading.Lock()

    def next_request(self, path: str) -> int:
        with self.count_lock:
            value = self.request_counts.get(path, 0) + 1
            self.request_counts[path] = value
            return value


class RecoveryHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args) -> None:
        return

    def do_GET(self) -> None:
        path = urlsplit(self.path).path
        request_number = self.server.next_request(path)
        if path == "/always-503.mp4" or (
            path == "/open-retry.mp4" and request_number == 1
        ):
            self.send_response(503)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        if path in (
            "/open-retry.mp4",
            "/read-retry.mp4",
            "/read-exhausted.mp4",
            "/stable.mp4",
        ):
            media_path = self.server.media_path
        elif path == "/external-read-retry.m4a":
            media_path = self.server.audio_path
        else:
            self.send_error(404)
            return

        media_size = media_path.stat().st_size
        start = 0
        end = media_size - 1
        range_header = self.headers.get("Range")
        if range_header and range_header.startswith("bytes="):
            requested = range_header[6:].split(",", 1)[0]
            first, _, last = requested.partition("-")
            if first:
                start = int(first)
            if last:
                end = min(end, int(last))
        if start >= media_size or start > end:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{media_size}")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        content_length = end - start + 1
        self.send_response(206 if range_header else 200)
        self.send_header(
            "Content-Type",
            "audio/mp4" if path.endswith(".m4a") else "video/mp4",
        )
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(content_length))
        self.send_header("Connection", "close")
        if range_header:
            self.send_header(
                "Content-Range", f"bytes {start}-{end}/{media_size}"
            )
        self.end_headers()
        self.close_connection = True

        fault_read = (
            path in ("/read-retry.mp4", "/external-read-retry.m4a")
            and request_number == 1
            and start == 0
        ) or path == "/read-exhausted.mp4"
        remaining = content_length
        if fault_read:
            if path == "/read-exhausted.mp4" and start > 0:
                threshold = 0
            elif path == "/read-exhausted.mp4" and request_number > 1:
                # Keep fast-start metadata readable while preventing the
                # replacement from reaching its first packet after seeking.
                threshold = 96 * 1024
            else:
                threshold = (
                    max(96 * 1024, media_size // 2)
                    if path.endswith(".m4a")
                    else max(512 * 1024, media_size // 4)
                )
            remaining = min(remaining, threshold)

        try:
            with media_path.open("rb") as media:
                media.seek(start)
                while remaining > 0:
                    payload = media.read(min(64 * 1024, remaining))
                    if not payload:
                        break
                    self.wfile.write(payload)
                    self.wfile.flush()
                    remaining -= len(payload)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            return

        if fault_read:
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", required=True)
    parser.add_argument("--media", required=True)
    parser.add_argument("--audio", required=True)
    args = parser.parse_args()

    server = RecoveryServer(
        ("127.0.0.1", 0),
        Path(args.media).resolve(),
        Path(args.audio).resolve(),
    )
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    host, port = server.server_address
    try:
        command = [args.player, f"http://{host}:{port}"]
        completed = subprocess.run(
            command,
            check=False,
            timeout=70,
        )
        return completed.returncode
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
