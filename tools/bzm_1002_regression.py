#!/usr/bin/env python3
"""Production regression runner for the Bitaxe 1002 Bonanza miner."""

from __future__ import annotations

import argparse
import fcntl
import functools
import gzip
import hashlib
import http.client
import json
import re
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


REQUIRED_UI_LABELS = (
    "Miner health",
    "Configured pool",
    "Work age:",
    "Useful hashrate",
    "ASIC topology",
    "Bridge and cooling",
    "Mapped results:",
    "Locally valid:",
    "Discarded bytes:",
    "Mapping rejects:",
    "Duplicates:",
    "Dispatch failures:",
)


REQUEST_OPENER = urllib.request.build_opener()


class SourceAddressHTTPHandler(urllib.request.HTTPHandler):
    """Bind device HTTP connections to a selected local IPv4 address."""

    def __init__(self, source_address: str) -> None:
        super().__init__()
        self._source_address = (source_address, 0)

    def http_open(self, request: urllib.request.Request) -> Any:
        connection = functools.partial(
            http.client.HTTPConnection,
            source_address=self._source_address,
        )
        return self.do_open(connection, request)


def bind_request_interface(interface: str) -> str:
    """Bind subsequent HTTP requests to the IPv4 address on interface."""
    global REQUEST_OPENER
    encoded = interface.encode("utf-8")
    if not encoded or len(encoded) > 15:
        raise ValueError(f"invalid network interface {interface!r}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
        result = fcntl.ioctl(
            control.fileno(),
            0x8915,  # Linux SIOCGIFADDR
            struct.pack("256s", encoded),
        )
    source_address = socket.inet_ntoa(result[20:24])
    REQUEST_OPENER = urllib.request.build_opener(
        SourceAddressHTTPHandler(source_address))
    return source_address


@dataclass
class Check:
    name: str
    passed: bool
    detail: str


@dataclass
class Report:
    started_at: float = field(default_factory=time.time)
    checks: list[Check] = field(default_factory=list)
    samples: list[dict[str, Any]] = field(default_factory=list)
    artifacts: dict[str, dict[str, Any]] = field(default_factory=dict)
    context: dict[str, Any] = field(default_factory=dict)

    def check(self, name: str, passed: bool, detail: str) -> None:
        self.checks.append(Check(name, passed, detail))
        marker = "PASS" if passed else "FAIL"
        print(f"[{marker}] {name}: {detail}", flush=True)

    def as_dict(self) -> dict[str, Any]:
        return {
            "startedAt": self.started_at,
            "finishedAt": time.time(),
            "passed": all(item.passed for item in self.checks),
            "context": self.context,
            "artifacts": self.artifacts,
            "checks": [item.__dict__ for item in self.checks],
            "samples": self.samples,
        }


def run_command(report: Report, name: str, command: list[str], cwd: Path) -> bool:
    print(f"[RUN] {name}: {' '.join(command)}", flush=True)
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    detail = f"exit={result.returncode}"
    if result.returncode:
        tail = (result.stdout + result.stderr).strip().splitlines()[-12:]
        detail += "; " + " | ".join(tail)
    report.check(name, result.returncode == 0, detail)
    return result.returncode == 0


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def record_artifact(report: Report, name: str, path: Path) -> None:
    if path.is_file():
        report.artifacts[name] = {
            "path": str(path.resolve()),
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        report.check(f"artifact {name}", True, report.artifacts[name]["sha256"])
    else:
        report.check(f"artifact {name}", False, f"missing {path}")


def request_bytes(base_url: str, path: str, *, data: bytes | None = None,
                  timeout: float = 10.0) -> tuple[bytes, Any]:
    request = urllib.request.Request(
        base_url + path,
        data=data,
        method="POST" if data is not None else "GET",
        headers={
            "Accept": "application/json,text/html,*/*",
            "Accept-Encoding": "identity",
            "Content-Type": "application/octet-stream",
        },
    )
    with REQUEST_OPENER.open(request, timeout=timeout) as response:
        body = response.read()
        if response.headers.get("Content-Encoding") == "gzip" or body[:2] == b"\x1f\x8b":
            body = gzip.decompress(body)
        return body, response.headers


def get_info(base_url: str, *, timeout: float = 10.0) -> dict[str, Any]:
    body, _ = request_bytes(base_url, "/api/system/info", timeout=timeout)
    value = json.loads(body)
    if not isinstance(value, dict):
        raise ValueError("system info response is not an object")
    return value


def wait_for_device(base_url: str, timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    error = "no response"
    while time.monotonic() < deadline:
        try:
            return get_info(base_url)
        except (OSError, ValueError, urllib.error.URLError) as exc:
            error = str(exc)
            time.sleep(2)
    raise RuntimeError(f"device did not answer within {timeout:.0f}s: {error}")


def ota_upload(report: Report, base_url: str, firmware: Path, web: Path,
               boot_timeout: float) -> dict[str, Any]:
    for name, path, endpoint in (
        ("AxeOS", web, "/api/system/OTAWWW"),
        ("ESP firmware", firmware, "/api/system/OTA"),
    ):
        body, _ = request_bytes(base_url, endpoint, data=path.read_bytes(), timeout=180)
        report.check(f"OTA {name}", True, body.decode("utf-8", "replace").strip())
    return wait_for_device(base_url, boot_timeout)


def health_check(report: Report, info: dict[str, Any], *, require_mining: bool,
                 max_work_age: float) -> None:
    health = info.get("asicHealth")
    report.check("board identity", info.get("boardVersion") == "1002",
                 f"boardVersion={info.get('boardVersion')!r}")
    if not isinstance(health, dict):
        report.check("generic ASIC health", False, "asicHealth missing")
        return
    lifecycle = health.get("lifecycle")
    allowed = {"SAFE_OFF", "STARTING", "MINING", "FAULT", "MAINTENANCE"}
    report.check("lifecycle schema", lifecycle in allowed, f"lifecycle={lifecycle}")
    if require_mining:
        report.check("automatic mining", lifecycle == "MINING", f"lifecycle={lifecycle}")
    report.check("four ASIC topology",
                 health.get("asicCount") == health.get("expectedAsicCount") == 4,
                 f"{health.get('asicCount')}/{health.get('expectedAsicCount')}")
    report.check("944 active engines",
                 health.get("activeEngineCount") == health.get("expectedEngineCount") == 944,
                 f"{health.get('activeEngineCount')}/{health.get('expectedEngineCount')}")
    report.check("locked profile",
                 health.get("fixedFrequencyMHz") == 800 and health.get("fixedVoltageMV") == 2800,
                 f"{health.get('fixedFrequencyMHz')} MHz, {health.get('fixedVoltageMV')} mV")
    report.check("bridge compatibility", health.get("bridgeCompatible") is True,
                 f"version={health.get('bridgeVersion')} protocol={health.get('bridgeProtocolMajor')}.{health.get('bridgeProtocolMinor')}")
    report.check("fan telemetry", (health.get("fanRPM") or 0) > 0,
                 f"{health.get('fanPercent')}%, {health.get('fanRPM')} RPM")
    work_age = float(info.get("currentWorkAgeSeconds", -1))
    report.check("fresh pool work", 0 <= work_age <= max_work_age,
                 f"age={work_age:.1f}s connection={info.get('poolConnectionInfo')}")
    report.check("no persistent fault", not health.get("lastFault"),
                 health.get("lastFault") or "none")


def sample_device(report: Report, base_url: str, duration: int,
                  interval: float, request_timeout: float,
                  recovery_timeout: float) -> None:
    deadline = time.monotonic() + duration
    recovery_deadline = deadline + recovery_timeout
    while True:
        try:
            info = get_info(base_url, timeout=request_timeout)
        except (OSError, ValueError, urllib.error.URLError) as exc:
            sample = {"time": time.time(), "requestError": str(exc)}
            report.samples.append(sample)
            print("[SAMPLE] " + json.dumps(sample, sort_keys=True), flush=True)
            now = time.monotonic()
            if now >= recovery_deadline:
                break
            delay = 2.0 if now >= deadline else interval
            time.sleep(min(delay, max(0.0, recovery_deadline - now)))
            continue
        health = info.get("asicHealth") or {}
        sample = {
            "time": time.time(),
            "lifecycle": health.get("lifecycle"),
            "workAgeSeconds": info.get("currentWorkAgeSeconds"),
            "hashRate": info.get("hashRate"),
            "hashRate1m": info.get("hashRate_1m"),
            "sharesAccepted": info.get("sharesAccepted"),
            "sharesRejected": info.get("sharesRejected"),
            "mappedResults": health.get("mappedResults"),
            "locallyValidResults": health.get("locallyValidResults"),
            "mappingRejections": health.get("mappingRejections"),
            "localRejections": health.get("localRejections"),
            "duplicates": health.get("duplicateResults"),
            "dispatchFailures": health.get("dispatchFailures"),
            "parserDiscardedBytes": health.get("parserDiscardedBytes"),
            "parserRecoveries": health.get("parserRecoveries"),
            "transportCrcFailures": health.get("transportCrcFailures"),
            "transportSequenceGaps": health.get("transportSequenceGaps"),
            "bridgePioFifoOverflows": health.get("bridgePioFifoOverflows"),
            "bridgeSoftwareRingOverflows": health.get("bridgeSoftwareRingOverflows"),
        }
        report.samples.append(sample)
        print("[SAMPLE] " + json.dumps(sample, sort_keys=True), flush=True)
        now = time.monotonic()
        if now >= deadline and "requestError" not in sample:
            break
        if now >= recovery_deadline:
            break
        delay = 2.0 if now >= deadline else interval
        time.sleep(min(delay, max(0.0, recovery_deadline - now)))


def check_sample_deltas(report: Report, min_hashrate_ghs: float,
                        sample_interval: float,
                        request_timeout: float) -> None:
    valid_samples = [item for item in report.samples if "requestError" not in item]
    request_errors = len(report.samples) - len(valid_samples)
    consecutive = 0
    max_consecutive = 0
    for item in report.samples:
        if "requestError" in item:
            consecutive += 1
            max_consecutive = max(max_consecutive, consecutive)
        else:
            consecutive = 0
    success_gaps = [
        valid_samples[index]["time"] - valid_samples[index - 1]["time"]
        for index in range(1, len(valid_samples))
    ]
    max_success_gap = max(success_gaps, default=0.0)
    maximum_recovery_gap = max(30.0, sample_interval * 3.0 +
                               request_timeout * 2.0)
    api_recovered = bool(valid_samples) and consecutive == 0
    api_continuous = (api_recovered and max_consecutive <= 2 and
                      max_success_gap <= maximum_recovery_gap)
    report.check(
        "API sampling continuity", api_continuous,
        f"successful={len(valid_samples)} transientErrors={request_errors} "
        f"maxConsecutive={max_consecutive} recovered={api_recovered} "
        f"maxSuccessGap={max_success_gap:.1f}s/{maximum_recovery_gap:.1f}s")
    if len(valid_samples) < 2:
        report.check("live samples", False, "fewer than two samples")
        return
    first, last = valid_samples[0], valid_samples[-1]
    non_mining = [item.get("lifecycle") for item in valid_samples
                  if item.get("lifecycle") != "MINING"]
    report.check("lifecycle remains mining", not non_mining,
                 "all samples MINING" if not non_mining else
                 f"non-mining samples={non_mining}")
    valid_delta = (last.get("locallyValidResults") or 0) - (first.get("locallyValidResults") or 0)
    report.check("locally valid results increase", valid_delta > 0, f"delta={valid_delta}")
    duplicate_delta = ((last.get("duplicates") or 0) -
                       (first.get("duplicates") or 0))
    rejected_share_delta = ((last.get("sharesRejected") or 0) -
                            (first.get("sharesRejected") or 0))
    report.check("no duplicate share submission", rejected_share_delta == 0,
                 f"raw duplicate results suppressed={duplicate_delta} "
                 f"pool rejected-share delta={rejected_share_delta}")
    report.check("no dispatch failures",
                 (last.get("dispatchFailures") or 0) == (first.get("dispatchFailures") or 0),
                 f"delta={(last.get('dispatchFailures') or 0) - (first.get('dispatchFailures') or 0)}")
    ring_delta = ((last.get("bridgeSoftwareRingOverflows") or 0) -
                  (first.get("bridgeSoftwareRingOverflows") or 0))
    report.check("bridge software ring remains lossless", ring_delta == 0,
                 f"delta={ring_delta}")
    rates = [float(item.get("hashRate") or 0) for item in valid_samples[len(valid_samples) // 2:]]
    average = sum(rates) / len(rates)
    report.check("sustained local hashrate", average >= min_hashrate_ghs,
                 f"second-half average={average:.2f} GH/s floor={min_hashrate_ghs:.2f} GH/s")
    ages = [float(item.get("workAgeSeconds") or 0) for item in valid_samples]
    report.check("pool work remains live", max(ages) <= 90,
                 f"age range={min(ages):.1f}..{max(ages):.1f}s")


def check_ui(report: Report, base_url: str) -> None:
    body, _ = request_bytes(base_url, "/")
    html = body.decode("utf-8", "replace")
    rendered_sources = [html]
    for asset in re.findall(r'(?:src|href)=["\']([^"\']+\.(?:js|css))["\']', html):
        asset_path = "/" + asset.lstrip("/")
        asset_body, _ = request_bytes(base_url, asset_path)
        rendered_sources.append(asset_body.decode("utf-8", "replace"))
    bundle_text = "\n".join(rendered_sources)
    missing = [label for label in REQUIRED_UI_LABELS if label not in bundle_text]
    report.check("AxeOS production health labels", not missing,
                 "all required labels present" if not missing else f"missing={missing}")


def write_junit(report: Report, path: Path) -> None:
    suite = ET.Element("testsuite", name="bzm-1002-regression",
                       tests=str(len(report.checks)),
                       failures=str(sum(not item.passed for item in report.checks)))
    for item in report.checks:
        case = ET.SubElement(suite, "testcase", name=item.name)
        if not item.passed:
            ET.SubElement(case, "failure", message=item.detail).text = item.detail
        else:
            ET.SubElement(case, "system-out").text = item.detail
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="device IP address or hostname")
    parser.add_argument("--interface", help="source network interface used for device HTTP")
    parser.add_argument("--serial", help="serial device recorded in the report")
    parser.add_argument("--soak-seconds", type=int, default=180)
    parser.add_argument("--sample-interval", type=float, default=10)
    parser.add_argument("--request-timeout", type=float, default=3)
    parser.add_argument("--recovery-timeout", type=float, default=30)
    parser.add_argument("--boot-timeout", type=float, default=240)
    parser.add_argument("--max-work-age", type=float, default=90)
    parser.add_argument("--min-hashrate-ghs", type=float, default=600)
    parser.add_argument("--report", type=Path, default=Path("build-mvo/bzm-regression.json"))
    parser.add_argument("--junit", type=Path)
    parser.add_argument("--bridge-repo", type=Path,
                        default=Path(__file__).resolve().parents[2] / "bonanza-bridge-fw-mvo")
    parser.add_argument("--skip-local", action="store_true")
    parser.add_argument("--local-only", action="store_true")
    parser.add_argument("--ota", action="store_true",
                        help="explicitly authorize guarded AxeOS and ESP firmware OTA")
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--web", type=Path)
    parser.add_argument("--restart-check", action="store_true",
                        help="explicitly request a safe production restart after sampling")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo = Path(__file__).resolve().parents[1]
    report = Report()
    report.context = {
        "device": args.device,
        "interface": args.interface,
        "serial": args.serial,
        "soakSeconds": args.soak_seconds,
        "espRepo": str(repo),
        "bridgeRepo": str(args.bridge_repo.resolve()),
    }

    if args.ota and (args.firmware is None or args.web is None):
        raise SystemExit("--ota requires both --firmware and --web")
    if not args.local_only and not args.device:
        raise SystemExit("--device is required unless --local-only is used")
    if args.interface:
        try:
            report.context["sourceAddress"] = bind_request_interface(args.interface)
        except (OSError, ValueError) as exc:
            raise SystemExit(
                f"cannot bind device HTTP to interface {args.interface!r}: {exc}") from exc

    if not args.skip_local:
        run_command(report, "production config", [sys.executable, "tools/validate_bitaxe_1002_config.py"], repo)
        run_command(report, "ESP QEMU unit tests", ["tools/run_qemu_tests.sh"], repo)
        run_command(report, "AxeOS production build", ["npm", "run", "build:no-api-gen"],
                    repo / "main/http_server/axe-os")
        if args.bridge_repo.is_dir():
            run_command(report, "bridge host tests",
                        ["cargo", "test", "--lib", "--target", "x86_64-unknown-linux-gnu"],
                        args.bridge_repo)
        else:
            report.check("bridge repository", False, f"missing {args.bridge_repo}")

    firmware = (args.firmware or repo / "build-mvo/esp-miner.bin").resolve()
    web = (args.web or repo / "build-mvo/www.bin").resolve()
    record_artifact(report, "esp-miner.bin", firmware)
    record_artifact(report, "www.bin", web)

    if not args.local_only:
        base_url = "http://" + args.device.rstrip("/")
        info = wait_for_device(base_url, args.boot_timeout)
        if args.ota:
            info = ota_upload(report, base_url, firmware, web, args.boot_timeout)

        deadline = time.monotonic() + args.boot_timeout
        while (info.get("asicHealth") or {}).get("lifecycle") not in {"MINING", "FAULT"} and time.monotonic() < deadline:
            time.sleep(2)
            try:
                info = get_info(base_url)
            except (OSError, ValueError, urllib.error.URLError):
                continue
        health_check(report, info, require_mining=True, max_work_age=args.max_work_age)
        try:
            check_ui(report, base_url)
        except (OSError, ValueError, urllib.error.URLError) as exc:
            report.check("AxeOS production health labels", False,
                         f"UI request failed: {exc}")
        sample_device(report, base_url, args.soak_seconds,
                      args.sample_interval, args.request_timeout,
                      args.recovery_timeout)
        check_sample_deltas(report, args.min_hashrate_ghs,
                            args.sample_interval, args.request_timeout)

        if args.restart_check:
            body, _ = request_bytes(base_url, "/api/system/restart", data=b"{}")
            report.check("safe restart request", True, body.decode("utf-8", "replace").strip())
            wait_for_device(base_url, args.boot_timeout)

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report.as_dict(), indent=2, sort_keys=True) + "\n")
    if args.junit:
        write_junit(report, args.junit)
    print(f"Report: {args.report.resolve()}")
    return 0 if all(item.passed for item in report.checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
