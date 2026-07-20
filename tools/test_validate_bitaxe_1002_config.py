import csv
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from validate_bitaxe_1002_config import (
    EXPECTED,
    EXPECTED_PRODUCTION_DEFAULTS,
    validate,
    validate_production_defaults,
)


class Bitaxe1002ConfigValidationTest(unittest.TestCase):
    def write_rows(self, rows, header=("key", "type", "encoding", "value")):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "config.cvs"
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(header)
            writer.writerows(rows)
        return path

    @staticmethod
    def good_rows():
        return [
            (key, "data", encoding, value)
            for key, (encoding, value) in EXPECTED.items()
        ]

    def test_accepts_the_fail_closed_factory_profile(self):
        validate(self.write_rows(self.good_rows()))

    def test_rejects_powered_factory_self_test(self):
        rows = self.good_rows()
        rows = [
            (key, kind, encoding, "1" if key == "selftest" else value)
            for key, kind, encoding, value in rows
        ]
        with self.assertRaisesRegex(ValueError, "selftest"):
            validate(self.write_rows(rows))

    def test_rejects_duplicate_keys(self):
        rows = self.good_rows()
        rows.append(rows[0])
        with self.assertRaisesRegex(ValueError, "duplicate factory key"):
            validate(self.write_rows(rows))

    def test_rejects_missing_required_values(self):
        rows = [row for row in self.good_rows() if row[0] != "boardversion"]
        with self.assertRaisesRegex(ValueError, "boardversion"):
            validate(self.write_rows(rows))

    def test_rejects_an_unexpected_header(self):
        with self.assertRaisesRegex(ValueError, "unexpected factory CSV header"):
            validate(self.write_rows(self.good_rows(), header=("value", "key")))

    def test_rejects_an_empty_file(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "empty.cvs"
        path.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "no data rows"):
            validate(path)


class Bitaxe1002ProductionDefaultsValidationTest(unittest.TestCase):
    def write_defaults(self, overrides=None, omitted=()):
        overrides = overrides or {}
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "sdkconfig.defaults"
        lines = []
        for key, expected in EXPECTED_PRODUCTION_DEFAULTS.items():
            if key in omitted:
                continue
            value = overrides.get(key, expected)
            lines.append(
                f"# {key} is not set" if value is None else f"{key}={value}"
            )
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def test_accepts_explicit_production_sdkconfig_defaults(self):
        validate_production_defaults(self.write_defaults())

    def test_rejects_a_non_mining_default_image(self):
        path = self.write_defaults({
            "CONFIG_BZM_1002_MAX_STAGE": "0",
            "CONFIG_BZM_1002_POWERED_VALIDATION": None,
        })
        with self.assertRaisesRegex(ValueError, "MAX_STAGE.*POWERED_VALIDATION"):
            validate_production_defaults(path)

    def test_rejects_uart_console_on_the_bonanza_bridge_pins(self):
        path = self.write_defaults({
            "CONFIG_ESP_CONSOLE_UART_DEFAULT": "y",
            "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": None,
            "CONFIG_ESP_CONSOLE_SECONDARY_NONE": None,
            "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG": "y",
        })
        with self.assertRaisesRegex(
            ValueError,
            "ESP_CONSOLE_UART_DEFAULT.*ESP_CONSOLE_USB_SERIAL_JTAG",
        ):
            validate_production_defaults(path)

    def test_rejects_an_enabled_lab_escape_hatch(self):
        path = self.write_defaults({
            "CONFIG_BZM_1002_ALLOW_ESP_ONLY_KILL_IN_LAB": "y",
        })
        with self.assertRaisesRegex(ValueError, "ALLOW_ESP_ONLY"):
            validate_production_defaults(path)

    def test_requires_board_managed_fixed_voltage_safety(self):
        path = self.write_defaults({
            "CONFIG_BZM_1002_BOARD_MANAGED_SAFETY": None,
        })
        with self.assertRaisesRegex(ValueError, "BOARD_MANAGED_SAFETY"):
            validate_production_defaults(path)

    def test_rejects_an_enabled_usb_serial_arm(self):
        path = self.write_defaults({
            "CONFIG_BZM_1002_USB_SERIAL_ARM": "y",
        })
        with self.assertRaisesRegex(ValueError, "USB_SERIAL_ARM"):
            validate_production_defaults(path)

    def test_requires_the_qualified_engine_ramp(self):
        path = self.write_defaults({
            "CONFIG_BZM_1002_STAGE6_BALANCED_RAMP": None,
        })
        with self.assertRaisesRegex(ValueError, "STAGE6_BALANCED_RAMP"):
            validate_production_defaults(path)

    def test_requires_automatic_mining_support(self):
        path = self.write_defaults({
            "CONFIG_BZM_1002_STAGE7_MINING": None,
        })
        with self.assertRaisesRegex(ValueError, "STAGE7_MINING"):
            validate_production_defaults(path)

    def test_rejects_relaxed_default_pll_lock_confirmation(self):
        path = self.write_defaults({
            "CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES": "3",
        })
        with self.assertRaisesRegex(ValueError, "PLL_LOCK_CONFIRM_SAMPLES"):
            validate_production_defaults(path)

    def test_rejects_a_missing_explicit_safety_default(self):
        path = self.write_defaults(
            omitted=("CONFIG_BZM_1002_POWERED_VALIDATION",)
        )
        with self.assertRaisesRegex(ValueError, "missing explicit production default"):
            validate_production_defaults(path)


if __name__ == "__main__":
    unittest.main()
