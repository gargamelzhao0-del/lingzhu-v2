#!/usr/bin/env python3
"""Local BLE provisioning bridge for the browser-based integration client.

The protocol implementation is imported from the exact managed
espressif/network_provisioning component used by this firmware build.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
import threading
import time
import uuid
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse
from urllib.error import HTTPError, URLError
from urllib.request import ProxyHandler, Request, build_opener


SIMULATOR_DIR = Path(__file__).resolve().parent


def _find_project_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        contract = candidate / "main" / "nuwax_provision_new" / "esp32s3-idf6-provisioning-contract.json"
        if contract.is_file():
            return candidate
    raise RuntimeError("Cannot locate the NuwaxAIPartner project root")


ROOT = _find_project_root(SIMULATOR_DIR)
PACKAGED_CONTRACT = SIMULATOR_DIR.parent / "provisioning-contract.json"
CONTRACT_PATH = Path(os.environ.get(
    "NUWAX_PROVISION_CONTRACT",
    str(PACKAGED_CONTRACT if PACKAGED_CONTRACT.is_file() else (
        ROOT / "main" / "nuwax_provision_new" / "esp32s3-idf6-provisioning-contract.json"
    )),
))
COMPONENT_DIR = ROOT / "managed_components" / "espressif__network_provisioning"
ESP_PROV_TOOL = COMPONENT_DIR / "tool" / "esp_prov"
PROJECT_DESCRIPTION = ROOT / "build" / "project_description.json"
STATIC_PREFIXES = (
    "/docs/ble-dynamic-cloud-binding-delivery-package/simulator",
    "/docs/provision_simulator",
    "/simulator",
)
MAX_RETAINED_JOBS = 64
BLE_OPERATION_TIMEOUT_SECONDS = 90
PROVISION_OPERATION_TIMEOUT_SECONDS = 240
TEST_GATEWAY_HTTP_URL = os.environ.get("NUWAX_TEST_GATEWAY_HTTP_URL", "").rstrip("/")
TEST_GATEWAY_CLAIM_TOKEN = os.environ.get("NUWAX_TEST_GATEWAY_CLAIM_TOKEN", "")
CLOUD_ONLINE_TIMEOUT_SECONDS = int(os.environ.get("NUWAX_CLOUD_ONLINE_TIMEOUT_SECONDS", "90"))
HTTP = build_opener(ProxyHandler({}))


def _validate_text(payload: dict[str, Any], key: str, max_bytes: int) -> str:
    value = payload.get(key)
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError(f"{key} is required")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{key} is not valid UTF-8") from exc
    if len(encoded) > max_bytes:
        raise ValueError(f"{key} exceeds the protocol size limit")
    return value


def validate_scan_payload(payload: dict[str, Any]) -> dict[str, str]:
    service_name = _validate_text(payload, "serviceName", 15)
    if re.fullmatch(r"PROV_[0-9A-F]{6}", service_name) is None:
        raise ValueError("serviceName must match PROV_ followed by six uppercase hex digits")
    return {
        "serviceName": service_name,
        "username": _validate_text(payload, "username", 128),
        "pop": _validate_text(payload, "pop", 128),
    }


def validate_provision_payload(payload: dict[str, Any]) -> dict[str, str]:
    values = validate_scan_payload(payload)
    values["ssid"] = _validate_text(payload, "ssid", 32)
    values["password"] = _validate_text(payload, "password", 64)
    return values


def validate_protocol_info(protocol_info: dict[str, Any]) -> None:
    """Validate the official provisioning and Nuwax application capabilities."""
    prov_info = protocol_info.get("prov", {})
    app_info = protocol_info.get("nuwax", {})
    try:
        security_version = int(prov_info.get("sec_ver", -1))
    except (TypeError, ValueError) as exc:
        raise RuntimeError("Device proto-ver has an invalid Security version") from exc
    if security_version != 2:
        raise RuntimeError("Device does not advertise Security 2")

    prov_caps = set(prov_info.get("cap", []))
    app_caps = set(app_info.get("cap", []))
    missing = []
    if "wifi_prov" not in prov_caps:
        missing.append("prov.cap:wifi_prov")
    if "wifi_scan" not in prov_caps:
        missing.append("prov.cap:wifi_scan")
    if "cloud_prov" not in app_caps:
        missing.append("nuwax.cap:cloud_prov")
    if missing:
        raise RuntimeError("Required proto-ver capability is missing: " + ", ".join(missing))


def validate_claim_bundle(bundle: Any, expected_device_id: str) -> dict[str, Any]:
    """Validate the cloud response without changing any signed field value."""
    if not isinstance(bundle, dict):
        raise RuntimeError("Test gateway returned an invalid cloud bundle")
    version = bundle.get("version")
    bundle_version = bundle.get("bundleVersion")
    if isinstance(version, bool) or version != 1:
        raise RuntimeError("Test gateway returned an invalid cloud bundle version")
    if (isinstance(bundle_version, bool) or not isinstance(bundle_version, int)
            or not 1 <= bundle_version <= 9007199254740991):
        raise RuntimeError("Test gateway returned an invalid bundleVersion")
    limits = {
        "deviceId": 39,
        "cloudUrl": 191,
        "credentialId": 63,
        "deviceSecret": 95,
        "signature": 111,
    }
    for field, limit in limits.items():
        try:
            _validate_text(bundle, field, limit)
        except ValueError as exc:
            raise RuntimeError("Test gateway returned an invalid cloud bundle") from exc
    if bundle["deviceId"] != expected_device_id:
        raise DeviceIdentityMismatchError()
    return bundle


def validate_cloud_config_response(result: Any,
                                   bundle: dict[str, Any]) -> dict[str, Any]:
    """Require the device acknowledgement to identify the staged bundle."""
    if not isinstance(result, dict):
        raise RuntimeError("cloud-config returned an invalid response")
    if result.get("ok") is not True:
        raise RuntimeError(str(result.get("error") or "CLOUD_CONFIG_INVALID"))
    response_version = result.get("bundleVersion")
    if (result.get("state") != "pending"
            or result.get("credentialId") != bundle.get("credentialId")
            or isinstance(response_version, bool)
            or response_version != bundle.get("bundleVersion")):
        raise RuntimeError("cloud-config acknowledgement does not match the claimed bundle")
    return result


def _gateway_json(method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    if not TEST_GATEWAY_HTTP_URL or not TEST_GATEWAY_CLAIM_TOKEN:
        raise RuntimeError("Test gateway is not configured; use start-full-stack.ps1")
    parsed = urlparse(TEST_GATEWAY_HTTP_URL)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password:
        raise RuntimeError("NUWAX_TEST_GATEWAY_HTTP_URL is invalid")
    body = None if payload is None else json.dumps(payload, separators=(",", ":")).encode("utf-8")
    request = Request(
        f"{TEST_GATEWAY_HTTP_URL}{path}",
        data=body,
        method=method,
        headers={
            "Authorization": f"Bearer {TEST_GATEWAY_CLAIM_TOKEN}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    try:
        with HTTP.open(request, timeout=5) as response:
            return json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        raise RuntimeError(f"Test gateway returned HTTP {exc.code}") from exc
    except (URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise RuntimeError("Test gateway request failed") from exc


def is_loopback_authority(authority: str) -> bool:
    try:
        return urlparse(f"//{authority}").hostname in ("127.0.0.1", "localhost")
    except ValueError:
        return False


def _resolve_idf_path() -> str:
    configured = os.environ.get("IDF_PATH")
    if configured:
        return configured
    if PROJECT_DESCRIPTION.exists():
        data = json.loads(PROJECT_DESCRIPTION.read_text(encoding="utf-8"))
        if data.get("idf_path"):
            return str(data["idf_path"])
    raise RuntimeError("IDF_PATH is not set and build/project_description.json is unavailable")


os.environ.setdefault("IDF_PATH", _resolve_idf_path())
sys.path.insert(0, str(ESP_PROV_TOOL))

try:
    import bleak
    import esp_prov
    import proto as esp_prov_proto
    import prov
    from utils import str_to_bytes
except ImportError as exc:
    raise SystemExit(
        f"Bridge dependency missing: {exc}. Run: python -m pip install -r "
        f"{SIMULATOR_DIR / 'requirements.txt'}"
    ) from exc


esp_prov.config_throw_except = True


class NoPairBleakClient(esp_prov.transport.ble_cli.BLE_Bleak_Client):
    """Component-compatible BLE transport that never creates a GAP bond.

    Espressif's generic Windows CLI calls ``pair()`` to support examples that
    require protected characteristics. This firmware protects provisioning with
    protocomm Security 2 instead, so the integration bridge must not create a
    system pairing or a persistent BLE bond.
    """

    async def connect(self, devname: str, iface: str, chrc_names: Any,
                      fallback_srv_uuid: str) -> None:
        self.devname = devname
        self.srv_uuid_fallback = fallback_srv_uuid
        self.chrc_names = [name.lower() for name in chrc_names]
        self.iface = iface
        discovery = await bleak.BleakScanner.discover(timeout=5.0, return_adv=True)
        expected_uuid = fallback_srv_uuid.lower()
        found_device = next((item for item in discovery.values() if (
            (item[0].name or item[1].local_name or "") == devname
            and expected_uuid in [value.lower() for value in item[1].service_uuids]
        )), None)
        if not found_device:
            raise RuntimeError("Device not found")

        advertised_uuids = [item.lower() for item in found_device[1].service_uuids]
        self.srv_uuid_adv = advertised_uuids[0] if len(advertised_uuids) == 1 else None
        self.device = bleak.BleakClient(found_device[0])
        await self.device.connect()
        services = self.device.services
        service = None
        if self.srv_uuid_adv:
            service = services.get_service(self.srv_uuid_adv)
        if service is None:
            service = services.get_service(self.srv_uuid_fallback)
        if service is None:
            await self.device.disconnect()
            self.device = None
            raise RuntimeError("Provisioning service not found")

        lookup: dict[str, str] = {}
        for characteristic in service.characteristics:
            for descriptor in characteristic.descriptors:
                if descriptor.uuid[4:8].lower() != "2901":
                    continue
                descriptor_value = await self.device.read_gatt_descriptor(descriptor.handle)
                endpoint_name = "".join(chr(byte) for byte in descriptor_value).lower()
                lookup[endpoint_name] = characteristic.uuid
                self.characteristics[characteristic.uuid] = characteristic
        if not all(name in lookup for name in self.chrc_names):
            await self.device.disconnect()
            self.device = None
            self.characteristics = {}
            raise RuntimeError("Required provisioning endpoint not found")
        self.nu_lookup = lookup
        return True

    async def disconnect(self) -> None:
        if self.device:
            await self.device.disconnect()
            self.device = None
        self.nu_lookup = None
        self.characteristics = {}


class JobStore:
    def __init__(self) -> None:
        self._jobs: dict[str, dict[str, Any]] = {}
        self._lock = threading.Lock()
        self._operation_lock = threading.Lock()

    def create(self) -> str:
        job_id = uuid.uuid4().hex
        with self._lock:
            while len(self._jobs) >= MAX_RETAINED_JOBS:
                terminal = [
                    job for job in self._jobs.values()
                    if job["state"] not in ("queued", "running")
                ]
                if not terminal:
                    raise BusyOperationError("Provisioning job capacity is exhausted")
                oldest = min(terminal, key=lambda job: job["createdAt"])
                self._jobs.pop(oldest["id"], None)
            self._jobs[job_id] = {
                "id": job_id,
                "state": "queued",
                "stage": "queued",
                "result": None,
                "error": None,
                "events": [],
                "deviceInfo": None,
                "protocolInfo": None,
                "createdAt": time.time(),
            }
        return job_id

    def event(self, job_id: str, stage: str, message: str, result: str = "") -> None:
        with self._lock:
            job = self._jobs[job_id]
            job["stage"] = stage
            job["events"].append({
                "index": len(job["events"]),
                "timeMs": int((time.time() - job["createdAt"]) * 1000),
                "stage": stage,
                "message": message,
                "result": result,
            })

    def update(self, job_id: str, **values: Any) -> None:
        with self._lock:
            self._jobs[job_id].update(values)

    def get(self, job_id: str) -> dict[str, Any] | None:
        with self._lock:
            job = self._jobs.get(job_id)
            return json.loads(json.dumps(job)) if job else None

    def start(self, payload: dict[str, str]) -> str:
        if not self._operation_lock.acquire(blocking=False):
            payload.clear()
            raise BusyOperationError()
        try:
            job_id = self.create()
            thread = threading.Thread(
                target=self._run_thread,
                args=(job_id, payload),
                name=f"ble-provision-{job_id[:8]}",
                daemon=True,
            )
            thread.start()
            return job_id
        except Exception:
            payload.clear()
            self._operation_lock.release()
            raise

    def scan_wifi(self, payload: dict[str, str]) -> dict[str, Any]:
        if not self._operation_lock.acquire(blocking=False):
            payload.clear()
            raise BusyOperationError()
        try:
            return asyncio.run(asyncio.wait_for(
                self._scan_wifi(payload), BLE_OPERATION_TIMEOUT_SECONDS,
            ))
        finally:
            payload.clear()
            self._operation_lock.release()

    def scan_devices(self) -> list[dict[str, Any]]:
        if not self._operation_lock.acquire(blocking=False):
            raise BusyOperationError()
        try:
            return asyncio.run(scan_devices())
        finally:
            self._operation_lock.release()

    def _run_thread(self, job_id: str, payload: dict[str, str]) -> None:
        try:
            self.update(job_id, state="running")
            asyncio.run(asyncio.wait_for(
                self._provision(job_id, payload), PROVISION_OPERATION_TIMEOUT_SECONDS,
            ))
        except Exception as exc:  # Boundary for background operation reporting.
            outcome = classify_exception(exc)
            self.event(job_id, "failed", "Provisioning operation failed", outcome)
            self.update(job_id, state="failed", result=outcome, error=safe_error(exc))
        finally:
            payload.clear()
            self._operation_lock.release()

    async def _read_device_info(self, transport: Any, security: Any) -> dict[str, Any]:
        request = security.encrypt_data(b"{}").decode("latin-1")
        response = await transport.send_data("device-info", request)
        decrypted = security.decrypt_data(str_to_bytes(response))
        return json.loads(decrypted.decode("utf-8"))

    async def _read_wifi_status(self, transport: Any, security: Any) -> dict[str, Any]:
        request = prov.config_get_status_request("wifi", security)
        response = await transport.send_data("prov-config", request)
        decrypted = security.decrypt_data(str_to_bytes(response))
        return parse_wifi_status_message(decrypted)

    async def _send_cloud_config(self, transport: Any, security: Any,
                                 bundle: dict[str, Any]) -> dict[str, Any]:
        request_json = json.dumps({"op": "set", "bundle": bundle},
                                  separators=(",", ":")).encode("utf-8")
        request = security.encrypt_data(request_json).decode("latin-1")
        response = await transport.send_data("cloud-config", request)
        decrypted = security.decrypt_data(str_to_bytes(response))
        result = json.loads(decrypted.decode("utf-8"))
        return validate_cloud_config_response(result, bundle)

    async def _claim_cloud_bundle(self, device_id: str) -> dict[str, Any]:
        response = await asyncio.to_thread(
            _gateway_json, "POST", "/api/onboarding/claim", {"deviceId": device_id},
        )
        bundle = response.get("bundle") if response.get("ok") is True else None
        return validate_claim_bundle(bundle, device_id)

    async def _wait_for_cloud_online(self, job_id: str, device_id: str,
                                     credential_id: str) -> str:
        deadline = time.monotonic() + CLOUD_ONLINE_TIMEOUT_SECONDS
        commit_pending_reported = False
        while time.monotonic() < deadline:
            response = await asyncio.to_thread(
                _gateway_json, "GET",
                f"/api/onboarding/status?device_id={device_id}", None,
            )
            if (response.get("state") == "DEVICE_ONLINE" and
                    response.get("credentialId") == credential_id):
                return "DEVICE_ONLINE"
            if (response.get("state") == "CLOUD_CONFIG_COMMIT_PENDING" and
                    response.get("credentialId") == credential_id and
                    not commit_pending_reported):
                commit_pending_reported = True
                self.update(job_id, milestone="CLOUD_CONFIG_COMMIT_PENDING")
                self.event(job_id, "cloud-auth",
                           "Cloud credential authenticated; waiting for device commit",
                           "CLOUD_CONFIG_COMMIT_PENDING")
            if (response.get("state") == "CLOUD_CREDENTIAL_REJECTED" and
                    response.get("credentialId") == credential_id):
                return "CLOUD_CREDENTIAL_REJECTED"
            await asyncio.sleep(1)
        return "CLOUD_CONNECT_TIMEOUT"

    async def _scan_wifi(self, payload: dict[str, str]) -> dict[str, Any]:
        service_name = payload["serviceName"]
        username = payload["username"]
        pop = payload["pop"]
        contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
        service_uuid = contract["transport"]["primaryServiceUuid"]
        endpoint_fallback = {
            "prov-ctrl": "ff4f",
            "prov-scan": "ff50",
            "prov-session": "ff51",
            "prov-config": "ff52",
            "proto-ver": "ff53",
        }
        transport = esp_prov.transport.Transport_BLE(
            service_uuid=service_uuid,
            nu_lookup=endpoint_fallback,
        )
        transport.cli = NoPairBleakClient()
        try:
            await transport.connect(devname=service_name)
            version_raw = await esp_prov.get_version(transport)
            protocol_info = json.loads(version_raw)
            prov_info = protocol_info.get("prov", {})
            if int(prov_info.get("sec_ver", -1)) != 2:
                raise RuntimeError("Device does not advertise Security 2")
            capabilities = prov_info.get("cap", [])
            if "wifi_scan" not in capabilities:
                raise RuntimeError("Required wifi_scan capability is missing")

            sec_patch_ver = await esp_prov.get_sec_patch_ver(transport)
            security = esp_prov.get_security(2, sec_patch_ver, username, pop)
            if security is None:
                raise SecurityAuthenticationError()
            try:
                session_established = await esp_prov.establish_session(transport, security)
            except Exception as exc:
                raise SecurityAuthenticationError() from exc
            if not session_established:
                raise SecurityAuthenticationError()

            device_info = await self._read_device_info(transport, security)
            assert_device_identity(device_info, username)
            networks = await esp_prov.scan_wifi_APs("ble", transport, security)
            if networks is None:
                raise RuntimeError("Device Wi-Fi scan failed")
            return {
                "deviceInfo": device_info,
                "protocolInfo": protocol_info,
                "networks": normalize_wifi_networks(networks),
            }
        finally:
            try:
                await transport.disconnect()
            except Exception:
                pass

    async def _provision(self, job_id: str, payload: dict[str, str]) -> None:
        service_name = payload["serviceName"]
        username = payload["username"]
        pop = payload["pop"]
        ssid = payload["ssid"]
        passphrase = payload["password"]
        contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
        service_uuid = contract["transport"]["primaryServiceUuid"]
        transport = None
        wifi_ready = False
        cloud_credential_id = ""

        try:
            self.event(job_id, "scan", "Locating provisioning peripheral", service_name)
            endpoint_fallback = {
                "prov-ctrl": "ff4f",
                "prov-scan": "ff50",
                "prov-session": "ff51",
                "prov-config": "ff52",
                "proto-ver": "ff53",
            }
            transport = esp_prov.transport.Transport_BLE(
                service_uuid=service_uuid,
                nu_lookup=endpoint_fallback,
            )
            transport.cli = NoPairBleakClient()
            await transport.connect(devname=service_name)
            self.event(job_id, "connect", "BLE GATT connected", service_name)

            version_raw = await esp_prov.get_version(transport)
            protocol_info = json.loads(version_raw)
            self.update(job_id, protocolInfo=protocol_info)
            validate_protocol_info(protocol_info)
            self.event(job_id, "proto", "Protocol and capabilities verified",
                       "Security 2 / wifi_prov / wifi_scan / cloud_prov")

            sec_patch_ver = await esp_prov.get_sec_patch_ver(transport)
            security = esp_prov.get_security(2, sec_patch_ver, username, pop)
            if security is None:
                raise SecurityAuthenticationError()
            try:
                session_established = await esp_prov.establish_session(transport, security)
            except Exception as exc:
                raise SecurityAuthenticationError() from exc
            if not session_established:
                raise SecurityAuthenticationError()
            self.event(job_id, "security", "Security 2 session established", "SRP6a + AES-GCM")

            device_info = await self._read_device_info(transport, security)
            assert_device_identity(device_info, username)
            self.update(job_id, deviceInfo=device_info)
            self.event(job_id, "device", "device-info received", device_info.get("serialNumber", "unknown"))

            cloud_bundle = await self._claim_cloud_bundle(username)
            cloud_result = await self._send_cloud_config(transport, security, cloud_bundle)
            cloud_credential_id = str(cloud_result.get("credentialId") or "")
            self.update(job_id, cloudConfig={
                "state": cloud_result.get("state"),
                "credentialId": cloud_credential_id,
                "bundleVersion": cloud_result.get("bundleVersion"),
                "cloudUrl": cloud_bundle.get("cloudUrl"),
            })
            self.event(job_id, "cloud-config", "Signed cloud configuration staged",
                       cloud_credential_id)
            cloud_bundle.clear()

            if not await esp_prov.send_wifi_config(transport, security, ssid, passphrase):
                raise RuntimeError("prov-config rejected Wi-Fi credentials")
            self.event(job_id, "config", "Wi-Fi credentials accepted", "prov-config set")

            if not await esp_prov.apply_wifi_config(transport, security):
                raise RuntimeError("prov-config apply failed")
            self.event(job_id, "apply", "Wi-Fi configuration applied", "prov-config apply")

            deadline = time.monotonic() + contract["successCriteria"]["recommendedAppStatusTimeoutMs"] / 1000
            last_state = "connecting"
            while time.monotonic() < deadline:
                await asyncio.sleep(1)
                wifi_status = await self._read_wifi_status(transport, security)
                last_state = wifi_status["state"]
                self.update(job_id, wifiStatus=wifi_status)
                self.event(job_id, "status", "Official provisioning status", str(last_state))
                if last_state == "connected":
                    wifi_ready = True
                    self.update(job_id, milestone="WIFI_READY")
                    self.event(job_id, "wifi-ready", "Wi-Fi provisioning completed", "IP_EVENT_STA_GOT_IP")
                    break
                if last_state == "failed":
                    latest_info = None
                    try:
                        latest_info = await self._read_device_info(transport, security)
                        self.update(job_id, deviceInfo=latest_info)
                    except Exception:
                        pass
                    outcome = map_device_outcome(
                        latest_info.get("lastProvisioningOutcome")
                        if latest_info else None,
                        last_state,
                    )
                    if latest_info and isinstance(
                            latest_info.get("wifiDisconnectReason"), int):
                        wifi_status["rawDisconnectReason"] = latest_info[
                            "wifiDisconnectReason"
                        ]
                        wifi_status["rawDisconnectReasonName"] = latest_info.get(
                            "wifiDisconnectReasonName"
                        )
                        self.update(job_id, wifiStatus=wifi_status)
                    self.update(job_id, state="failed", result=outcome,
                                error="Wi-Fi provisioning failed")
                    self.event(job_id, "failed", "Wi-Fi provisioning failed", outcome)
                    return

            if not wifi_ready:
                latest_info = None
                try:
                    latest_info = await self._read_device_info(transport, security)
                    self.update(job_id, deviceInfo=latest_info)
                except Exception:
                    # The BLE session may close at the timeout boundary. Preserve
                    # the last official prov-config state instead of hiding it
                    # behind a generic transport error.
                    pass
                outcome = map_device_outcome(
                    latest_info.get("lastProvisioningOutcome") if latest_info else None,
                    last_state,
                )
                self.update(job_id, state="failed", result=outcome,
                            error="Provisioning did not reach GOT_IP")
                self.event(job_id, "failed", "Wi-Fi provisioning failed", outcome)
        finally:
            if transport is not None:
                try:
                    await transport.disconnect()
                except Exception:
                    pass

        if not wifi_ready:
            return
        self.event(job_id, "cloud", "Waiting for authenticated device cloud session",
                   "DEVICE_ONLINE")
        cloud_state = await self._wait_for_cloud_online(
            job_id, username, cloud_credential_id,
        )
        if cloud_state == "DEVICE_ONLINE":
            self.update(job_id, state="success", result="SUCCESS",
                        milestone="DEVICE_ONLINE")
            self.event(job_id, "success", "Onboarding completed",
                       "WIFI_READY + DEVICE_ONLINE")
            return
        self.update(job_id, state="failed", result=cloud_state,
                    error="Wi-Fi is ready but cloud authentication did not complete")
        self.event(job_id, "failed", "Cloud authentication did not complete",
                   cloud_state)


def parse_wifi_status_message(decrypted_message: bytes) -> dict[str, Any]:
    """Preserve the official Wi-Fi failure reason discarded by esp_prov."""
    payload = esp_prov_proto.network_config_pb2.NetworkConfigPayload()
    payload.ParseFromString(decrypted_message)
    if payload.msg != esp_prov_proto.network_config_pb2.TypeRespGetWifiStatus:
        raise RuntimeError("Unexpected prov-config status response")

    response = payload.resp_get_wifi_status
    states = {
        0: "connected",
        1: "connecting",
        2: "disconnected",
        3: "failed",
    }
    state = states.get(int(response.wifi_sta_state), "unknown")
    detail: dict[str, Any] = {"state": state}
    if state == "connecting" and response.HasField("attempt_failed"):
        detail["attemptsRemaining"] = int(response.attempt_failed.attempts_remaining)
    if state == "failed":
        failure_reason = int(response.wifi_fail_reason)
        detail["failureReason"] = (
            "auth_failed" if failure_reason == 0
            else "network_not_found" if failure_reason == 1
            else "unknown"
        )
        detail["outcome"] = (
            "WIFI_AUTH_FAILED" if failure_reason == 0
            else "NETWORK_NOT_FOUND" if failure_reason == 1
            else "INTERNAL_ERROR"
        )
    return detail


def normalize_wifi_networks(networks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Deduplicate APs by SSID, keeping the strongest device-visible BSSID."""
    strongest: dict[str, dict[str, Any]] = {}
    for network in networks:
        ssid = network.get("ssid")
        if not isinstance(ssid, str) or not ssid.strip():
            continue
        try:
            # Espressif's helper decodes raw SSID bytes as latin-1. Restore
            # ordinary UTF-8 SSIDs while preserving arbitrary byte sequences.
            ssid = ssid.encode("latin-1").decode("utf-8")
        except (UnicodeEncodeError, UnicodeDecodeError):
            pass
        normalized = {
            "ssid": ssid,
            "rssi": int(network.get("rssi", -127)),
            "channel": int(network.get("channel", 0)),
            "auth": str(network.get("auth", "Unknown")),
        }
        current = strongest.get(ssid)
        if current is None or normalized["rssi"] > current["rssi"]:
            strongest[ssid] = normalized
    return sorted(
        strongest.values(),
        key=lambda item: (-item["rssi"], item["ssid"].casefold()),
    )[:24]


def map_device_outcome(outcome: str | None, last_state: str | None) -> str:
    mapping = {
        "auth_failed": "WIFI_AUTH_FAILED",
        "network_not_found": "NETWORK_NOT_FOUND",
        "timeout": "TIMEOUT",
        "cloud_config_required": "CLOUD_CONFIG_REQUIRED",
        "internal_error": "INTERNAL_ERROR",
        "stopped": "TIMEOUT",
        "success": "SUCCESS",
    }
    if outcome in mapping:
        return mapping[outcome]
    return "TIMEOUT" if last_state in ("connecting", "disconnected", None) else "INTERNAL_ERROR"


class SecurityAuthenticationError(RuntimeError):
    def __init__(self) -> None:
        super().__init__("Security 2 authentication failed")


class DeviceIdentityMismatchError(RuntimeError):
    def __init__(self) -> None:
        super().__init__("Device identity does not match the QR credential")


class BusyOperationError(RuntimeError):
    def __init__(self, message: str = "Another BLE operation is running") -> None:
        super().__init__(message)


def assert_device_identity(device_info: dict[str, Any], expected_username: str) -> None:
    if device_info.get("serialNumber") != expected_username:
        raise DeviceIdentityMismatchError()


def classify_exception(exc: Exception) -> str:
    if isinstance(exc, (asyncio.TimeoutError, TimeoutError)):
        return "TIMEOUT"
    if isinstance(exc, SecurityAuthenticationError):
        return "SECURITY_AUTH_FAILED"
    if isinstance(exc, DeviceIdentityMismatchError):
        return "DEVICE_ID_MISMATCH"
    message = str(exc).lower()
    for cloud_code in (
        "CLOUD_CONFIG_INVALID",
        "CLOUD_SIGNATURE_INVALID",
        "CLOUD_CONFIG_ROLLBACK",
        "CLOUD_CONNECT_TIMEOUT",
        "CLOUD_CREDENTIAL_REJECTED",
    ):
        if cloud_code.lower() in message:
            return cloud_code
    if any(token in message for token in ("credential", "proof", "srp", "session")):
        return "SECURITY_AUTH_FAILED"
    if "device not found" in message:
        return "DEVICE_NOT_FOUND"
    if "endpoint" in message or "capability" in message or "security 2" in message:
        return "PROTOCOL_ERROR"
    return "INTERNAL_ERROR"


def safe_error(exc: Exception) -> str:
    text = str(exc).strip() or exc.__class__.__name__
    lowered = text.lower()
    if "could not write value" in lowered:
        return "BLE GATT write failed"
    if "bytearray(" in lowered or "\\x" in text:
        return exc.__class__.__name__
    return text[:240]


async def scan_devices() -> list[dict[str, Any]]:
    contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    prefix = contract["transport"]["serviceNamePrefix"]
    expected_uuid = contract["transport"]["primaryServiceUuid"].lower()
    discovery = await bleak.BleakScanner.discover(timeout=5.0, return_adv=True)
    devices = []
    for device, advertisement in discovery.values():
        name = device.name or advertisement.local_name or ""
        service_uuids = [value.lower() for value in advertisement.service_uuids]
        if not name.startswith(prefix) or expected_uuid not in service_uuids:
            continue
        devices.append({
            "name": name,
            "rssi": advertisement.rssi,
            "serviceUuidMatched": expected_uuid in service_uuids,
        })
    devices.sort(key=lambda item: item["rssi"], reverse=True)
    return devices


JOBS = JobStore()


class BridgeHandler(SimpleHTTPRequestHandler):
    server_version = "NuwaxProvisionBridge/1.0"

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        # Serve only simulator assets. Serving ROOT would expose sdkconfig,
        # source files and build artifacts through the local HTTP server.
        super().__init__(*args, directory=str(SIMULATOR_DIR), **kwargs)

    def translate_path(self, path: str) -> str:
        requested = urlparse(path).path
        for prefix in STATIC_PREFIXES:
            if requested == prefix:
                requested = f"{prefix}/"
            if requested.startswith(f"{prefix}/"):
                relative = unquote(requested[len(prefix) + 1:])
                candidate = (SIMULATOR_DIR / relative).resolve()
                try:
                    candidate.relative_to(SIMULATOR_DIR)
                except ValueError:
                    return str(SIMULATOR_DIR / "__not_found__")
                return str(candidate)
        return str(SIMULATOR_DIR / "__not_found__")

    def end_headers(self) -> None:
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Permissions-Policy", "camera=(self)")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; "
            "img-src 'self' data: blob:; media-src 'self' blob:; "
            "connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'",
        )
        super().end_headers()

    def log_message(self, format: str, *args: Any) -> None:
        # Never log request bodies or query strings that could contain input.
        super().log_message("%s %s", self.command, urlparse(self.path).path)

    def _json(self, status: int, payload: Any) -> None:
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def _read_json(self) -> dict[str, Any]:
        if self.headers.get_content_type() != "application/json":
            raise ValueError("Content-Type must be application/json")
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 16384:
            raise ValueError("Invalid request size")
        payload = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("JSON request body must be an object")
        return payload

    def _local_request_allowed(self) -> bool:
        authority = self.headers.get("Host", "")
        if not is_loopback_authority(authority):
            return False
        origin = self.headers.get("Origin")
        if not origin:
            return True
        try:
            parsed = urlparse(origin)
        except ValueError:
            return False
        return parsed.scheme == "http" and parsed.netloc.lower() == authority.lower()

    def _reject_non_local_request(self) -> bool:
        if self._local_request_allowed():
            return False
        self._json(HTTPStatus.FORBIDDEN, {"error": "Only same-origin loopback requests are allowed"})
        return True

    def do_GET(self) -> None:
        if self._reject_non_local_request():
            return
        path = urlparse(self.path).path
        if path == "/api/health":
            self._json(HTTPStatus.OK, {
                "ok": True,
                "transport": "ble",
                "security": 2,
                "componentVersion": json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))["firmware"]["componentVersion"],
                "mockOnly": False,
                "cloudOnboarding": bool(TEST_GATEWAY_HTTP_URL and TEST_GATEWAY_CLAIM_TOKEN),
                "testGateway": TEST_GATEWAY_HTTP_URL or None,
            })
            return
        if path == "/api/contract":
            self._json(HTTPStatus.OK, json.loads(CONTRACT_PATH.read_text(encoding="utf-8")))
            return
        if path.startswith("/api/jobs/"):
            job = JOBS.get(path.rsplit("/", 1)[-1])
            if job is None:
                self._json(HTTPStatus.NOT_FOUND, {"error": "Job not found"})
            else:
                self._json(HTTPStatus.OK, job)
            return
        if path == "/":
            self.send_response(HTTPStatus.FOUND)
            self.send_header("Location", "/simulator/")
            self.end_headers()
            return
        super().do_GET()

    def do_POST(self) -> None:
        if self._reject_non_local_request():
            return
        path = urlparse(self.path).path
        if path == "/api/devices":
            try:
                payload = self._read_json()
                payload.clear()
                self._json(HTTPStatus.OK, {"devices": JOBS.scan_devices()})
            except BusyOperationError as exc:
                self._json(HTTPStatus.CONFLICT, {"error": safe_error(exc), "result": "BUSY", "devices": []})
            except (ValueError, json.JSONDecodeError) as exc:
                self._json(HTTPStatus.BAD_REQUEST, {"error": safe_error(exc), "devices": []})
            except Exception as exc:
                self._json(HTTPStatus.SERVICE_UNAVAILABLE, {"error": safe_error(exc), "devices": []})
            return
        if path == "/api/wifi-scan":
            try:
                payload = self._read_json()
                credentials = validate_scan_payload(payload)
                payload.clear()
                result = JOBS.scan_wifi(credentials)
                self._json(HTTPStatus.OK, result)
            except BusyOperationError as exc:
                self._json(HTTPStatus.CONFLICT, {"error": safe_error(exc), "result": "BUSY"})
            except (ValueError, json.JSONDecodeError) as exc:
                self._json(HTTPStatus.BAD_REQUEST, {"error": safe_error(exc)})
            except Exception as exc:
                self._json(HTTPStatus.UNPROCESSABLE_ENTITY, {
                    "error": safe_error(exc),
                    "result": classify_exception(exc),
                })
            return
        if path != "/api/provision":
            self._json(HTTPStatus.NOT_FOUND, {"error": "Not found"})
            return
        try:
            payload = self._read_json()
            credentials = validate_provision_payload(payload)
            payload.clear()
            job_id = JOBS.start(credentials)
            self._json(HTTPStatus.ACCEPTED, {"jobId": job_id})
        except BusyOperationError as exc:
            self._json(HTTPStatus.CONFLICT, {"error": safe_error(exc), "result": "BUSY"})
        except (ValueError, json.JSONDecodeError) as exc:
            self._json(HTTPStatus.BAD_REQUEST, {"error": safe_error(exc)})


def main() -> None:
    parser = argparse.ArgumentParser(description="Nuwax local BLE provisioning bridge")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if args.host not in ("127.0.0.1", "localhost"):
        raise SystemExit("For credential safety, the bridge only supports loopback binding")
    server = ThreadingHTTPServer((args.host, args.port), BridgeHandler)
    print(f"Nuwax BLE provisioning bridge: http://{args.host}:{args.port}/simulator/")
    print("Press Ctrl+C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
