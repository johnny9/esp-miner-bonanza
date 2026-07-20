#!/usr/bin/env python3
"""Run one bounded powered BZM 1002 validation and always request safe-off."""

from __future__ import annotations

import http.client
import argparse
import json
import threading
import time

import serial


DEVICE_IP = "192.168.1.193"
SOURCE_IP = "192.168.1.227"
SERIAL_PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_10:B4:1D:E2:17:08-if00"
BASE_PATH = "/api/system/bzm"
TRANSPORT_ERRORS = (ConnectionResetError, TimeoutError, OSError, http.client.HTTPException, json.JSONDecodeError)


def request(method: str, path: str, body: dict | None = None, timeout: float = 180.0) -> tuple[int, dict]:
    encoded = None if body is None else json.dumps(body, separators=(",", ":")).encode()
    headers = {} if encoded is None else {"Content-Type": "application/json"}
    connection = http.client.HTTPConnection(
        DEVICE_IP,
        timeout=timeout,
        source_address=(SOURCE_IP, 0),
    )
    # The ESP HTTP server can exhaust its nonblocking send window while
    # returning the large validation snapshot over a persistent HTTP/1.1
    # connection.  HTTP/1.0 closes after the response and has proven reliable
    # for this bounded hardware-validation client.
    connection._http_vsn = 10
    connection._http_vsn_str = "HTTP/1.0"
    try:
        connection.request(method, path, body=encoded, headers=headers)
        response = connection.getresponse()
        try:
            payload = response.read()
        except http.client.IncompleteRead as error:
            payload = error.partial
            try:
                decoded = json.loads(payload) if payload else {}
            except json.JSONDecodeError:
                decoded = {
                    "transportIncomplete": True,
                    "receivedBytes": len(payload),
                    "missingBytes": error.expected,
                }
            return response.status, decoded
        return response.status, json.loads(payload) if payload else {}
    finally:
        connection.close()


def request_with_retries(
    method: str,
    path: str,
    body: dict | None = None,
    *,
    timeout: float = 15.0,
    attempts: int = 5,
) -> tuple[int, dict]:
    """Retry idempotent status, heartbeat, and safe-off requests across device reconnects."""
    last_error: BaseException | None = None
    for attempt in range(1, attempts + 1):
        try:
            return request(method, path, body, timeout=timeout)
        except TRANSPORT_ERRORS as error:
            last_error = error
            print(
                f"{method}_{path.rsplit('/', 1)[-1]}_RETRY_{attempt} "
                f"{type(error).__name__}: {error}",
                flush=True,
            )
            if attempt != attempts:
                time.sleep(2.0)
    assert last_error is not None
    raise last_error


def summarize(label: str, status: int, snapshot: dict) -> None:
    runtime = snapshot.get("runtime", {})
    fault = snapshot.get("fault", {})
    health = snapshot.get("health", {})
    evidence = snapshot.get("runningEvidence", {})
    report = snapshot.get("report", {})
    bridge = snapshot.get("bridge", {}).get("safetyStatus", {})
    operation = snapshot.get("operation", {})
    print(
        label,
        json.dumps(
            {
                "http": status,
                "operation": operation,
                "runtime": runtime,
                "fault": fault,
                "health": health,
                "runningEvidence": evidence,
                "reportOverall": report.get("overallName"),
                "reportState": report.get("stateName"),
                "reachedStage": report.get("reachedStageName"),
                "finalSafeOff": report.get("finalSafeOff"),
                "bridge": {
                    "state": bridge.get("stateName"),
                    "runtimeVerdict": bridge.get("runtimeVerdictName"),
                    "fiveVoltEnabled": bridge.get("fiveVoltEnabled"),
                    "asicResetAsserted": bridge.get("asicResetAsserted"),
                    "leaseRemainingMs": bridge.get("leaseRemainingMs"),
                },
            },
            separators=(",", ":"),
        ),
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-stage", type=int, choices=range(2, 8), default=7)
    parser.add_argument(
        "--post-proof-seconds",
        type=float,
        default=0.0,
        help="continue heartbeats after Stage-7 proof to exercise job rotation",
    )
    parser.add_argument(
        "--lease-seconds",
        type=int,
        default=60,
        help="powered lease used for admission and heartbeats (default: 60)",
    )
    args = parser.parse_args()
    if args.post_proof_seconds < 0:
        parser.error("--post-proof-seconds must be non-negative")
    if args.lease_seconds < 1 or args.lease_seconds > 300:
        parser.error("--lease-seconds must be in 1..300")
    target_stage = args.target_stage
    serial_chunks: list[bytes] = []
    stop_reader = threading.Event()

    with serial.Serial(SERIAL_PORT, 115200, timeout=0.1) as console:
        console.reset_input_buffer()

        def read_console() -> None:
            while not stop_reader.is_set():
                chunk = console.read(4096)
                if chunk:
                    serial_chunks.append(chunk)

        reader = threading.Thread(target=read_console, daemon=True)
        reader.start()

        last_snapshot: dict = {}
        stopped_snapshot: dict = {}
        stop_status = 0
        try:
            for attempt in range(1, 4):
                # Abort/clear any partial linenoise input left by an earlier
                # serial-console attachment, then prove the one-shot arm via
                # the HTTP snapshot before a powered request is permitted.
                console.write(b"\x03\x15\r\n")
                console.flush()
                time.sleep(0.2)
                console.write(b"bzm-arm ENERGIZE_BZM_1002\r\n")
                console.flush()
                time.sleep(0.5)
                if b"GOOD: BZM powered validation armed once" in b"".join(serial_chunks):
                    print(f"ARM_{attempt} SERIAL_GOOD", flush=True)
                    break
            else:
                raise RuntimeError("USB serial arm did not become active after three clean-line attempts")

            try:
                status, last_snapshot = request(
                    "POST",
                    f"{BASE_PATH}/validation",
                    {
                        "targetStage": target_stage,
                        "hold": target_stage == 7,
                        "leaseSeconds": args.lease_seconds,
                        "confirm": "ENERGIZE_BZM_1002",
                    },
                    timeout=60.0,
                )
                summarize("START", status, last_snapshot)
            except (ConnectionResetError, TimeoutError) as error:
                print(f"START_TRANSPORT_REFRESH {type(error).__name__}: {error}", flush=True)
                status, last_snapshot = request_with_retries("GET", f"{BASE_PATH}/validation")
                summarize("START_REFRESH", status, last_snapshot)
            if last_snapshot.get("transportIncomplete"):
                status, last_snapshot = request_with_retries("GET", f"{BASE_PATH}/validation")
                summarize("START_REFRESH", status, last_snapshot)

            deadline = time.monotonic() + (
                25.0 + args.post_proof_seconds if target_stage == 7 else 0.0
            )
            proof_at: float | None = None
            while target_stage == 7 and status == 200 and time.monotonic() < deadline:
                runtime = last_snapshot.get("runtime", {})
                fault = last_snapshot.get("fault", {})
                evidence = last_snapshot.get("runningEvidence", {})
                if fault.get("latched"):
                    break
                if runtime.get("stage7Complete") or evidence.get("complete"):
                    if proof_at is None:
                        proof_at = time.monotonic()
                    if time.monotonic() - proof_at >= args.post_proof_seconds:
                        break
                time.sleep(2.0)
                heartbeat_status, heartbeat = request_with_retries(
                    "POST",
                    f"{BASE_PATH}/validation/heartbeat",
                    {"leaseSeconds": args.lease_seconds},
                )
                summarize("HEARTBEAT", heartbeat_status, heartbeat)
                last_snapshot = heartbeat
                status = heartbeat_status
        finally:
            try:
                stop_status, stopped_snapshot = request_with_retries(
                    "POST",
                    f"{BASE_PATH}/validation/stop",
                    {"reason": f"bounded Stage {target_stage} validation complete"},
                    timeout=60.0,
                )
                summarize("STOP", stop_status, stopped_snapshot)
            finally:
                time.sleep(0.5)
                stop_reader.set()
                reader.join(timeout=1.0)
                print("SERIAL_BEGIN")
                print(b"".join(serial_chunks).decode("utf-8", errors="replace"))
                print("SERIAL_END")

    fault = last_snapshot.get("fault", {})
    evidence = last_snapshot.get("runningEvidence", {})
    runtime = last_snapshot.get("runtime", {})
    report = last_snapshot.get("report", {})
    if target_stage < 7:
        return 0 if (not fault.get("latched") and report.get("overallName") == "GOOD" and
                     report.get("reachedStage") == target_stage) else 1
    stopped_fault = stopped_snapshot.get("fault", {})
    stopped_evidence = stopped_snapshot.get("runningEvidence", {})
    final_safe_off = stopped_snapshot.get("report", {}).get("finalSafeOff", {})
    return 0 if (
        stop_status == 200 and
        not stopped_fault.get("latched") and
        stopped_evidence.get("complete") and
        final_safe_off.get("statusName") == "GOOD"
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
