import binascii
import struct
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bzm_blank_bridge_recovery_regression import (
    BLANK_IMAGE_SIZE,
    BLANK_RESET_OFFSET,
    BRIDGE_MANIFEST_CRC_OFFSET,
    BRIDGE_MANIFEST_MAGIC,
    BRIDGE_MANIFEST_SIZE,
    FLASH_VECTOR_OFFSET,
    RegressionError,
    THUMB_BRANCH_TO_SELF,
    make_nonresponsive_bridge_image,
    normalize_mac,
    parse_udev_properties,
    parse_bridge_manifest,
    require_blank_update_result,
    require_good_update_result,
    validate_blank_state,
    validate_bridge_image,
    validate_device_identity,
    validate_final_mining,
)


def valid_image(size=0x200):
    image = bytearray(size)
    image[0] = 0x42
    struct.pack_into("<I", image, FLASH_VECTOR_OFFSET, 0x20042000)
    struct.pack_into("<I", image, FLASH_VECTOR_OFFSET + 4, 0x10000109)
    return bytes(image)


def manifest_image(version="0.0.1+g5ab20c2"):
    image = bytearray(valid_image(0x300))
    offset = 0x180
    manifest = bytearray(BRIDGE_MANIFEST_SIZE)
    manifest[:len(BRIDGE_MANIFEST_MAGIC)] = BRIDGE_MANIFEST_MAGIC
    manifest[16] = 1
    manifest[17] = BRIDGE_MANIFEST_SIZE
    struct.pack_into("<H", manifest, 18, 1002)
    manifest[20] = 1
    manifest[21] = 1
    manifest[22] = 0
    manifest[23] = len(version)
    manifest[24:24 + len(version)] = version.encode("ascii")
    struct.pack_into(
        "<I", manifest, BRIDGE_MANIFEST_CRC_OFFSET,
        binascii.crc32(
            manifest[:BRIDGE_MANIFEST_CRC_OFFSET]) & 0xFFFFFFFF)
    image[offset:offset + len(manifest)] = manifest
    return bytes(image)


def system_info(**overrides):
    info = {
        "boardVersion": "1002",
        "ASICModel": "BZM",
        "isPSRAMAvailable": 1,
        "macAddr": "10:B4:1D:E2:17:08",
        "miningPaused": False,
        "poolConnectionInfo": "IPv4",
        "asicHealth": {
            "lifecycle": "MINING",
            "asicCount": 4,
            "activeEngineCount": 944,
            "expectedEngineCount": 944,
            "bridgeVersion": "0.0.1+g5ab20c2",
            "lastFault": "",
        },
    }
    info.update(overrides)
    return info


class BridgeImageTest(unittest.TestCase):
    def test_nonresponsive_image_preserves_boot2_and_loops_at_reset(self):
        good = valid_image()
        blank = make_nonresponsive_bridge_image(good)

        self.assertEqual(BLANK_IMAGE_SIZE, len(blank))
        self.assertEqual(good[:FLASH_VECTOR_OFFSET],
                         blank[:FLASH_VECTOR_OFFSET])
        self.assertEqual(0x20042000,
                         struct.unpack_from("<I", blank, 0x100)[0])
        self.assertEqual(0x10000109,
                         struct.unpack_from("<I", blank, 0x104)[0])
        self.assertEqual(THUMB_BRANCH_TO_SELF,
                         blank[BLANK_RESET_OFFSET:])
        validate_bridge_image(blank)

    def test_rejects_erased_and_out_of_range_images(self):
        with self.assertRaisesRegex(RegressionError, "boot2"):
            validate_bridge_image(bytes([0xFF]) * 0x200)

        bad_reset = bytearray(valid_image())
        struct.pack_into("<I", bad_reset, 0x104, 0x10000301)
        with self.assertRaisesRegex(RegressionError, "outside the image"):
            validate_bridge_image(bytes(bad_reset))

    def test_bridge_manifest_is_unique_versioned_and_crc_protected(self):
        manifest = parse_bridge_manifest(manifest_image())
        self.assertEqual(0x180, manifest["offset"])
        self.assertEqual(1002, manifest["targetBoardVersion"])
        self.assertEqual(1, manifest["protocolMajor"])
        self.assertEqual(0, manifest["protocolMinor"])
        self.assertEqual("0.0.1+g5ab20c2", manifest["version"])

        corrupt = bytearray(manifest_image())
        corrupt[0x180 + BRIDGE_MANIFEST_CRC_OFFSET] ^= 1
        with self.assertRaisesRegex(RegressionError, "CRC"):
            parse_bridge_manifest(bytes(corrupt))
        with self.assertRaisesRegex(RegressionError, "no BZM"):
            parse_bridge_manifest(valid_image())


class IdentityAndStateTest(unittest.TestCase):
    def test_identity_requires_exact_board_asic_psram_and_mac(self):
        validate_device_identity(
            system_info(), "10-b4-1d-e2-17-08")

        for override, message in (
            ({"boardVersion": "999"}, "board 1002"),
            ({"ASICModel": "BM1370"}, "BZM ASIC"),
            ({"isPSRAMAvailable": 0}, "PSRAM"),
            ({"macAddr": "00:11:22:33:44:55"}, "does not match"),
        ):
            with self.subTest(override=override):
                with self.assertRaisesRegex(RegressionError, message):
                    validate_device_identity(
                        system_info(**override), "10:B4:1D:E2:17:08")

    def test_blank_state_requires_fault_pause_zero_engines_and_no_pool(self):
        info = system_info(
            miningPaused=True, poolConnectionInfo="Not Connected")
        info["asicHealth"] = {
            "lifecycle": "FAULT",
            "activeEngineCount": 0,
        }
        validate_blank_state(info)

        info["asicHealth"]["activeEngineCount"] = 1
        with self.assertRaisesRegex(RegressionError, "active engines"):
            validate_blank_state(info)

        info["asicHealth"]["activeEngineCount"] = 0
        info["poolConnectionInfo"] = "IPv4"
        with self.assertRaisesRegex(RegressionError, "pool state"):
            validate_blank_state(info)
        validate_blank_state(
            info, require_pool_disconnected=False)

    def test_final_state_requires_expected_bridge_and_no_fault(self):
        validate_final_mining(
            system_info(), "0.0.1+g5ab20c2")

        info = system_info()
        info["asicHealth"]["bridgeVersion"] = "wrong"
        with self.assertRaisesRegex(RegressionError, "final bridge version"):
            validate_final_mining(info, "0.0.1+g5ab20c2")


class UpdateStatusTest(unittest.TestCase):
    def test_blank_update_must_fail_only_after_programming(self):
        require_blank_update_result({
            "state": "failed",
            "progress": 100,
            "running": False,
            "error": "ESP_ERR_TIMEOUT",
            "manifestValidated": False,
            "forceRequested": True,
        })
        with self.assertRaisesRegex(RegressionError, "unexpected status"):
            require_blank_update_result({
                "state": "complete",
                "progress": 100,
                "running": False,
                "error": None,
                "manifestValidated": False,
                "forceRequested": True,
            })

    def test_good_update_requires_versioned_complete_status(self):
        status = {
            "state": "complete",
            "progress": 100,
            "running": False,
            "versionQuerySupported": True,
            "manifestValidated": True,
            "forceRequested": False,
            "imageVersion": "0.0.1+g5ab20c2",
            "currentVersion": "0.0.1+g5ab20c2",
            "error": None,
        }
        require_good_update_result(
            status, "0.0.1+g5ab20c2")
        status["currentVersion"] = "wrong"
        with self.assertRaisesRegex(RegressionError, "does not match"):
            require_good_update_result(
                status, "0.0.1+g5ab20c2")

    def test_mac_and_udev_parsing_are_stable(self):
        self.assertEqual(
            "10:B4:1D:E2:17:08",
            normalize_mac("10-b4-1d-e2-17-08"))
        self.assertEqual(
            {"ID_VENDOR_ID": "303a", "ID_SERIAL_SHORT": "10:B4"},
            parse_udev_properties(
                "ID_VENDOR_ID=303a\nID_SERIAL_SHORT=10:B4\n"))
        with self.assertRaisesRegex(RegressionError, "invalid MAC"):
            normalize_mac("GG:11:22:33:44:55")


if __name__ == "__main__":
    unittest.main()
