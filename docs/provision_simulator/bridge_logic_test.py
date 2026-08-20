"""Focused safety checks for the local provisioning bridge."""

import json
import unittest
from types import SimpleNamespace
from unittest import mock

import bridge

from bridge import (
    BridgeHandler,
    DeviceIdentityMismatchError,
    GatewayControlError,
    assert_device_identity,
    classify_exception,
    esp_prov_proto,
    is_loopback_authority,
    map_device_outcome,
    normalize_wifi_networks,
    parse_wifi_status_message,
    set_test_gateway_audio_mode,
    set_test_gateway_volume,
    targets_test_gateway,
    validate_gateway_url,
    validate_protocol_info,
    validate_provision_payload,
    validate_reprovision_payload,
    validate_scan_payload,
    validate_audio_mode_payload,
    validate_volume_payload,
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
                "state": "pending",
                "deviceId": "NX-OTHER-DEVICE",
            }, "NX-A1B2-C3D4-E5F6")

    def test_vox_config_acknowledgement_accepts_pending_transaction(self) -> None:
        self.assertEqual(
            validate_vox_config_response({
                "ok": True,
                "state": "pending",
                "deviceId": "NX-A1B2-C3D4-E5F6",
            }, "NX-A1B2-C3D4-E5F6")["state"],
            "pending",
        )

    def test_vox_config_acknowledgement_rejects_premature_active_state(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            validate_vox_config_response({
                "ok": True,
                "state": "active",
                "deviceId": "NX-A1B2-C3D4-E5F6",
            }, "NX-A1B2-C3D4-E5F6")

    def test_vox_config_acknowledgement_must_not_echo_secret(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "must not echo"):
            validate_vox_config_response({
                "ok": True,
                "state": "pending",
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

    def test_volume_boundaries_are_accepted(self) -> None:
        for volume in (0, 100):
            with self.subTest(volume=volume):
                values = validate_volume_payload({
                    "deviceId": "NX-A1B2-C3D4-E5F6",
                    "gatewayUrl": "ws://127.0.0.1:8800/api/device/ws",
                    "volumePercent": volume,
                    "persist": False,
                })
                self.assertEqual(values["volumePercent"], volume)

    def test_invalid_volume_types_and_ranges_are_rejected(self) -> None:
        for volume in (-1, 101, 52.5, "52", True):
            with self.subTest(volume=volume), self.assertRaises(ValueError):
                validate_volume_payload({
                    "deviceId": "NX-A1B2-C3D4-E5F6",
                    "gatewayUrl": "ws://127.0.0.1:8800/api/device/ws",
                    "volumePercent": volume,
                    "persist": False,
                })

    def test_audio_mode_payload_requires_known_mode_and_boolean_state(self) -> None:
        valid = {
            "deviceId": "NX-A1B2-C3D4-E5F6",
            "gatewayUrl": "ws://127.0.0.1:8800/api/device/ws",
            "mode": "meeting",
            "enabled": True,
        }
        self.assertEqual(validate_audio_mode_payload(valid)["mode"], "meeting")
        for mode in ("conversation", "MEETING", "", None):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                validate_audio_mode_payload({**valid, "mode": mode})
        for enabled in (1, 0, "true", None):
            with self.subTest(enabled=enabled), self.assertRaises(ValueError):
                validate_audio_mode_payload({**valid, "enabled": enabled})


class TestGatewayVolumeTests(unittest.TestCase):
    DEVICE_ID = "NX-A1B2-C3D4-E5F6"
    GATEWAY_URL = "ws://127.0.0.1:8800/api/device/ws"

    def setUp(self) -> None:
        self.config = mock.patch.multiple(
            bridge,
            TEST_GATEWAY_HTTP_URL="http://127.0.0.1:8800",
            TEST_GATEWAY_WS_URL=self.GATEWAY_URL,
            TEST_GATEWAY_TOKEN="unit-test-token",
        )
        self.config.start()

    def tearDown(self) -> None:
        self.config.stop()

    def _values(self, volume: int = 52, persist: bool = False) -> dict:
        return {
            "deviceId": self.DEVICE_ID,
            "gatewayUrl": self.GATEWAY_URL,
            "volumePercent": volume,
            "persist": persist,
        }

    def _online(self, last_ack: dict | None = None, mode: str = "") -> dict:
        device = {
            "device_id": self.DEVICE_ID,
            "lastAck": last_ack,
            "currentUplinkAudioSession": {"mode": mode} if mode else None,
        }
        return {"devices": [device]}

    def test_only_exact_test_gateway_websocket_target_is_accepted(self) -> None:
        self.assertTrue(targets_test_gateway(self.GATEWAY_URL))
        self.assertFalse(targets_test_gateway("wss://testagent.xspaceagi.com/api/device/ws"))
        self.assertFalse(targets_test_gateway("ws://127.0.0.1:8800/other"))

    def test_formal_gateway_is_blocked_before_any_gateway_request(self) -> None:
        values = self._values()
        values["gatewayUrl"] = "wss://testagent.xspaceagi.com/api/device/ws"
        with mock.patch.object(bridge, "_gateway_json") as gateway, \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_volume(values)
        self.assertEqual(raised.exception.code, "FORMAL_CLOUD_CONTROL_DISABLED")
        gateway.assert_not_called()

    def test_offline_device_is_rejected(self) -> None:
        with mock.patch.object(bridge, "_gateway_json", return_value={"devices": []}), \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_volume(self._values())
        self.assertEqual(raised.exception.code, "DEVICE_OFFLINE")

    def test_meeting_and_monitoring_sessions_are_rejected(self) -> None:
        for mode in ("meeting", "monitoring"):
            with self.subTest(mode=mode), \
                    mock.patch.object(bridge, "_gateway_json", return_value=self._online(mode=mode)), \
                    self.assertRaises(GatewayControlError) as raised:
                set_test_gateway_volume(self._values())
            self.assertEqual(raised.exception.code, "AUDIO_MODE_BUSY")

    def test_matching_device_ack_completes_volume_command(self) -> None:
        request_id = ""

        def gateway(method: str, path: str, payload: dict | None = None) -> dict:
            nonlocal request_id
            if method == "POST":
                request_id = str(payload["request_id"])
                return {"ok": True, "device_id": self.DEVICE_ID, "command": dict(payload)}
            if request_id:
                return self._online({
                    "request_id": request_id,
                    "command": "set_volume",
                    "ok": True,
                    "message": "volume=52 persist=0",
                })
            return self._online()

        with mock.patch.object(bridge, "_gateway_json", side_effect=gateway):
            result = set_test_gateway_volume(self._values(), poll_seconds=0)
        self.assertTrue(result["ok"])
        self.assertEqual(result["volumePercent"], 52)
        self.assertEqual(result["ack"]["command"], "set_volume")
        self.assertNotIn("unit-test-token", json.dumps(result))
        self.assertNotIn("deviceSecret", json.dumps(result))

    def test_mismatched_ack_is_ignored_until_timeout(self) -> None:
        request_id = ""

        def gateway(method: str, path: str, payload: dict | None = None) -> dict:
            nonlocal request_id
            if method == "POST":
                request_id = str(payload["request_id"])
                return {"ok": True, "command": dict(payload)}
            return self._online({
                "request_id": "old_request",
                "command": "set_volume",
                "ok": True,
            })

        with mock.patch.object(bridge, "_gateway_json", side_effect=gateway), \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_volume(self._values(), ack_timeout_seconds=0, poll_seconds=0)
        self.assertTrue(request_id.startswith("volume_"))
        self.assertEqual(raised.exception.code, "VOLUME_ACK_TIMEOUT")

    def test_negative_device_ack_is_rejected(self) -> None:
        request_id = ""

        def gateway(method: str, path: str, payload: dict | None = None) -> dict:
            nonlocal request_id
            if method == "POST":
                request_id = str(payload["request_id"])
                return {"ok": True, "command": dict(payload)}
            if request_id:
                return self._online({
                    "request_id": request_id,
                    "command": "set_volume",
                    "ok": False,
                })
            return self._online()

        with mock.patch.object(bridge, "_gateway_json", side_effect=gateway), \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_volume(self._values(), poll_seconds=0)
        self.assertEqual(raised.exception.code, "VOLUME_REJECTED")


class TestGatewayAudioModeTests(unittest.TestCase):
    DEVICE_ID = "NX-A1B2-C3D4-E5F6"
    GATEWAY_URL = "ws://127.0.0.1:8800/api/device/ws"

    def setUp(self) -> None:
        self.config = mock.patch.multiple(
            bridge,
            TEST_GATEWAY_HTTP_URL="http://127.0.0.1:8800",
            TEST_GATEWAY_WS_URL=self.GATEWAY_URL,
            TEST_GATEWAY_TOKEN="unit-test-token",
        )
        self.config.start()

    def tearDown(self) -> None:
        self.config.stop()

    def _values(self, mode: str = "meeting", enabled: bool = True) -> dict:
        return {
            "deviceId": self.DEVICE_ID,
            "gatewayUrl": self.GATEWAY_URL,
            "mode": mode,
            "enabled": enabled,
        }

    def _online(self, *, last_ack: dict | None = None,
                active_mode: str = "", meeting: bool = True,
                monitoring: bool = True) -> dict:
        return {
            "devices": [{
                "device_id": self.DEVICE_ID,
                "features": {
                    "meeting_mode_v1": meeting,
                    "monitoring_mode_v1": monitoring,
                },
                "lastAck": last_ack,
                "currentUplinkAudioSession": (
                    {"mode": active_mode} if active_mode else None
                ),
            }],
        }

    def test_formal_gateway_is_blocked_before_request(self) -> None:
        values = self._values()
        values["gatewayUrl"] = "wss://testagent.xspaceagi.com/api/device/ws"
        with mock.patch.object(bridge, "_gateway_json") as gateway, \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_audio_mode(values)
        self.assertEqual(raised.exception.code, "FORMAL_CLOUD_CONTROL_DISABLED")
        gateway.assert_not_called()

    def test_offline_and_unsupported_devices_are_rejected(self) -> None:
        with mock.patch.object(bridge, "_gateway_json", return_value={"devices": []}), \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_audio_mode(self._values())
        self.assertEqual(raised.exception.code, "DEVICE_OFFLINE")

        with mock.patch.object(
                bridge, "_gateway_json",
                return_value=self._online(meeting=False)), \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_audio_mode(self._values())
        self.assertEqual(raised.exception.code, "AUDIO_MODE_UNSUPPORTED")

    def test_other_exclusive_mode_must_be_disabled_first(self) -> None:
        with mock.patch.object(
                bridge, "_gateway_json",
                return_value=self._online(active_mode="monitoring")), \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_audio_mode(self._values(mode="meeting"))
        self.assertEqual(raised.exception.code, "AUDIO_MODE_BUSY")

    def _successful_gateway(self, mode: str, enabled: bool,
                            ack_message: str):
        request_id = ""
        sent_command: dict = {}

        def gateway(method: str, path: str, payload: dict | None = None) -> dict:
            nonlocal request_id, sent_command
            if method == "POST":
                sent_command = dict(payload or {})
                request_id = str(sent_command["request_id"])
                return {"ok": True, "command": sent_command}
            if request_id:
                return self._online(
                    last_ack={
                        "request_id": request_id,
                        "command": "set_audio_mode",
                        "ok": True,
                        "message": ack_message,
                    },
                    active_mode=mode if enabled else "",
                )
            return self._online(active_mode=mode if not enabled else "")

        return gateway, lambda: sent_command

    def test_meeting_enable_preserves_existing_protocol_fields(self) -> None:
        gateway, command = self._successful_gateway("meeting", True, "active")
        with mock.patch.object(bridge, "_gateway_json", side_effect=gateway):
            result = set_test_gateway_audio_mode(self._values(), poll_seconds=0)
        self.assertEqual(command()["command"], "set_audio_mode")
        self.assertEqual(command()["mode"], "meeting")
        self.assertEqual(command()["state"], "enabled")
        self.assertIs(command()["interrupt_current_turn"], True)
        self.assertEqual(result["audioMode"], "meeting")
        self.assertNotIn("unit-test-token", json.dumps(result))
        self.assertNotIn("deviceSecret", json.dumps(result))

    def test_monitoring_enable_does_not_add_meeting_only_field(self) -> None:
        gateway, command = self._successful_gateway("monitoring", True, "already_active")
        with mock.patch.object(bridge, "_gateway_json", side_effect=gateway):
            result = set_test_gateway_audio_mode(
                self._values(mode="monitoring"), poll_seconds=0,
            )
        self.assertEqual(command()["state"], "enabled")
        self.assertNotIn("interrupt_current_turn", command())
        self.assertEqual(result["audioMode"], "monitoring")

    def test_disable_accepts_existing_success_messages(self) -> None:
        for message in ("disabled", "already_disabled"):
            with self.subTest(message=message):
                gateway, command = self._successful_gateway("meeting", False, message)
                with mock.patch.object(bridge, "_gateway_json", side_effect=gateway):
                    result = set_test_gateway_audio_mode(
                        self._values(enabled=False), poll_seconds=0,
                    )
                self.assertEqual(command()["state"], "disabled")
                self.assertIsNone(result["audioMode"])

    def test_busy_and_other_negative_acknowledgements_are_mapped(self) -> None:
        for message, expected in (
                ("audio_mode_busy", "AUDIO_MODE_BUSY"),
                ("meeting_busy", "AUDIO_MODE_BUSY"),
                ("invalid_state", "AUDIO_MODE_REJECTED")):
            request_id = ""

            def gateway(method: str, path: str, payload: dict | None = None) -> dict:
                nonlocal request_id
                if method == "POST":
                    request_id = str(payload["request_id"])
                    return {"ok": True, "command": dict(payload)}
                if request_id:
                    return self._online(last_ack={
                        "request_id": request_id,
                        "command": "set_audio_mode",
                        "ok": False,
                        "message": message,
                    })
                return self._online()

            with self.subTest(message=message), \
                    mock.patch.object(bridge, "_gateway_json", side_effect=gateway), \
                    self.assertRaises(GatewayControlError) as raised:
                set_test_gateway_audio_mode(self._values(), poll_seconds=0)
            self.assertEqual(raised.exception.code, expected)

    def test_mismatched_ack_times_out(self) -> None:
        def gateway(method: str, path: str, payload: dict | None = None) -> dict:
            if method == "POST":
                return {"ok": True, "command": dict(payload)}
            return self._online(last_ack={
                "request_id": "old_request",
                "command": "set_audio_mode",
                "ok": True,
                "message": "active",
            })

        with mock.patch.object(bridge, "_gateway_json", side_effect=gateway), \
                self.assertRaises(GatewayControlError) as raised:
            set_test_gateway_audio_mode(
                self._values(), ack_timeout_seconds=0, poll_seconds=0,
            )
        self.assertEqual(raised.exception.code, "AUDIO_MODE_ACK_TIMEOUT")


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
