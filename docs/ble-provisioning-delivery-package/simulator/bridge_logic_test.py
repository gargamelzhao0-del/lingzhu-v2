"""Focused safety checks for the local provisioning bridge."""

import unittest

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
    validate_claim_bundle,
    validate_cloud_config_response,
    validate_protocol_info,
    validate_provision_payload,
    validate_scan_payload,
)


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


class CloudBundleTests(unittest.TestCase):
    @staticmethod
    def _bundle() -> dict:
        return {
            "version": 1,
            "deviceId": "NX-A1B2-C3D4-E5F6",
            "cloudUrl": "wss://gateway.invalid/api/device/ws",
            "credentialId": "cred_test_reference_01",
            "deviceSecret": "a" * 32,
            "bundleVersion": 1784954350746,
            "signature": "MEUCIQTestSignatureOnly",
        }

    def test_complete_claim_bundle_is_accepted_without_rewriting(self) -> None:
        bundle = self._bundle()
        self.assertIs(validate_claim_bundle(bundle, bundle["deviceId"]), bundle)

    def test_claim_bundle_requires_protocol_version(self) -> None:
        bundle = self._bundle()
        bundle.pop("version")
        with self.assertRaisesRegex(RuntimeError, "bundle version"):
            validate_claim_bundle(bundle, "NX-A1B2-C3D4-E5F6")

    def test_claim_bundle_identity_mismatch_is_blocked(self) -> None:
        with self.assertRaises(DeviceIdentityMismatchError):
            validate_claim_bundle(self._bundle(), "NX-0000-0000-0000")

    def test_cloud_config_acknowledgement_must_match_claim(self) -> None:
        bundle = self._bundle()
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            validate_cloud_config_response({
                "ok": True,
                "state": "pending",
                "credentialId": bundle["credentialId"],
                "bundleVersion": bundle["bundleVersion"] + 1,
            }, bundle)


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

    def test_device_extension_exposes_cloud_precondition_failure(self) -> None:
        self.assertEqual(
            map_device_outcome("cloud_config_required", "failed"),
            "CLOUD_CONFIG_REQUIRED",
        )


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


class RequestBoundaryTests(unittest.TestCase):
    def test_only_loopback_host_authorities_are_accepted(self) -> None:
        self.assertTrue(is_loopback_authority("127.0.0.1:8765"))
        self.assertTrue(is_loopback_authority("localhost:8765"))
        self.assertFalse(is_loopback_authority("example.com:8765"))
        self.assertFalse(is_loopback_authority("127.0.0.1.example.com:8765"))

    def test_protocol_field_size_limits_are_enforced(self) -> None:
        with self.assertRaises(ValueError):
            validate_provision_payload({
                "serviceName": "PROV_A1B2C3",
                "username": "NX-A1B2-C3D4-E5F6",
                "pop": "development-secret",
                "ssid": "x" * 33,
                "password": "valid-password",
            })
        with self.assertRaises(ValueError):
            validate_provision_payload({
                "serviceName": "PROV_A1B2C3",
                "username": "NX-A1B2-C3D4-E5F6",
                "pop": "development-secret",
                "ssid": "valid-ssid",
                "password": "x" * 65,
            })


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
