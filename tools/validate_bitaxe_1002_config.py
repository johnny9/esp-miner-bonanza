#!/usr/bin/env python3

import csv
from pathlib import Path
import sys
from typing import Iterable


EXPECTED = {
    "asicfrequency": ("u16", "800"),
    "asicvoltage": ("u16", "2800"),
    "asicmodel": ("string", "BZM"),
    "devicemodel": ("string", "bonanza"),
    "boardversion": ("string", "1002"),
    "fanspeed": ("u16", "100"),
    "selftest": ("u16", "0"),
}

EXPECTED_PRODUCTION_DEFAULTS = {
    "CONFIG_ESP_CONSOLE_UART_DEFAULT": None,
    "CONFIG_ESP_CONSOLE_USB_CDC": None,
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": "y",
    "CONFIG_ESP_CONSOLE_UART_CUSTOM": None,
    "CONFIG_ESP_CONSOLE_NONE": None,
    "CONFIG_ESP_CONSOLE_SECONDARY_NONE": "y",
    "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG": None,
    "CONFIG_BZM_1002_PROOF_TIMEOUT_SECONDS": "90",
    "CONFIG_BZM_1002_RESULT_RECOVERY_TIMEOUT_MS": "5000",
    "CONFIG_BZM_1002_MIN_VALID_RESULTS": "1",
    "CONFIG_BZM_1002_MAX_LOCAL_REJECTIONS": "16",
    "CONFIG_BZM_1002_MAX_MAPPING_REJECTIONS": "16",
    "CONFIG_BZM_1002_MIN_NONCE_DIFFICULTY": "1",
    "CONFIG_BZM_1002_LEAD_ZEROS": "36",
    "CONFIG_BZM_1002_PARSER_REALIGN_MAX_DISCARDS": "256",
    "CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS": "2",
    "CONFIG_BZM_1002_PARSER_REALIGN_MAX_WINDOWS": "20",
    "CONFIG_BZM_1002_PARSER_REALIGN_MAX_EVENTS": "20",
    "CONFIG_BZM_1002_SAFE_OFF_VCORE_MV": "250",
    "CONFIG_BZM_1002_TELEMETRY_MAX_AGE_MS": "2000",
    "CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT": "0",
    "CONFIG_BZM_1002_TEMP_MIN_C": "-20",
    "CONFIG_BZM_1002_TEMP_MAX_C": "105",
    "CONFIG_BZM_1002_STACK_MV_MIN": "300",
    "CONFIG_BZM_1002_STACK_MV_MAX": "800",
    "CONFIG_BZM_1002_INTERSTACK_DIFF_ABS_MAX_MV": "50",
    "CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES": "3",
    "CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES": "3",
    "CONFIG_BZM_1002_STACK_MAX_SPREAD_MV": "100",
}

FORBIDDEN_QUALIFICATION_DEFAULTS = {
    "CONFIG_BZM_1002_MAX_STAGE",
    "CONFIG_BZM_1002_POWERED_VALIDATION",
    "CONFIG_BZM_1002_LAB_VALIDATION",
    "CONFIG_BZM_1002_ALLOW_ESP_ONLY_KILL_IN_LAB",
    "CONFIG_BZM_1002_BOARD_MANAGED_SAFETY",
    "CONFIG_BZM_1002_USB_SERIAL_ARM",
    "CONFIG_BZM_1002_USB_SERIAL_ARM_WINDOW_SECONDS",
    "CONFIG_BZM_1002_STAGE6_BALANCED_RAMP",
    "CONFIG_BZM_1002_MINING",
    "CONFIG_BZM_1002_ALLOW_MAPPING_RECOVERY",
    "CONFIG_BZM_1002_ALLOW_PARSER_REALIGN",
    "CONFIG_BZM_1002_MAX_LEASE_SECONDS",
    "CONFIG_BZM_BRIDGE_UPDATE_EXPERIMENTAL",
}


def validate(path: Path) -> None:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

    if not rows:
        raise ValueError("factory CSV has no data rows")
    if list(rows[0].keys()) != ["key", "type", "encoding", "value"]:
        raise ValueError("unexpected factory CSV header")

    values = {}
    for row in rows:
        key = row["key"]
        if key in values:
            raise ValueError(f"duplicate factory key: {key}")
        values[key] = (row["encoding"], row["value"])

    errors = []
    for key, expected in EXPECTED.items():
        if values.get(key) != expected:
            errors.append(f"{key}: expected {expected}, got {values.get(key)}")
    if errors:
        raise ValueError("; ".join(errors))


def validate_production_defaults(path: Path) -> None:
    values: dict[str, str | None] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            key = line[2:-len(" is not set")]
            value = None
        else:
            continue
        if key in values:
            raise ValueError(f"duplicate sdkconfig default: {key}")
        values[key] = value

    errors = []
    for key in sorted(FORBIDDEN_QUALIFICATION_DEFAULTS & values.keys()):
        errors.append(f"{key}: qualification-only option is forbidden")
    for key, expected in EXPECTED_PRODUCTION_DEFAULTS.items():
        if key not in values:
            errors.append(f"{key}: missing explicit production default")
        elif values[key] != expected:
            rendered = "not set" if expected is None else expected
            errors.append(
                f"{key}: expected {rendered}, got {values[key]!r}"
            )
    if errors:
        raise ValueError("; ".join(errors))


def main(argv: Iterable[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) > 2:
        raise ValueError(
            "usage: validate_bitaxe_1002_config.py "
            "[config.cvs] [sdkconfig.defaults]"
        )
    root = Path(__file__).resolve().parents[1]
    path = Path(args[0]) if args else root / "config-1002.cvs"
    defaults_path = (
        Path(args[1]) if len(args) == 2 else root / "sdkconfig.defaults"
    )
    validate(path)
    validate_production_defaults(defaults_path)

    print(
        "Bitaxe 1002 factory configuration and sdkconfig defaults "
        "match the automatic fixed-profile production configuration"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"Bitaxe 1002 factory configuration invalid: {error}",
              file=sys.stderr)
        sys.exit(1)
