#!/usr/bin/env python3
"""Destructive HIL regression for factory-blank BZM bridge recovery."""

from __future__ import annotations

import argparse
import binascii
import fcntl
import hashlib
import http.client
import json
import string
import shutil
import socket
import struct
import subprocess
import time
import urllib.parse
from pathlib import Path
from typing import Any


CONFIRMATION = "ERASE-BRIDGE-FIRMWARE"
FLASH_VECTOR_OFFSET = 0x100
RP2040_SRAM_START = 0x20000000
RP2040_SRAM_END = 0x20042000
RP2040_XIP_START = 0x10000100
RP2040_XIP_END = 0x10200000
BRIDGE_FLASH_CAPACITY = 2 * 1024 * 1024
BLANK_RESET_OFFSET = 0x108
BLANK_IMAGE_SIZE = BLANK_RESET_OFFSET + 2
THUMB_BRANCH_TO_SELF = b"\xfe\xe7"
BRIDGE_MANIFEST_MAGIC = b"BZM-BRIDGE-FW\0\0\0"
BRIDGE_MANIFEST_SIZE = 96
BRIDGE_MANIFEST_CRC_OFFSET = BRIDGE_MANIFEST_SIZE - 4
BRIDGE_MANIFEST_VERSION_OFFSET = 24
BRIDGE_MANIFEST_SCHEMA = 1
BRIDGE_MANIFEST_KIND = 1
BRIDGE_MANIFEST_TARGET_BOARD = 1002


class RegressionError(RuntimeError):
    """A regression precondition or runtime assertion failed."""


def normalize_mac(value: str) -> str:
    compact = "".join(character for character in value if character.isalnum())
    if len(compact) != 12 or any(
            character not in string.hexdigits for character in compact):
        raise RegressionError(f"invalid MAC address {value!r}")
    return ":".join(compact[index:index + 2] for index in range(0, 12, 2)).upper()


def read_le32(image: bytes, offset: int) -> int:
    return struct.unpack_from("<I", image, offset)[0]


def validate_bridge_image(image: bytes) -> None:
    if len(image) < FLASH_VECTOR_OFFSET + 8 or len(image) > BRIDGE_FLASH_CAPACITY:
        raise RegressionError(
            f"bridge image size {len(image)} is outside the supported range")
    if not any(byte not in (0x00, 0xFF)
               for byte in image[:FLASH_VECTOR_OFFSET]):
        raise RegressionError("bridge image has no valid boot2 data")

    stack_pointer = read_le32(image, FLASH_VECTOR_OFFSET)
    reset_vector = read_le32(image, FLASH_VECTOR_OFFSET + 4)
    reset_address = reset_vector & ~1
    if stack_pointer & 0x3:
        raise RegressionError("bridge image stack pointer is not aligned")
    if not RP2040_SRAM_START <= stack_pointer <= RP2040_SRAM_END:
        raise RegressionError("bridge image stack pointer is outside RP2040 SRAM")
    if reset_vector & 1 == 0:
        raise RegressionError("bridge image reset vector is not Thumb code")
    if not RP2040_XIP_START <= reset_address < RP2040_XIP_END:
        raise RegressionError("bridge image reset vector is outside RP2040 XIP")
    if reset_address >= 0x10000000 + len(image):
        raise RegressionError("bridge image reset vector is outside the image")


def parse_bridge_manifest(image: bytes) -> dict[str, Any]:
    offsets: list[int] = []
    offset = image.find(BRIDGE_MANIFEST_MAGIC)
    while offset >= 0:
        offsets.append(offset)
        offset = image.find(BRIDGE_MANIFEST_MAGIC, offset + 1)
    if not offsets:
        raise RegressionError("bridge image has no BZM firmware manifest")
    if len(offsets) != 1:
        raise RegressionError("bridge image has duplicate BZM firmware manifests")

    offset = offsets[0]
    if offset + BRIDGE_MANIFEST_SIZE > len(image):
        raise RegressionError("bridge firmware manifest is truncated")
    encoded = image[offset:offset + BRIDGE_MANIFEST_SIZE]
    schema = encoded[16]
    manifest_size = encoded[17]
    target_board = struct.unpack_from("<H", encoded, 18)[0]
    firmware_kind = encoded[20]
    protocol_major = encoded[21]
    protocol_minor = encoded[22]
    version_length = encoded[23]
    if (schema != BRIDGE_MANIFEST_SCHEMA or
            manifest_size != BRIDGE_MANIFEST_SIZE or
            target_board != BRIDGE_MANIFEST_TARGET_BOARD or
            firmware_kind != BRIDGE_MANIFEST_KIND or
            protocol_major != 1 or
            not 1 <= version_length <= 63):
        raise RegressionError("bridge firmware manifest identity is invalid")

    version_bytes = encoded[
        BRIDGE_MANIFEST_VERSION_OFFSET:
        BRIDGE_MANIFEST_VERSION_OFFSET + version_length]
    if any(byte < 0x20 or byte > 0x7E for byte in version_bytes):
        raise RegressionError("bridge firmware manifest version is invalid")
    if any(encoded[
            BRIDGE_MANIFEST_VERSION_OFFSET + version_length:
            BRIDGE_MANIFEST_CRC_OFFSET]):
        raise RegressionError("bridge firmware manifest padding is invalid")

    expected_crc = struct.unpack_from(
        "<I", encoded, BRIDGE_MANIFEST_CRC_OFFSET)[0]
    actual_crc = binascii.crc32(
        encoded[:BRIDGE_MANIFEST_CRC_OFFSET]) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise RegressionError("bridge firmware manifest CRC is invalid")
    return {
        "offset": offset,
        "targetBoardVersion": target_board,
        "protocolMajor": protocol_major,
        "protocolMinor": protocol_minor,
        "version": version_bytes.decode("ascii"),
    }


def make_nonresponsive_bridge_image(good_image: bytes) -> bytes:
    """Keep boot2, then loop before configuring UART, GPIO, or safety outputs."""
    validate_bridge_image(good_image)
    if len(good_image) < BLANK_IMAGE_SIZE:
        raise RegressionError("bridge recovery image is too short")

    blank_image = bytearray(good_image[:BLANK_IMAGE_SIZE])
    struct.pack_into(
        "<I", blank_image, FLASH_VECTOR_OFFSET + 4,
        0x10000000 + BLANK_RESET_OFFSET + 1)
    blank_image[BLANK_RESET_OFFSET:] = THUMB_BRANCH_TO_SELF
    result = bytes(blank_image)
    validate_bridge_image(result)
    return result


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def interface_address(interface: str) -> str:
    encoded = interface.encode("utf-8")
    if not encoded or len(encoded) > 15:
        raise RegressionError(f"invalid network interface {interface!r}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as query:
        result = fcntl.ioctl(
            query.fileno(), 0x8915, struct.pack("256s", encoded))
    return socket.inet_ntoa(result[20:24])


def parse_udev_properties(output: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            properties[key] = value
    return properties


def serial_device_mac(serial: Path) -> str:
    if not serial.exists():
        raise RegressionError(f"serial device does not exist: {serial}")
    if shutil.which("udevadm") is None:
        raise RegressionError("udevadm is required to verify serial identity")
    result = subprocess.run(
        ["udevadm", "info", "--query=property", f"--name={serial}"],
        text=True, capture_output=True, timeout=10)
    if result.returncode != 0:
        raise RegressionError(
            f"udevadm could not inspect {serial}: {result.stderr.strip()}")
    properties = parse_udev_properties(result.stdout)
    if properties.get("ID_VENDOR_ID") != "303a":
        raise RegressionError(
            f"{serial} is not an Espressif USB device")
    serial_mac = properties.get("ID_SERIAL_SHORT")
    if not serial_mac:
        raise RegressionError(f"{serial} has no USB serial MAC")
    return normalize_mac(serial_mac)


class DeviceHttp:
    def __init__(self, device: str, interface: str | None, timeout: float):
        base_url = device if "://" in device else f"http://{device}"
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme != "http" or parsed.hostname is None:
            raise RegressionError("--device must be an HTTP host or address")
        if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
            raise RegressionError("--device must not include a path, query, or fragment")
        self.host = parsed.hostname
        self.port = parsed.port or 80
        self.timeout = timeout
        self.source_address = (
            (interface_address(interface), 0) if interface else None)

    def request(
        self, method: str, path: str, body: bytes | None = None,
        content_type: str = "application/json",
    ) -> tuple[int, bytes]:
        connection = http.client.HTTPConnection(
            self.host, self.port, timeout=self.timeout,
            source_address=self.source_address)
        try:
            connection.request(
                method, path, body=body,
                headers={
                    "Accept": "application/json",
                    "Content-Type": content_type,
                    "Connection": "close",
                })
            response = connection.getresponse()
            response_body = response.read()
            if response.status >= 400:
                detail = response_body.decode("utf-8", "replace").strip()
                raise RegressionError(
                    f"{method} {path} returned {response.status}: {detail}")
            return response.status, response_body
        finally:
            connection.close()

    def get_json(self, path: str) -> dict[str, Any]:
        _, body = self.request("GET", path)
        value = json.loads(body)
        if not isinstance(value, dict):
            raise RegressionError(f"GET {path} did not return an object")
        return value

    def post_json(self, path: str) -> tuple[int, dict[str, Any]]:
        status, body = self.request("POST", path, b"{}", "application/json")
        value = json.loads(body)
        if not isinstance(value, dict):
            raise RegressionError(f"POST {path} did not return an object")
        return status, value

    def post_bridge_image(
        self, image: bytes, *, force: bool = False,
    ) -> dict[str, Any]:
        path = "/api/system/bridge/firmware"
        if force:
            path += "?force=true"
        status, body = self.request(
            "POST", path, image,
            "application/octet-stream")
        if status != 202:
            raise RegressionError(
                f"bridge update returned HTTP {status}, expected 202")
        value = json.loads(body)
        if not isinstance(value, dict) or value.get("running") is not True:
            raise RegressionError(
                f"bridge update was not accepted: {value!r}")
        return value


def validate_device_identity(info: dict[str, Any], expected_mac: str) -> None:
    if info.get("boardVersion") != "1002":
        raise RegressionError(
            f"expected board 1002, got {info.get('boardVersion')!r}")
    if info.get("ASICModel") != "BZM":
        raise RegressionError(
            f"expected BZM ASIC, got {info.get('ASICModel')!r}")
    if info.get("isPSRAMAvailable") != 1:
        raise RegressionError("device PSRAM is unavailable")
    actual_mac = normalize_mac(str(info.get("macAddr", "")))
    if actual_mac != normalize_mac(expected_mac):
        raise RegressionError(
            f"device MAC {actual_mac} does not match {normalize_mac(expected_mac)}")


def validate_initial_mining(info: dict[str, Any]) -> None:
    health = info.get("asicHealth")
    if not isinstance(health, dict):
        raise RegressionError("initial ASIC health is missing")
    if health.get("lifecycle") != "MINING":
        raise RegressionError(
            f"device must start in MINING, got {health.get('lifecycle')!r}")
    if health.get("asicCount") != 4:
        raise RegressionError(
            f"expected four ASICs, got {health.get('asicCount')!r}")
    if (health.get("activeEngineCount"), health.get("expectedEngineCount")) != (
            944, 944):
        raise RegressionError(
            "device must start with 944/944 active engines")
    if info.get("miningPaused") is not False:
        raise RegressionError("device unexpectedly starts with mining paused")


def validate_blank_state(
    info: dict[str, Any], *, require_pool_disconnected: bool = True,
) -> None:
    health = info.get("asicHealth")
    if not isinstance(health, dict):
        raise RegressionError("blank-bridge ASIC health is missing")
    if info.get("miningPaused") is not True:
        raise RegressionError("blank-bridge device is not mining-paused")
    if health.get("lifecycle") != "FAULT":
        raise RegressionError(
            f"blank-bridge lifecycle is {health.get('lifecycle')!r}, not FAULT")
    if health.get("activeEngineCount") != 0:
        raise RegressionError(
            f"blank-bridge device has {health.get('activeEngineCount')} active engines")
    if (require_pool_disconnected and
            info.get("poolConnectionInfo") != "Not Connected"):
        raise RegressionError(
            f"blank-bridge pool state is {info.get('poolConnectionInfo')!r}")


def validate_final_mining(
    info: dict[str, Any], expected_bridge_version: str,
) -> None:
    validate_initial_mining(info)
    health = info["asicHealth"]
    if health.get("bridgeVersion") != expected_bridge_version:
        raise RegressionError(
            f"final bridge version {health.get('bridgeVersion')!r} "
            f"does not match {expected_bridge_version!r}")
    if health.get("lastFault"):
        raise RegressionError(
            f"final device still reports fault {health.get('lastFault')!r}")


def wait_for_info(
    device: DeviceHttp, timeout: float, expected_mac: str,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error = "no response"
    while time.monotonic() < deadline:
        try:
            info = device.get_json("/api/system/info")
            validate_device_identity(info, expected_mac)
            return info
        except (OSError, ValueError, json.JSONDecodeError, RegressionError) as exc:
            last_error = str(exc)
            time.sleep(2)
    raise RegressionError(
        f"device did not return valid system info within {timeout:.0f}s: "
        f"{last_error}")


def wait_for_update(
    device: DeviceHttp, timeout: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    previous: tuple[Any, Any] | None = None
    last_error = "no status"
    while time.monotonic() < deadline:
        try:
            status = device.get_json(
                "/api/system/bridge/firmware/status")
            current = (status.get("state"), status.get("progress"))
            if current != previous:
                print(
                    f"[STATUS] bridge update state={current[0]} "
                    f"progress={current[1]}%", flush=True)
                previous = current
            if status.get("running") is False:
                return status
        except (OSError, ValueError, json.JSONDecodeError, RegressionError) as exc:
            last_error = str(exc)
        time.sleep(1)
    raise RegressionError(
        f"bridge update did not finish within {timeout:.0f}s: {last_error}")


def require_blank_update_result(status: dict[str, Any]) -> None:
    if status.get("state") != "failed" or status.get("progress") != 100:
        raise RegressionError(
            f"nonresponsive image produced unexpected status {status!r}")
    if not status.get("error"):
        raise RegressionError(
            "nonresponsive bridge update did not report reconnect failure")
    if status.get("manifestValidated") is not False:
        raise RegressionError(
            "nonresponsive image unexpectedly passed manifest validation")
    if status.get("forceRequested") is not True:
        raise RegressionError(
            "nonresponsive image was not recorded as a forced test upload")


def require_good_update_result(
    status: dict[str, Any], expected_bridge_version: str,
) -> None:
    if (status.get("state"), status.get("progress"),
            status.get("running")) != ("complete", 100, False):
        raise RegressionError(
            f"recovery image produced unexpected status {status!r}")
    if status.get("versionQuerySupported") is not True:
        raise RegressionError("recovered bridge version query is unavailable")
    if status.get("manifestValidated") is not True:
        raise RegressionError(
            "known-good bridge manifest was not validated")
    if status.get("forceRequested") is not False:
        raise RegressionError(
            "known-good bridge firmware unexpectedly used force")
    if status.get("imageVersion") != expected_bridge_version:
        raise RegressionError(
            f"image manifest version {status.get('imageVersion')!r} "
            f"does not match {expected_bridge_version!r}")
    if status.get("currentVersion") != expected_bridge_version:
        raise RegressionError(
            f"recovered bridge version {status.get('currentVersion')!r} "
            f"does not match {expected_bridge_version!r}")
    if status.get("error") is not None:
        raise RegressionError(
            f"recovered bridge reports error {status.get('error')!r}")


def validate_bridge_api(
    device: DeviceHttp, expected_available: bool,
    expected_version: str | None = None,
) -> dict[str, Any]:
    bridge = device.get_json("/api/system/bridge")
    if bridge.get("available") is not expected_available:
        raise RegressionError(
            f"bridge availability is {bridge.get('available')!r}, "
            f"expected {expected_available}")
    if expected_version is not None and bridge.get("version") != expected_version:
        raise RegressionError(
            f"bridge API version {bridge.get('version')!r} "
            f"does not match {expected_version!r}")
    return bridge


def verify_blank_boot_stability(
    device: DeviceHttp, first: dict[str, Any], duration: float,
    expected_mac: str,
) -> dict[str, Any]:
    validate_blank_state(first)
    first_uptime = int(first.get("uptimeSeconds", -1))
    if first_uptime < 0:
        raise RegressionError("blank-bridge uptime is unavailable")
    previous_uptime = first_uptime
    last = first
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        time.sleep(min(2.0, max(0.0, deadline - time.monotonic())))
        last = wait_for_info(device, min(10.0, duration + 2.0), expected_mac)
        validate_blank_state(last)
        uptime = int(last.get("uptimeSeconds", -1))
        if uptime < previous_uptime:
            raise RegressionError(
                f"device rebooted during blank-bridge wait: "
                f"{previous_uptime}s -> {uptime}s")
        previous_uptime = uptime
    elapsed_uptime = previous_uptime - first_uptime
    minimum_growth = max(1, int(duration) - 5)
    if elapsed_uptime < minimum_growth:
        raise RegressionError(
            f"blank-bridge uptime grew only {elapsed_uptime}s during "
            f"a {duration:.0f}s stability check")
    return last


def reset_esp_with_esptool(
    esptool: str, serial: Path, expected_mac: str,
) -> None:
    executable = shutil.which(esptool)
    if executable is None:
        raise RegressionError(
            f"{esptool!r} is not available; source the ESP-IDF environment")
    command = [
        executable, "--chip", "esp32s3", "--port", str(serial), "flash_id",
    ]
    print(f"[RUN] {' '.join(command)}", flush=True)
    result = subprocess.run(
        command, text=True, capture_output=True, timeout=60)
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise RegressionError(
            f"esptool reset failed: {' | '.join(output.splitlines()[-12:])}")
    normalized_output = "".join(
        character for character in output if character.isalnum()).upper()
    expected_compact = normalize_mac(expected_mac).replace(":", "")
    if expected_compact not in normalized_output:
        raise RegressionError(
            "esptool did not report the expected ESP32-S3 MAC")
    print("[PASS] controlled ESP32-S3 USB reset", flush=True)


def wait_for_mining(
    device: DeviceHttp, timeout: float, expected_mac: str,
    expected_bridge_version: str,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error = "no response"
    while time.monotonic() < deadline:
        try:
            info = device.get_json("/api/system/info")
            validate_device_identity(info, expected_mac)
            validate_final_mining(info, expected_bridge_version)
            return info
        except (OSError, ValueError, json.JSONDecodeError, RegressionError) as exc:
            last_error = str(exc)
            time.sleep(2)
    raise RegressionError(
        f"device did not return to mining within {timeout:.0f}s: {last_error}")


def summarize_info(info: dict[str, Any]) -> dict[str, Any]:
    health = info.get("asicHealth")
    health = health if isinstance(health, dict) else {}
    return {
        "uptimeSeconds": info.get("uptimeSeconds"),
        "resetReason": info.get("resetReason"),
        "version": info.get("version"),
        "axeOSVersion": info.get("axeOSVersion"),
        "boardVersion": info.get("boardVersion"),
        "ASICModel": info.get("ASICModel"),
        "macAddr": info.get("macAddr"),
        "miningPaused": info.get("miningPaused"),
        "poolConnectionInfo": info.get("poolConnectionInfo"),
        "lifecycle": health.get("lifecycle"),
        "asicCount": health.get("asicCount"),
        "activeEngineCount": health.get("activeEngineCount"),
        "expectedEngineCount": health.get("expectedEngineCount"),
        "bridgeVersion": health.get("bridgeVersion"),
        "lastFault": health.get("lastFault"),
        "hashRate": info.get("hashRate"),
        "hashRate1m": info.get("hashRate_1m"),
    }


def restore_bridge(
    device: DeviceHttp, good_image: bytes, update_timeout: float,
    expected_bridge_version: str,
) -> dict[str, Any] | None:
    try:
        running = device.get_json(
            "/api/system/bridge/firmware/status")
        if running.get("running") is True:
            wait_for_update(device, update_timeout)
    except (OSError, ValueError, json.JSONDecodeError, RegressionError):
        pass

    try:
        current = validate_bridge_api(
            device, True, expected_bridge_version)
        print(
            f"[PASS] bridge already restored: {current.get('version')}",
            flush=True)
        return None
    except (OSError, ValueError, json.JSONDecodeError, RegressionError):
        pass

    print("[RECOVERY] uploading known-good bridge firmware", flush=True)
    device.post_bridge_image(good_image)
    status = wait_for_update(device, update_timeout)
    require_good_update_result(status, expected_bridge_version)
    validate_bridge_api(device, True, expected_bridge_version)
    print("[PASS] known-good bridge firmware restored", flush=True)
    return status


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True,
                        help="device IP address or hostname")
    parser.add_argument("--interface",
                        help="source network interface for device HTTP")
    parser.add_argument("--serial", required=True, type=Path,
                        help="matching ESP32-S3 USB serial device")
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--bridge-firmware", required=True, type=Path,
                        help="known-good raw RP2040 .bin used for recovery")
    parser.add_argument("--expected-bridge-sha256", required=True,
                        help="expected SHA-256 of the recovery binary")
    parser.add_argument("--expected-bridge-version", required=True)
    parser.add_argument("--confirm", required=True, choices=(CONFIRMATION,),
                        help=f"must be exactly {CONFIRMATION}")
    parser.add_argument("--esptool", default="esptool.py")
    parser.add_argument("--request-timeout", type=float, default=15)
    parser.add_argument("--update-timeout", type=float, default=180)
    parser.add_argument("--boot-timeout", type=float, default=240)
    parser.add_argument("--stability-seconds", type=float, default=30)
    parser.add_argument(
        "--report", type=Path,
        default=Path("build-mvo/bzm-blank-bridge-recovery.json"))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    expected_mac = normalize_mac(args.expected_mac)
    if args.stability_seconds < 10:
        raise SystemExit("--stability-seconds must be at least 10")
    if args.request_timeout <= 0 or args.update_timeout <= 0 or args.boot_timeout <= 0:
        raise SystemExit("timeouts must be positive")
    if not args.bridge_firmware.is_file():
        raise SystemExit(
            f"bridge firmware does not exist: {args.bridge_firmware}")

    good_image = args.bridge_firmware.read_bytes()
    try:
        validate_bridge_image(good_image)
        good_manifest = parse_bridge_manifest(good_image)
        if good_manifest["version"] != args.expected_bridge_version:
            raise RegressionError(
                f"bridge manifest version {good_manifest['version']!r} "
                f"does not match {args.expected_bridge_version!r}")
        good_sha256 = sha256_bytes(good_image)
        if good_sha256.lower() != args.expected_bridge_sha256.lower():
            raise RegressionError(
                f"bridge firmware SHA-256 {good_sha256} does not match "
                f"{args.expected_bridge_sha256.lower()}")
        blank_image = make_nonresponsive_bridge_image(good_image)
        usb_mac = serial_device_mac(args.serial)
        if usb_mac != expected_mac:
            raise RegressionError(
                f"serial MAC {usb_mac} does not match {expected_mac}")
        if shutil.which(args.esptool) is None:
            raise RegressionError(
                f"{args.esptool!r} is unavailable; source ESP-IDF first")
        device = DeviceHttp(
            args.device, args.interface, args.request_timeout)
    except (OSError, ValueError, RegressionError) as exc:
        raise SystemExit(f"preflight failed: {exc}") from exc

    report: dict[str, Any] = {
        "device": args.device,
        "interface": args.interface,
        "serial": str(args.serial),
        "expectedMac": expected_mac,
        "expectedBridgeVersion": args.expected_bridge_version,
        "goodImage": {
            "path": str(args.bridge_firmware.resolve()),
            "size": len(good_image),
            "sha256": good_sha256,
            "manifest": good_manifest,
        },
        "nonresponsiveImage": {
            "size": len(blank_image),
            "sha256": sha256_bytes(blank_image),
        },
        "startedAt": time.time(),
        "passed": False,
    }
    mutation_started = False
    restored = False
    failure: Exception | None = None

    try:
        initial = wait_for_info(
            device, args.boot_timeout, expected_mac)
        validate_initial_mining(initial)
        validate_bridge_api(
            device, True, args.expected_bridge_version)
        report["initial"] = summarize_info(initial)
        print(
            f"[PASS] preflight: board 1002 BZM {expected_mac}, "
            f"bridge {args.expected_bridge_version}, mining 944/944",
            flush=True)

        mutation_started = True
        accepted = device.post_bridge_image(blank_image, force=True)
        report["blankAccepted"] = accepted
        print("[PASS] nonresponsive bridge image accepted", flush=True)
        blank_status = wait_for_update(device, args.update_timeout)
        report["blankUpdate"] = blank_status
        require_blank_update_result(blank_status)
        validate_bridge_api(device, False)
        before_reset = wait_for_info(
            device, args.boot_timeout, expected_mac)
        validate_blank_state(
            before_reset, require_pool_disconnected=False)
        report["beforeReset"] = summarize_info(before_reset)
        print("[PASS] bridge unavailable and mining stopped", flush=True)

        reset_esp_with_esptool(
            args.esptool, args.serial, expected_mac)
        booted_blank = wait_for_info(
            device, args.boot_timeout, expected_mac)
        validate_bridge_api(device, False)
        stable_blank = verify_blank_boot_stability(
            device, booted_blank, args.stability_seconds, expected_mac)
        report["blankBoot"] = summarize_info(stable_blank)
        print(
            f"[PASS] blank-bridge boot stayed up for "
            f"{args.stability_seconds:.0f}s with mining disabled",
            flush=True)

        good_status = restore_bridge(
            device, good_image, args.update_timeout,
            args.expected_bridge_version)
        if good_status is not None:
            report["goodUpdate"] = good_status
        restored = True
        _, restart = device.post_json("/api/system/restart")
        report["restart"] = restart
        time.sleep(3)
        final = wait_for_mining(
            device, args.boot_timeout, expected_mac,
            args.expected_bridge_version)
        validate_bridge_api(
            device, True, args.expected_bridge_version)
        report["final"] = summarize_info(final)
        report["passed"] = True
        print(
            f"[PASS] recovered bridge {args.expected_bridge_version}; "
            "device returned to MINING with 944/944 engines",
            flush=True)
    except Exception as exc:  # Cleanup must run for every post-mutation failure.
        failure = exc
        report["error"] = str(exc)
        print(f"[FAIL] {exc}", flush=True)

    if mutation_started and (failure is not None or not restored):
        try:
            wait_for_info(device, args.boot_timeout, expected_mac)
            restore_bridge(
                device, good_image, args.update_timeout,
                args.expected_bridge_version)
            restored = True
            try:
                device.post_json("/api/system/restart")
                time.sleep(3)
                cleanup_info = wait_for_mining(
                    device, args.boot_timeout, expected_mac,
                    args.expected_bridge_version)
                report["cleanupFinal"] = summarize_info(cleanup_info)
            except Exception as restart_error:
                report["cleanupRestartError"] = str(restart_error)
            print("[RECOVERY] cleanup restoration completed", flush=True)
        except Exception as cleanup_error:
            report["cleanupError"] = str(cleanup_error)
            print(
                f"[RECOVERY-FAIL] automatic restoration failed: "
                f"{cleanup_error}", flush=True)

    report["restored"] = restored
    report["finishedAt"] = time.time()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(f"Report: {args.report.resolve()}", flush=True)
    return 0 if failure is None and report["passed"] and restored else 1


if __name__ == "__main__":
    raise SystemExit(main())
