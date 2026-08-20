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


def _find_project_root(start: Path) -> Path | None:
    for candidate in (start, *start.parents):
        contract = candidate / "main" / "nuwax_provision_new" / "esp32s3-idf6-provisioning-contract.json"
        if contract.is_file():
            return candidate
    return None


ROOT = _find_project_root(SIMULATOR_DIR)
PACKAGED_CONTRACT = next((path for path in (
    SIMULATOR_DIR.parent / "provisioning-contract.json",
    SIMULATOR_DIR.parent / "05-provisioning-contract.json",
) if path.is_file()), None)
PROJECT_CONTRACT = (ROOT / "main" / "nuwax_provision_new" /
                    "esp32s3-idf6-provisioning-contract.json") if ROOT else None
CONTRACT_PATH = Path(os.environ.get(
    "NUWAX_PROVISION_CONTRACT",
    str(PACKAGED_CONTRACT or PROJECT_CONTRACT or ""),
))
if not CONTRACT_PATH.is_file():
    raise RuntimeError("Cannot locate the provisioning contract")
PACKAGED_COMPONENT_DIR = SIMULATOR_DIR / "vendor" / "espressif__network_provisioning"
PROJECT_COMPONENT_DIR = (ROOT / "managed_components" /
                         "espressif__network_provisioning") if ROOT else None
COMPONENT_DIR = (PACKAGED_COMPONENT_DIR if PACKAGED_COMPONENT_DIR.is_dir()
                 else PROJECT_COMPONENT_DIR)
if COMPONENT_DIR is None or not COMPONENT_DIR.is_dir():
    raise RuntimeError("Cannot locate espressif/network_provisioning tooling")
ESP_PROV_TOOL = COMPONENT_DIR / "tool" / "esp_prov"
PROJECT_DESCRIPTION = ROOT / "build" / "project_description.json" if ROOT else None
STATIC_PREFIXES = ("/docs/provision_simulator", "/simulator")
MAX_RETAINED_JOBS = 64
BLE_OPERATION_TIMEOUT_SECONDS = 90
PROVISION_OPERATION_TIMEOUT_SECONDS = 240
TEST_GATEWAY_HTTP_URL = os.environ.get("NUWAX_TEST_GATEWAY_HTTP_URL", "").rstrip("/")
TEST_GATEWAY_TOKEN = os.environ.get("NUWAX_TEST_GATEWAY_TOKEN", "")
TEST_GATEWAY_WS_URL = os.environ.get("NUWAX_TEST_GATEWAY_WS_URL", "")
DEVICE_SERIAL_PORT = os.environ.get("NUWAX_DEVICE_SERIAL_PORT", "").strip()
CLOUD_ONLINE_TIMEOUT_SECONDS = int(os.environ.get("NUWAX_CLOUD_ONLINE_TIMEOUT_SECONDS", "90"))
VOLUME_ACK_TIMEOUT_SECONDS = float(os.environ.get("NUWAX_VOLUME_ACK_TIMEOUT_SECONDS", "8"))
VOLUME_ACK_POLL_SECONDS = float(os.environ.get("NUWAX_VOLUME_ACK_POLL_SECONDS", "0.25"))
AUDIO_MODE_ENABLE_ACK_TIMEOUT_SECONDS = float(os.environ.get(
    "NUWAX_AUDIO_MODE_ENABLE_ACK_TIMEOUT_SECONDS", "12",
))
AUDIO_MODE_DISABLE_ACK_TIMEOUT_SECONDS = float(os.environ.get(
    "NUWAX_AUDIO_MODE_DISABLE_ACK_TIMEOUT_SECONDS", "25",
))
HTTP = build_opener(ProxyHandler({}))


class GatewayControlError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


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
    values["gatewayUrl"] = validate_gateway_url(
        _validate_text(payload, "gatewayUrl", 191),
    )
    values["deviceSecret"] = _validate_text(payload, "deviceSecret", 95)
    if any(ord(char) < 0x21 or ord(char) > 0x7e for char in values["deviceSecret"]):
        raise ValueError("deviceSecret must contain printable ASCII characters only")
    return values


def validate_reprovision_payload(payload: dict[str, Any]) -> dict[str, str]:
    device_id = _validate_text(payload, "deviceId", 39)
    if any(char.isspace() or ord(char) < 0x21 or ord(char) == 0x7f for char in device_id):
        raise ValueError("deviceId must not contain whitespace or control characters")
    service_name = _validate_text(payload, "serviceName", 15)
    if re.fullmatch(r"PROV_[0-9A-F]{6}", service_name) is None:
        raise ValueError("serviceName must match PROV_ followed by six uppercase hex digits")
    return {"deviceId": device_id, "serviceName": service_name}


def validate_gateway_control_payload(payload: dict[str, Any]) -> dict[str, str]:
    device_id = _validate_text(payload, "deviceId", 39)
    if any(char.isspace() or ord(char) < 0x21 or ord(char) == 0x7f for char in device_id):
        raise ValueError("deviceId must not contain whitespace or control characters")
    return {
        "deviceId": device_id,
        "gatewayUrl": validate_gateway_url(_validate_text(payload, "gatewayUrl", 191)),
    }


def validate_volume_payload(payload: dict[str, Any]) -> dict[str, Any]:
    values: dict[str, Any] = validate_gateway_control_payload(payload)
    volume_percent = payload.get("volumePercent")
    if type(volume_percent) is not int or not 0 <= volume_percent <= 100:
        raise ValueError("volumePercent must be an integer from 0 to 100")
    persist = payload.get("persist", False)
    if type(persist) is not bool:
        raise ValueError("persist must be a boolean")
    values["volumePercent"] = volume_percent
    values["persist"] = persist
    return values


def validate_audio_mode_payload(payload: dict[str, Any]) -> dict[str, Any]:
    values: dict[str, Any] = validate_gateway_control_payload(payload)
    mode = payload.get("mode")
    if mode not in ("meeting", "monitoring"):
        raise ValueError("mode must be meeting or monitoring")
    enabled = payload.get("enabled")
    if type(enabled) is not bool:
        raise ValueError("enabled must be a boolean")
    values["mode"] = mode
    values["enabled"] = enabled
    return values


def validate_gateway_url(value: str) -> str:
    try:
        parsed = urlparse(value)
        port = parsed.port
    except ValueError as exc:
        raise ValueError("gatewayUrl is invalid") from exc
    if (not value.isascii() or parsed.scheme not in ("ws", "wss") or not parsed.hostname
            or parsed.username or parsed.password or parsed.fragment
            or ":" in parsed.hostname
            or parsed.hostname.startswith(".") or parsed.hostname.endswith(".")):
        raise ValueError("gatewayUrl must be a ws:// or wss:// URL without credentials or fragment")
    if port is not None and not 1 <= port <= 65535:
        raise ValueError("gatewayUrl port is invalid")
    return value


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


def validate_vox_config_response(result: Any, expected_device_id: str) -> dict[str, Any]:
    """Require an acknowledgement without accepting or exposing the secret."""
    if not isinstance(result, dict):
        raise RuntimeError("vox-config returned an invalid response")
    if result.get("ok") is not True:
        raise RuntimeError(str(result.get("error") or "VOX_CONFIG_INVALID"))
    if result.get("state") != "pending" or result.get("deviceId") != expected_device_id:
        raise RuntimeError("vox-config acknowledgement does not match the device")
    if "deviceSecret" in result:
        raise RuntimeError("vox-config response must not echo deviceSecret")
    return result


def _gateway_json(method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    if not TEST_GATEWAY_HTTP_URL or not TEST_GATEWAY_TOKEN:
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
            "Authorization": f"Bearer {TEST_GATEWAY_TOKEN}",
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


def request_remote_ble_reprovision(device_id: str) -> dict[str, Any]:
    request_id = f"bleprov_{uuid.uuid4().hex[:12]}"
    result = _gateway_json("POST", "/api/device/command", {
        "device_id": device_id,
        "request_id": request_id,
        "command": "start_ble_provisioning",
    })
    command = result.get("command") if isinstance(result, dict) else None
    if (not isinstance(result, dict) or result.get("ok") is not True or
            not isinstance(command, dict) or
            command.get("command") != "start_ble_provisioning" or
            command.get("request_id") != request_id):
        raise RuntimeError("Test gateway did not accept the BLE provisioning command")
    return {
        "ok": True,
        "deviceId": str(result.get("device_id") or device_id),
        "requestId": request_id,
        "state": "command_forwarded",
    }


def is_loopback_authority(authority: str) -> bool:
    try:
        return urlparse(f"//{authority}").hostname in ("127.0.0.1", "localhost")
    except ValueError:
        return False


def targets_test_gateway(gateway_url: str) -> bool:
    if not TEST_GATEWAY_HTTP_URL or not TEST_GATEWAY_TOKEN:
        return False
    try:
        target = urlparse(gateway_url)
        if TEST_GATEWAY_WS_URL:
            test = urlparse(TEST_GATEWAY_WS_URL)
            test_scheme = test.scheme
            test_path = test.path or "/"
            test_query = test.query
        else:
            test = urlparse(TEST_GATEWAY_HTTP_URL)
            test_scheme = "wss" if test.scheme == "https" else "ws"
            test_path = "/api/device/ws"
            test_query = ""
        target_port = target.port or (443 if target.scheme == "wss" else 80)
        test_port = test.port or (443 if test_scheme == "wss" else 80)
    except ValueError:
        return False
    return (
        target.scheme == test_scheme
        and (target.hostname or "").casefold() == (test.hostname or "").casefold()
        and target_port == test_port
        and (target.path or "/") == test_path
        and target.query == test_query
        and not target.fragment
        and not target.username
        and not target.password
    )


def _require_test_gateway_target(gateway_url: str) -> None:
    if not TEST_GATEWAY_HTTP_URL or not TEST_GATEWAY_TOKEN:
        raise GatewayControlError(
            "TEST_GATEWAY_UNAVAILABLE",
            "The local test gateway is not configured",
        )
    if not targets_test_gateway(gateway_url):
        raise GatewayControlError(
            "FORMAL_CLOUD_CONTROL_DISABLED",
            "Simulator device control is disabled for non-test gateway URLs",
        )


def _read_test_gateway_status() -> dict[str, Any]:
    try:
        status = _gateway_json("GET", "/api/status")
    except RuntimeError as exc:
        raise GatewayControlError(
            "TEST_GATEWAY_UNAVAILABLE",
            "The local test gateway status is unavailable",
        ) from exc
    if not isinstance(status, dict) or not isinstance(status.get("devices"), list):
        raise GatewayControlError(
            "TEST_GATEWAY_UNAVAILABLE",
            "The local test gateway returned an invalid status response",
        )
    return status


def _find_exact_gateway_device(status: dict[str, Any], device_id: str) -> dict[str, Any] | None:
    return next((
        item for item in status.get("devices", [])
        if isinstance(item, dict) and str(item.get("device_id") or "") == device_id
    ), None)


def get_test_gateway_control_status(device_id: str, gateway_url: str) -> dict[str, Any]:
    _require_test_gateway_target(gateway_url)
    device = _find_exact_gateway_device(_read_test_gateway_status(), device_id)
    if device is None:
        return {
            "ok": True,
            "available": False,
            "deviceId": device_id,
            "reason": "DEVICE_OFFLINE",
            "audioMode": None,
            "features": {"meeting": False, "monitoring": False},
        }
    session = device.get("currentUplinkAudioSession")
    audio_mode = ""
    if isinstance(session, dict):
        audio_mode = str(session.get("mode") or "").strip().lower()
    busy = audio_mode in ("meeting", "monitoring")
    raw_features = device.get("features")
    if not isinstance(raw_features, dict):
        raw_features = {}
    return {
        "ok": True,
        "available": not busy,
        "deviceId": device_id,
        "reason": "AUDIO_MODE_BUSY" if busy else "READY",
        "audioMode": audio_mode or None,
        "features": {
            "meeting": raw_features.get("meeting_mode_v1") is True,
            "monitoring": raw_features.get("monitoring_mode_v1") is True,
        },
    }


def set_test_gateway_volume(values: dict[str, Any], *,
                            ack_timeout_seconds: float = VOLUME_ACK_TIMEOUT_SECONDS,
                            poll_seconds: float = VOLUME_ACK_POLL_SECONDS) -> dict[str, Any]:
    device_id = values["deviceId"]
    gateway_url = values["gatewayUrl"]
    volume_percent = values["volumePercent"]
    persist = values["persist"]
    control = get_test_gateway_control_status(device_id, gateway_url)
    if control["reason"] == "DEVICE_OFFLINE":
        raise GatewayControlError("DEVICE_OFFLINE", "The exact device is not online on the test gateway")
    if control["reason"] == "AUDIO_MODE_BUSY":
        raise GatewayControlError(
            "AUDIO_MODE_BUSY",
            "Volume control is unavailable while meeting or monitoring mode is active",
        )

    request_id = f"volume_{uuid.uuid4().hex[:12]}"
    try:
        accepted = _gateway_json("POST", "/api/device/command", {
            "device_id": device_id,
            "request_id": request_id,
            "command": "set_volume",
            "volume_percent": volume_percent,
            "persist": persist,
        })
    except RuntimeError as exc:
        raise GatewayControlError(
            "TEST_GATEWAY_UNAVAILABLE",
            "The local test gateway rejected the command request",
        ) from exc
    command = accepted.get("command") if isinstance(accepted, dict) else None
    if (not isinstance(accepted, dict) or accepted.get("ok") is not True
            or not isinstance(command, dict)
            or command.get("request_id") != request_id
            or command.get("command") != "set_volume"):
        raise GatewayControlError(
            "TEST_GATEWAY_UNAVAILABLE",
            "The local test gateway did not accept the volume command",
        )

    deadline = time.monotonic() + max(0.0, ack_timeout_seconds)
    while True:
        device = _find_exact_gateway_device(_read_test_gateway_status(), device_id)
        if device is None:
            raise GatewayControlError("DEVICE_OFFLINE", "The device disconnected before acknowledging the command")
        ack = device.get("lastAck")
        if isinstance(ack, dict) and str(ack.get("request_id") or "") == request_id:
            if str(ack.get("command") or "") != "set_volume" or ack.get("ok") is not True:
                raise GatewayControlError("VOLUME_REJECTED", "The device rejected the volume command")
            return {
                "ok": True,
                "deviceId": device_id,
                "requestId": request_id,
                "volumePercent": volume_percent,
                "persist": persist,
                "ack": {
                    "ok": True,
                    "command": "set_volume",
                    "message": str(ack.get("message") or "")[:120],
                },
            }
        if time.monotonic() >= deadline:
            raise GatewayControlError(
                "VOLUME_ACK_TIMEOUT",
                "The device did not acknowledge the volume command in time",
            )
        time.sleep(max(0.0, poll_seconds))


def set_test_gateway_audio_mode(values: dict[str, Any], *,
                                ack_timeout_seconds: float | None = None,
                                poll_seconds: float = VOLUME_ACK_POLL_SECONDS) -> dict[str, Any]:
    device_id = values["deviceId"]
    gateway_url = values["gatewayUrl"]
    mode = values["mode"]
    enabled = values["enabled"]
    control = get_test_gateway_control_status(device_id, gateway_url)
    if control["reason"] == "DEVICE_OFFLINE":
        raise GatewayControlError("DEVICE_OFFLINE", "The exact device is not online on the test gateway")
    if control["features"].get(mode) is not True:
        raise GatewayControlError(
            "AUDIO_MODE_UNSUPPORTED",
            f"The device did not advertise {mode}_mode_v1 support",
        )
    current_mode = str(control.get("audioMode") or "")
    if current_mode and current_mode != mode:
        raise GatewayControlError(
            "AUDIO_MODE_BUSY",
            "Another exclusive audio mode must be disabled first",
        )

    action = "open" if enabled else "close"
    request_id = f"sim_{mode}_{action}_{uuid.uuid4().hex[:8]}"
    command_payload: dict[str, Any] = {
        "device_id": device_id,
        "request_id": request_id,
        "command": "set_audio_mode",
        "mode": mode,
        "state": "enabled" if enabled else "disabled",
    }
    if mode == "meeting" and enabled:
        command_payload["interrupt_current_turn"] = True
    try:
        accepted = _gateway_json("POST", "/api/device/command", command_payload)
    except RuntimeError as exc:
        raise GatewayControlError(
            "TEST_GATEWAY_UNAVAILABLE",
            "The local test gateway rejected the audio mode request",
        ) from exc
    command = accepted.get("command") if isinstance(accepted, dict) else None
    expected_state = "enabled" if enabled else "disabled"
    if (not isinstance(accepted, dict) or accepted.get("ok") is not True
            or not isinstance(command, dict)
            or command.get("request_id") != request_id
            or command.get("command") != "set_audio_mode"
            or command.get("mode") != mode
            or command.get("state") != expected_state):
        raise GatewayControlError(
            "TEST_GATEWAY_UNAVAILABLE",
            "The local test gateway did not accept the audio mode command",
        )

    if ack_timeout_seconds is None:
        ack_timeout_seconds = (
            AUDIO_MODE_ENABLE_ACK_TIMEOUT_SECONDS if enabled
            else AUDIO_MODE_DISABLE_ACK_TIMEOUT_SECONDS
        )
    deadline = time.monotonic() + max(0.0, ack_timeout_seconds)
    successful_messages = (
        ("active", "already_active") if enabled
        else ("disabled", "already_disabled")
    )
    while True:
        device = _find_exact_gateway_device(_read_test_gateway_status(), device_id)
        if device is None:
            raise GatewayControlError("DEVICE_OFFLINE", "The device disconnected before acknowledging the command")
        ack = device.get("lastAck")
        if isinstance(ack, dict) and str(ack.get("request_id") or "") == request_id:
            message = str(ack.get("message") or "")
            if str(ack.get("command") or "") != "set_audio_mode" or ack.get("ok") is not True:
                code = "AUDIO_MODE_BUSY" if message in ("audio_mode_busy", "meeting_busy") else "AUDIO_MODE_REJECTED"
                raise GatewayControlError(code, "The device rejected the audio mode command")
            if message not in successful_messages:
                raise GatewayControlError(
                    "AUDIO_MODE_REJECTED",
                    "The device returned an unexpected audio mode acknowledgement",
                )
            session = device.get("currentUplinkAudioSession")
            observed_mode = ""
            if isinstance(session, dict):
                observed_mode = str(session.get("mode") or "").strip().lower()
            if enabled and observed_mode != mode:
                raise GatewayControlError(
                    "AUDIO_MODE_REJECTED",
                    "The acknowledged audio mode is not active on the test gateway",
                )
            if not enabled and observed_mode == mode:
                raise GatewayControlError(
                    "AUDIO_MODE_REJECTED",
                    "The acknowledged audio mode is still active on the test gateway",
                )
            return {
                "ok": True,
                "deviceId": device_id,
                "requestId": request_id,
                "mode": mode,
                "state": expected_state,
                "audioMode": mode if enabled else None,
                "ack": {
                    "ok": True,
                    "command": "set_audio_mode",
                    "message": message,
                },
            }
        if time.monotonic() >= deadline:
            raise GatewayControlError(
                "AUDIO_MODE_ACK_TIMEOUT",
                "The device did not acknowledge the audio mode command in time",
            )
        time.sleep(max(0.0, poll_seconds))


def gateway_control_http_status(code: str) -> HTTPStatus:
    return {
        "FORMAL_CLOUD_CONTROL_DISABLED": HTTPStatus.CONFLICT,
        "DEVICE_OFFLINE": HTTPStatus.CONFLICT,
        "AUDIO_MODE_BUSY": HTTPStatus.CONFLICT,
        "VOLUME_REJECTED": HTTPStatus.UNPROCESSABLE_ENTITY,
        "VOLUME_ACK_TIMEOUT": HTTPStatus.GATEWAY_TIMEOUT,
        "AUDIO_MODE_ACK_TIMEOUT": HTTPStatus.GATEWAY_TIMEOUT,
        "AUDIO_MODE_UNSUPPORTED": HTTPStatus.CONFLICT,
        "AUDIO_MODE_REJECTED": HTTPStatus.UNPROCESSABLE_ENTITY,
        "TEST_GATEWAY_UNAVAILABLE": HTTPStatus.SERVICE_UNAVAILABLE,
    }.get(code, HTTPStatus.BAD_GATEWAY)


def _resolve_idf_path() -> str:
    configured = os.environ.get("IDF_PATH")
    if configured:
        return configured
    if PROJECT_DESCRIPTION and PROJECT_DESCRIPTION.exists():
        data = json.loads(PROJECT_DESCRIPTION.read_text(encoding="utf-8"))
        if data.get("idf_path"):
            return str(data["idf_path"])
    raise RuntimeError("IDF_PATH is not set and build/project_description.json is unavailable")


os.environ.setdefault("IDF_PATH", _resolve_idf_path())
sys.path.insert(0, str(ESP_PROV_TOOL))

try:
    import bleak
    import serial
    import serial.tools.list_ports
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


def resolve_device_serial_port() -> str | None:
    ports = list(serial.tools.list_ports.comports())
    if DEVICE_SERIAL_PORT:
        return next((item.device for item in ports
                     if item.device.lower() == DEVICE_SERIAL_PORT.lower()), None)
    esp_ports = [item.device for item in ports if item.vid == 0x303A]
    if len(esp_ports) == 1:
        return esp_ports[0]
    return None


def request_usb_ble_reprovision(port: str) -> None:
    command = b"NUWAX_CTRL start_ble_provisioning\r\n"
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.2,
                               write_timeout=2)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    try:
        connection.open()
        connection.reset_input_buffer()
        connection.write(command)
        connection.flush()
        deadline = time.monotonic() + 5
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(connection.read(512))
            if b"NUWAX_CTRL ACK start_ble_provisioning" in received:
                return
            if b"NUWAX_CTRL BUSY start_ble_provisioning" in received:
                raise RuntimeError("Device already has a BLE provisioning restart pending")
            if len(received) > 8192:
                del received[:-4096]
        raise RuntimeError("USB control command was not acknowledged by the device")
    except serial.SerialException as exc:
        raise RuntimeError(f"Cannot use device USB serial port {port}") from exc
    finally:
        if connection.is_open:
            connection.close()


async def wait_for_ble_advertisement(service_name: str,
                                     timeout_seconds: float = 35) -> None:
    contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    expected_uuid = contract["transport"]["primaryServiceUuid"].lower()
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        discovery = await bleak.BleakScanner.discover(timeout=3.0, return_adv=True)
        for device, advertisement in discovery.values():
            name = device.name or advertisement.local_name or ""
            service_uuids = [value.lower() for value in advertisement.service_uuids]
            if name == service_name and expected_uuid in service_uuids:
                return
        await asyncio.sleep(0.25)
    raise RuntimeError(f"Exact BLE advertisement {service_name} was not observed")


def request_ble_reprovision(device_id: str, service_name: str) -> dict[str, Any]:
    serial_port = resolve_device_serial_port()
    usb_error: Exception | None = None
    if serial_port:
        try:
            request_usb_ble_reprovision(serial_port)
            asyncio.run(wait_for_ble_advertisement(service_name))
            return {
                "ok": True,
                "deviceId": device_id,
                "serviceName": service_name,
                "state": "ble_advertisement_confirmed",
                "method": "usb_serial",
            }
        except Exception as exc:
            usb_error = exc

    if TEST_GATEWAY_HTTP_URL and TEST_GATEWAY_TOKEN:
        try:
            forwarded = request_remote_ble_reprovision(device_id)
            asyncio.run(wait_for_ble_advertisement(service_name))
            return {
                **forwarded,
                "serviceName": service_name,
                "state": "ble_advertisement_confirmed",
                "method": "test_gateway",
            }
        except Exception as gateway_error:
            if usb_error:
                raise RuntimeError(
                    f"USB control failed ({safe_error(usb_error)}); "
                    f"test gateway control failed ({safe_error(gateway_error)})"
                ) from gateway_error
            raise

    if usb_error:
        raise usb_error
    raise RuntimeError(
        "No device USB serial port is available and no test gateway is configured"
    )


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

    def reprovision(self, device_id: str, service_name: str) -> dict[str, Any]:
        if not self._operation_lock.acquire(blocking=False):
            raise BusyOperationError()
        try:
            return request_ble_reprovision(device_id, service_name)
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

    async def _send_vox_config(self, transport: Any, security: Any,
                               device_id: str, gateway_url: str,
                               device_secret: str) -> dict[str, Any]:
        request_json = json.dumps({
            "deviceId": device_id,
            "gatewayUrl": gateway_url,
            "deviceSecret": device_secret,
        }, separators=(",", ":")).encode("utf-8")
        request = security.encrypt_data(request_json).decode("latin-1")
        response = await transport.send_data("vox-config", request)
        decrypted = security.decrypt_data(str_to_bytes(response))
        result = json.loads(decrypted.decode("utf-8"))
        return validate_vox_config_response(result, device_id)

    async def _wait_for_cloud_online(self, device_id: str,
                                     gateway_url: str) -> str:
        if not targets_test_gateway(gateway_url):
            return "NOT_OBSERVED"
        deadline = time.monotonic() + CLOUD_ONLINE_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            response = await asyncio.to_thread(
                _gateway_json, "GET", "/api/status", None,
            )
            devices = response.get("devices", [])
            if any(str(item.get("device_id") or "") == device_id
                   for item in devices if isinstance(item, dict)):
                return "DEVICE_ONLINE"
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
        gateway_url = payload["gatewayUrl"]
        device_secret = payload["deviceSecret"]
        contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
        service_uuid = contract["transport"]["primaryServiceUuid"]
        transport = None
        wifi_ready = False

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

            cloud_result = await self._send_vox_config(
                transport, security, username, gateway_url, device_secret,
            )
            self.update(job_id, cloudConfig={
                "state": cloud_result.get("state"),
                "deviceId": cloud_result.get("deviceId"),
                "gatewayUrl": gateway_url,
            })
            self.event(job_id, "vox-config", "Dynamic cloud configuration stored",
                       gateway_url)

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
        self.event(job_id, "cloud", "Observing device cloud session",
                   "optional after WIFI_READY")
        cloud_state = await self._wait_for_cloud_online(username, gateway_url)
        if cloud_state == "DEVICE_ONLINE":
            self.update(job_id, state="success", result="SUCCESS",
                        milestone="WIFI_READY", cloudState=cloud_state)
            self.event(job_id, "success", "Wi-Fi provisioning completed; device is online",
                       "WIFI_READY + DEVICE_ONLINE")
            return
        self.update(job_id, state="success", result="SUCCESS",
                    milestone="WIFI_READY", cloudState=cloud_state)
        self.event(job_id, "cloud-warning",
                   "Wi-Fi is ready; cloud state is reported separately",
                   cloud_state)
        self.event(job_id, "success", "Wi-Fi provisioning completed",
                   f"WIFI_READY / cloud={cloud_state}")


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
        "VOX_CONFIG_INVALID",
        "VOX_CONFIG_PERSIST_FAILED",
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
                "testGatewayConfigured": bool(TEST_GATEWAY_HTTP_URL and TEST_GATEWAY_TOKEN),
                "testGateway": TEST_GATEWAY_HTTP_URL or None,
                "testGatewayWsUrl": TEST_GATEWAY_WS_URL or None,
                "usbSerialControlAvailable": resolve_device_serial_port() is not None,
                "usbSerialPort": resolve_device_serial_port(),
                "reprovisionControlAvailable": (
                    resolve_device_serial_port() is not None or
                    bool(TEST_GATEWAY_HTTP_URL and TEST_GATEWAY_TOKEN)
                ),
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
        if path in (
                "/api/test-gateway/control-status",
                "/api/test-gateway/volume",
                "/api/test-gateway/audio-mode"):
            try:
                payload = self._read_json()
                if path.endswith("/control-status"):
                    values = validate_gateway_control_payload(payload)
                    payload.clear()
                    self._json(
                        HTTPStatus.OK,
                        get_test_gateway_control_status(values["deviceId"], values["gatewayUrl"]),
                    )
                elif path.endswith("/volume"):
                    values = validate_volume_payload(payload)
                    payload.clear()
                    self._json(HTTPStatus.OK, set_test_gateway_volume(values))
                else:
                    values = validate_audio_mode_payload(payload)
                    payload.clear()
                    self._json(HTTPStatus.OK, set_test_gateway_audio_mode(values))
            except GatewayControlError as exc:
                self._json(gateway_control_http_status(exc.code), {
                    "ok": False,
                    "code": exc.code,
                    "error": safe_error(exc),
                })
            except (ValueError, json.JSONDecodeError) as exc:
                self._json(HTTPStatus.BAD_REQUEST, {
                    "ok": False,
                    "code": "INVALID_REQUEST",
                    "error": safe_error(exc),
                })
            except Exception:
                self._json(HTTPStatus.BAD_GATEWAY, {
                    "ok": False,
                    "code": "INTERNAL_ERROR",
                    "error": "Test gateway control failed",
                })
            return
        if path == "/api/reprovision":
            try:
                payload = self._read_json()
                values = validate_reprovision_payload(payload)
                payload.clear()
                self._json(HTTPStatus.ACCEPTED,
                           JOBS.reprovision(values["deviceId"], values["serviceName"]))
            except BusyOperationError as exc:
                self._json(HTTPStatus.CONFLICT, {"error": safe_error(exc), "result": "BUSY"})
            except (ValueError, json.JSONDecodeError) as exc:
                self._json(HTTPStatus.BAD_REQUEST, {"error": safe_error(exc)})
            except Exception as exc:
                self._json(HTTPStatus.BAD_GATEWAY, {"error": safe_error(exc)})
            return
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
