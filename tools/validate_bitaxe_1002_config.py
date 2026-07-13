#!/usr/bin/env python3

import csv
from pathlib import Path
import sys


EXPECTED = {
    "asicfrequency": ("u16", "50"),
    "asicvoltage": ("u16", "2800"),
    "asicmodel": ("string", "BZM"),
    "devicemodel": ("string", "bonanza"),
    "boardversion": ("string", "1002"),
    "fanspeed": ("u16", "100"),
}


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    path = root / "config-1002.cvs"
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

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

    print("Bitaxe 1002 factory configuration matches the runtime profile")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"Bitaxe 1002 factory configuration invalid: {error}",
              file=sys.stderr)
        sys.exit(1)
