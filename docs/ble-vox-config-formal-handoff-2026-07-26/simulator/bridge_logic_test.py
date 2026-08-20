"""Focused safety checks for the local provisioning bridge."""

import unittest
from types import SimpleNamespace
from unittest import mock

import bridge

from bridge import (
    BridgeHandler,
    DeviceIdentityMismatchError,
    assert_device_identity,
    classify_exception,
    esp_prov_proto,
    is_loopback_authority,
    map_device_outcome,
    normalize_wifi_networks,
    parse_wifi_status_message,
    validate_gateway_url,
    validate_protocol_info,
    validate_provision_payload,
    validate_reprovision_payload,
    validate_scan_payload,
    validate_vox_config_response,
)


class UsbSerialSelectionTests(unittest.TestCase):
    def _ports(self, *entries: tuple[str, int | None]) -> list[SimpleNamespace]:
        return [SimpleNamespace(device=device, vid=vid) for device, vid in entries]

    def test_single_esp_usb_port_is_selected_by_vid(self) -> None:
        ports = self._ports(("COM4", 0x10C4), ("COM8", 0x303A))
        with mock.patch.object(bridge, "DEVICE_SERIAL_PORT", ""), \
                mock.patch.object(bridge.serial.tools.list_ports, "comports", return_value=ports):
            self.assertEqual(bridge.resolve_device_serial_port(), "COM8")

    def test_unrelated_single_serial_port_is_not_selected(self) -> None:
        ports = self._ports(("COM4", 0x10C4))
        with mock.patch.object(bridge, "DEVICE_SERIAL_PORT", ""), \
                mock.patch.object(bridge.serial.tools.list_ports, "comports", return_value=ports):
            self.assertIsNone(bridge.resolve_device_serial_port())

    def test_ambiguous_esp_usb_ports_require_override(self) -> None:
        ports = self._ports(("COM8", 0x303A), ("COM9", 0x303A))
        with mock.patch.object(bridge, "DEVICE_SERIAL_PORT", ""), \
                mock.patch.object(bridge.serial.tools.list_ports, "comports", return_value=ports):
            self.assertIsNone(bridge.resolve_device_serial_port())

    def test_explicit_serial_override_selects_exact_port(self) -> None:
        ports = self._ports(("COM8", 0x303A), ("COM9", 0x303A))
        with mock.patch.object(bridge, "DEVICE_SERIAL_PORT", "com9"), \
                mock.patch.object(bridge.serial.tools.list_ports, "comports", return_value=ports):
            self.assertEqual(bridge.resolve_device_serial_port(), "COM9")


class ProtocolInfoTests(unittest.TestCase):
    def test_official_and_application_capabilities_are_separate(self) -> None:
        validate_protocol_info({
            "prov": {"sec_ver": 2, "cap": ["wifi_prov", "wifi_scan"]},
            "nuwax": {"ver": "7.0.0", "cap": ["cloud_prov"]},
        })

    def test_cloud_capability_must_be_in_nuwax_app_info(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "nuwax.cap:cloud_prov"):
            validate_protocol_info({
                "prov": {"sec_ver": 2, "cap": ["wifi_prov", "wifi_scan", "cloud_prov"]},
                "nuwax": {"ver": "7.0.0", "cap": []},
            })


class VoxConfigTests(unittest.TestCase):
    def test_gateway_url_accepts_websocket_schemes(self) -> None:
        self.assertEqual(
            validate_gateway_url("wss://gateway.invalid/api/device/ws"),
            "wss://gateway.invalid/api/device/ws",
        )

    def test_gateway_url_rejects_embedded_credentials(self) -> None:
        with self.assertRaisesRegex(ValueError, "without credentials"):
            validate_gateway_url("wss://user:secret@gateway.invalid/api/device/ws")

    def test_gateway_url_rejects_fragment_ipv6_and_non_ascii(self) -> None:
        for value in (
            "wss://gateway.invalid/api/device/ws#fragment",
            "wss://[::1]/api/device/ws",
            "wss://gateway.invalid/api/设备/ws",
        ):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_gateway_url(value)

    def test_gateway_url_rejects_invalid_port_and_host_boundary(self) -> None:
        for value in (
            "wss://gateway.invalid:0/api/device/ws",
            "wss://gateway.invalid:65536/api/device/ws",
            "wss://.gateway.invalid/api/device/ws",
            "wss://gateway.invalid./api/device/ws",
        ):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_gateway_url(value)

    def test_vox_config_acknowledgement_must_match_device(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            validate_vox_config_response({
                "ok": True,
                "state": "active",
                "deviceId": "NX-OTHER-DEVICE",
            }, "NX-A1B2-C3D4-E5F6")

    def test_vox_config_acknowledgement_must_not_echo_secret(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "must not echo"):
            validate_vox_config_response({
                "ok": True,
                "state": "active",
                "deviceId": "NX-A1B2-C3D4-E5F6",
                "deviceSecret": "not-allowed",
            }, "NX-A1B2-C3D4-E5F6")


class DeviceIdentityTests(unittest.TestCase):
    def test_matching_serial_number_is_accepted(self) -> None:
        assert_device_identity(
            {"serialNumber": "NX-A1B2-C3D4-E5F6"},
            "NX-A1B2-C3D4-E5F6",
        )

    def test_mismatch_is_blocked_and_mapped(self) -> None:
        with self.assertRaises(DeviceIdentityMismatchError) as raised:
            assert_device_identity(
                {"serialNumber": "NX-OTHER-DEVICE"},
                "NX-A1B2-C3D4-E5F6",
            )
        self.assertEqual(classify_exception(raised.exception), "DEVICE_ID_MISMATCH")


class WifiStatusMappingTests(unittest.TestCase):
    @staticmethod
    def _response(state: int, failure_reason: int | None = None) -> bytes:
        payload = esp_prov_proto.network_config_pb2.NetworkConfigPayload()
        payload.msg = esp_prov_proto.network_config_pb2.TypeRespGetWifiStatus
        payload.resp_get_wifi_status.status = 0
        payload.resp_get_wifi_status.wifi_sta_state = state
        if failure_reason is not None:
            payload.resp_get_wifi_status.wifi_fail_reason = failure_reason
        return payload.SerializeToString()

    def test_connected_status(self) -> None:
        self.assertEqual(
            parse_wifi_status_message(self._response(0)),
            {"state": "connected"},
        )

    def test_auth_failure_is_not_internal_error(self) -> None:
        status = parse_wifi_status_message(self._response(3, 0))
        self.assertEqual(status["failureReason"], "auth_failed")
        self.assertEqual(status["outcome"], "WIFI_AUTH_FAILED")

    def test_missing_network_is_mapped(self) -> None:
        status = parse_wifi_status_message(self._response(3, 1))
        self.assertEqual(status["failureReason"], "network_not_found")
        self.assertEqual(status["outcome"], "NETWORK_NOT_FOUND")

    def test_unmapped_device_failure_is_internal_error(self) -> None:
        self.assertEqual(map_device_outcome("unknown", "failed"), "INTERNAL_ERROR")


class WifiScanResultTests(unittest.TestCase):
    def test_results_are_deduplicated_sorted_and_sanitized(self) -> None:
        networks = normalize_wifi_networks([
            {"ssid": "Office", "rssi": -70, "channel": 1, "auth": "WPA2_PSK"},
            {"ssid": "Office", "rssi": -42, "channel": 6, "auth": "WPA2_PSK"},
            {"ssid": "Guest", "rssi": -55, "channel": 11, "auth": "Open"},
            {"ssid": "", "rssi": -20, "channel": 3, "auth": "WPA2_PSK"},
        ])
        self.assertEqual([item["ssid"] for item in networks], ["Office", "Guest"])
        self.assertEqual(networks[0]["rssi"], -42)
        self.assertNotIn("bssid", networks[0])

    def test_utf8_ssid_is_restored_from_component_latin1_decode(self) -> None:
        raw_name = "测试网络".encode("utf-8").decode("latin-1")
        networks = normalize_wifi_networks([
            {"ssid": raw_name, "rssi": -48, "channel": 6, "auth": "WPA2_PSK"},
        ])
        self.assertEqual(networks[0]["ssid"], "测试网络")


class InputValidationTests(unittest.TestCase):
    def test_service_name_must_match_firmware_format(self) -> None:
        with self.assertRaises(ValueError):
            validate_scan_payload({
                "serviceName": "PROV_not-a-device",
                "username": "NX-A1B2-C3D4-E5F6",
                "pop": "development-secret",
            })

    def test_remote_reprovision_requires_a_bounded_device_id(self) -> None:
        self.assertEqual(
            validate_reprovision_payload({
                "deviceId": "NX-A1B2-C3D4-E5F6",
                "serviceName": "PROV_A1B2C3",
            }),
            {
                "deviceId": "NX-A1B2-C3D4-E5F6",
                "serviceName": "PROV_A1B2C3",
            },
        )
        with self.assertRaises(ValueError):
            validate_reprovision_payload({
                "deviceId": "NX bad id",
                "serviceName": "PROV_A1B2C3",
            })
        with self.assertRaises(ValueError):
            validate_reprovision_payload({
                "deviceId": "NX-A1B2-C3D4-E5F6",
                "serviceName": "PROV_wrong",
            })


class RequestBoundaryTests(unittest.TestCase):
    @staticmethod
    def _payload() -> dict:
        return {
            "serviceName": "PROV_A1B2C3",
            "username": "NX-A1B2-C3D4-E5F6",
            "pop": "development-secret",
            "gatewayUrl": "wss://gateway.invalid/api/device/ws",
            "deviceSecret": "a" * 32,
            "ssid": "valid-ssid",
            "password": "valid-password",
        }

    def test_only_loopback_host_authorities_are_accepted(self) -> None:
        self.assertTrue(is_loopback_authority("127.0.0.1:8765"))
        self.assertTrue(is_loopback_authority("localhost:8765"))
        self.assertFalse(is_loopback_authority("example.com:8765"))
        self.assertFalse(is_loopback_authority("127.0.0.1.example.com:8765"))

    def test_protocol_field_size_limits_are_enforced(self) -> None:
        payload = self._payload()
        payload["ssid"] = "x" * 33
        with self.assertRaises(ValueError):
            validate_provision_payload(payload)
        payload = self._payload()
        payload["password"] = "x" * 65
        with self.assertRaises(ValueError):
            validate_provision_payload(payload)

    def test_device_secret_rejects_whitespace(self) -> None:
        payload = self._payload()
        payload["deviceSecret"] = "secret with space"
        with self.assertRaisesRegex(ValueError, "printable ASCII"):
            validate_provision_payload(payload)

    def test_device_secret_rejects_non_ascii(self) -> None:
        payload = self._payload()
        payload["deviceSecret"] = "测试密钥"
        with self.assertRaisesRegex(ValueError, "printable ASCII"):
            validate_provision_payload(payload)


class StaticServingTests(unittest.TestCase):
    def test_static_handler_cannot_escape_simulator_directory(self) -> None:
        handler = BridgeHandler.__new__(BridgeHandler)
        safe = handler.translate_path("/docs/provision_simulator/app.js")
        packaged = handler.translate_path("/simulator/app.js")
        private = handler.translate_path("/sdkconfig")
        traversal = handler.translate_path("/docs/provision_simulator/../../sdkconfig")
        self.assertTrue(safe.endswith("simulator\\app.js"))
        self.assertEqual(packaged, safe)
        self.assertTrue(private.endswith("simulator\\__not_found__"))
        self.assertTrue(traversal.endswith("simulator\\__not_found__"))


if __name__ == "__main__":
    unittest.main()
