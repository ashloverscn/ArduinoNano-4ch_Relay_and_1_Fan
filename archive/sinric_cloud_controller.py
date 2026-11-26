import json
import socket
import threading
import tkinter as tk
from tkinter import scrolledtext, ttk
import urllib.error
import urllib.request

BASE_URL = "https://api.sinric.pro/api/v1"

# Credentials & discovered device IDs
DEFAULT_API_KEY = "53c90327-01dd-48f2-827d-b3960ef04b37"
DEFAULT_APP_KEY = "167aca6d-6a18-4130-87a0-4eb35903e60b"
DEFAULT_APP_SECRET = "fc5c8d46-ac77-4f95-86ea-a4720be13ab6-ed999d04-2a0b-4335-ba52-7112f10d2f4d"

DEVICE_IDS = {
    "light1": "",  # Not present in discovered portal devices
    "light2": "6b01c637814fdde8896de12d",
    "light3": "",  # Not present in discovered portal devices
    "light4": "6b01c5f59ea140585cb89aab",  # Discovered as 'Fan' (SWITCH)
    "fan":    "6b0809957d04ce9fd668137c",  # Discovered as 'Fan Speed' (FAN)
}


class SinricRestClient:
    """Zero-dependency REST client with full HTTP error body reporting."""

    def __init__(self, api_key: str, timeout: float = 12.0) -> None:
        self.api_key = (api_key or "").strip()
        self.timeout = timeout

    def _headers(self) -> dict[str, str]:
        if not self.api_key:
            raise ValueError("API Key is empty. Paste your SinricPro API Key first.")
        return {
            "X-SINRIC-API-KEY": self.api_key,
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "Mozilla/5.0",
        }

    def _request_json(
        self, method: str, path: str, payload: dict | None = None
    ) -> tuple[int, str, dict | list | None]:
        url = f"{BASE_URL}{path}"
        data_bytes = None
        if payload is not None:
            data_bytes = json.dumps(payload).encode("utf-8")

        req = urllib.request.Request(
            url=url,
            data=data_bytes,
            headers=self._headers(),
            method=method.upper(),
        )

        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                status = resp.getcode() or 200
                raw_text = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            status = exc.code
            try:
                raw_text = exc.read().decode("utf-8", errors="replace")
            except Exception:
                raw_text = str(exc.reason)
            raise RuntimeError(
                f"HTTP {status} on {method.upper()} {path}\n"
                f"Response Body: {raw_text}\n"
                f"Sent Payload: {json.dumps(payload) if payload is not None else 'None'}"
            ) from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"Network/URL error reaching SinricPro: {exc.reason}") from exc
        except (socket.timeout, TimeoutError) as exc:
            raise RuntimeError("Request timed out while waiting for SinricPro.") from exc
        except OSError as exc:
            raise RuntimeError(f"Socket/OS network error: {exc}") from exc

        parsed = self._try_parse_json(raw_text)
        if isinstance(parsed, dict) and parsed.get("success") is False:
            msg = parsed.get("message", "SinricPro returned success=false")
            raise RuntimeError(f"SinricPro API error (HTTP {status}): {msg} | Raw: {raw_text}")

        return status, raw_text, parsed

    @staticmethod
    def _try_parse_json(text: str) -> dict | list | None:
        try:
            return json.loads(text)
        except Exception:
            return None

    def list_devices(self) -> list[dict]:
        _, _, parsed = self._request_json("GET", "/devices", None)
        if not isinstance(parsed, dict):
            raise RuntimeError("Unexpected non-JSON response from GET /devices.")
        devices = parsed.get("devices")
        if not isinstance(devices, list):
            return []
        return [d for d in devices if isinstance(d, dict)]

    def send_action(self, device_id: str, action: str, value_obj: dict) -> str:
        dev_id = (device_id or "").strip()
        if not dev_id:
            raise ValueError("Device ID is empty for this control.")
        if not action or not action.strip():
            raise ValueError("Action is empty.")

        payload = {
            "type": "request",
            "action": action.strip(),
            "value": json.dumps(value_obj, separators=(",", ":")),
        }
        status, raw_text, _ = self._request_json("POST", f"/devices/{dev_id}/action", payload)
        return f"HTTP {status}: {raw_text}"


class CrashProofSinricGUI:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("SinricPro ESP32 Controller (Crash-Proof)")
        self.root.geometry("640x600")
        self.root.minsize(580, 520)

        self._lock = threading.Lock()
        self._busy = False
        self._slider_after_id: str | None = None
        self._last_sent_speed: int | None = None
        self._action_buttons: list[ttk.Button] = []

        self.api_key_var = tk.StringVar(value=DEFAULT_API_KEY)
        self.fan_speed_var = tk.IntVar(value=0)
        self.device_vars = {k: tk.StringVar(value=v) for k, v in DEVICE_IDS.items()}
        self.discovered_devices: dict[str, dict] = {}

        self._build_ui()
        self._install_global_tk_exception_handler()

    def _install_global_tk_exception_handler(self) -> None:
        def handle_tk_callback_exception(exc_type, exc_value, _exc_tb):
            self._append_log(f"[UI Exception Caught] {exc_type.__name__}: {exc_value}")
            self._set_status(f"Recovered from UI error: {exc_value}")

        self.root.report_callback_exception = handle_tk_callback_exception

    def _build_ui(self) -> None:
        pad = {"padx": 8, "pady": 4}

        # 1. API Key & Device Discovery
        auth_frame = ttk.LabelFrame(self.root, text="SinricPro REST API Key & Discovery")
        auth_frame.pack(fill="x", padx=10, pady=(8, 4))

        ttk.Label(auth_frame, text="API Key:").grid(row=0, column=0, sticky="w", **pad)
        ttk.Entry(auth_frame, textvariable=self.api_key_var, show="*", width=46).grid(
            row=0, column=1, sticky="ew", **pad
        )
        btn_load = ttk.Button(auth_frame, text="Load Devices", command=self._on_load_devices)
        btn_load.grid(row=0, column=2, **pad)
        self._action_buttons.append(btn_load)

        ttk.Label(auth_frame, text="Portal Devices:").grid(row=1, column=0, sticky="w", **pad)
        self.cmb_devices = ttk.Combobox(auth_frame, state="readonly", width=43)
        self.cmb_devices.grid(row=1, column=1, sticky="ew", **pad)
        self.cmb_devices.bind("<<ComboboxSelected>>", self._on_device_combo_select)

        btn_use_id = ttk.Button(auth_frame, text="Fill Selected ID", command=self._on_fill_selected_id)
        btn_use_id.grid(row=1, column=2, **pad)
        self._action_buttons.append(btn_use_id)

        # 2. Target Device IDs
        ids_frame = ttk.LabelFrame(self.root, text="Target Device IDs")
        ids_frame.pack(fill="x", padx=10, pady=4)

        field_specs = [
            ("light1", "Light 1:"),
            ("light2", "Light 2:"),
            ("light3", "Light 3:"),
            ("light4", "Light 4:"),
            ("fan", "Fan Speed:"),
        ]
        for idx, (key, label_text) in enumerate(field_specs):
            r = idx // 2
            c = (idx % 2) * 2
            ttk.Label(ids_frame, text=label_text).grid(row=r, column=c, sticky="w", padx=(8, 4), pady=2)
            ttk.Entry(ids_frame, textvariable=self.device_vars[key], width=28).grid(
                row=r, column=c + 1, sticky="ew", padx=(0, 8), pady=2
            )

        # 3. Lights / Switches Controls
        lights_frame = ttk.LabelFrame(self.root, text="Relays / Switches (setPowerState)")
        lights_frame.pack(fill="x", padx=10, pady=4)

        for i in range(1, 5):
            key = f"light{i}"
            row = ttk.Frame(lights_frame)
            row.pack(fill="x", padx=8, pady=2)

            ttk.Label(row, text=f"Slot {i} ({key})", width=14).pack(side="left")

            b_on = ttk.Button(
                row,
                text="ON",
                command=lambda k=key: self._start_task(
                    f"{k.upper()} -> On",
                    lambda dev_key=k: self._make_client().send_action(
                        self.device_vars[dev_key].get(),
                        "setPowerState",
                        {"state": "On"},
                    ),
                ),
            )
            b_on.pack(side="left", padx=4)

            b_off = ttk.Button(
                row,
                text="OFF",
                command=lambda k=key: self._start_task(
                    f"{k.upper()} -> Off",
                    lambda dev_key=k: self._make_client().send_action(
                        self.device_vars[dev_key].get(),
                        "setPowerState",
                        {"state": "Off"},
                    ),
                ),
            )
            b_off.pack(side="left", padx=4)
            self._action_buttons.extend([b_on, b_off])

        # 4. Fan / Dimmer Controls (0-9, auto-updates on slider move)
        fan_frame = ttk.LabelFrame(self.root, text="Fan Speed (0-9 -> Auto-Updates on Slider)")
        fan_frame.pack(fill="x", padx=10, pady=4)

        fan_row = ttk.Frame(fan_frame)
        fan_row.pack(fill="x", padx=8, pady=4)

        b_fan_on = ttk.Button(
            fan_row,
            text="Fan ON",
            command=lambda: self._start_task(
                "FAN -> On",
                lambda: self._make_client().send_action(
                    self.device_vars["fan"].get(),
                    "setPowerState",
                    {"state": "On"},
                ),
            ),
        )
        b_fan_on.pack(side="left", padx=(0, 4))

        b_fan_off = ttk.Button(
            fan_row,
            text="Fan OFF",
            command=lambda: self._start_task(
                "FAN -> Off",
                lambda: self._make_client().send_action(
                    self.device_vars["fan"].get(),
                    "setPowerState",
                    {"state": "Off"},
                ),
            ),
        )
        b_fan_off.pack(side="left", padx=4)

        self.lbl_speed = ttk.Label(fan_row, text="Speed: 0", width=9)
        scale = ttk.Scale(
            fan_row,
            from_=0,
            to=9,
            orient="horizontal",
            variable=self.fan_speed_var,
            command=self._on_slider_move,
        )
        scale.pack(side="left", fill="x", expand=True, padx=6)
        self.lbl_speed.pack(side="left")
        self._action_buttons.extend([b_fan_on, b_fan_off])

        # 5. Status & Diagnostic Console Log
        self.status_var = tk.StringVar(value="Ready.")
        ttk.Label(self.root, textvariable=self.status_var, wraplength=610).pack(
            fill="x", padx=12, pady=(4, 2)
        )

        log_frame = ttk.LabelFrame(self.root, text="Response & Diagnostic Log")
        log_frame.pack(fill="both", expand=True, padx=10, pady=(2, 8))

        self.log_box = scrolledtext.ScrolledText(log_frame, height=8, wrap=tk.WORD, state="normal")
        self.log_box.pack(fill="both", expand=True, padx=6, pady=6)

    def _safe_speed(self, raw_value=None) -> int:
        source = self.fan_speed_var.get() if raw_value is None else raw_value
        try:
            val = int(round(float(source)))
        except Exception:
            val = 0
        clamped = max(0, min(9, val))
        self.fan_speed_var.set(clamped)
        return clamped

    def _on_slider_move(self, value: str) -> None:
        speed = self._safe_speed(value)
        self.lbl_speed.config(text=f"Speed: {speed}")

        if self._slider_after_id is not None:
            try:
                self.root.after_cancel(self._slider_after_id)
            except Exception:
                pass
            self._slider_after_id = None

        self._slider_after_id = self.root.after(
            180, lambda target_speed=speed: self._commit_slider_speed(target_speed)
        )

    def _commit_slider_speed(self, target_speed: int) -> None:
        self._slider_after_id = None
        if target_speed == self._last_sent_speed:
            return

        range_val = target_speed + 1  # Maps 0..9 dimmer level to 1..10 rangeValue

        def task() -> str:
            res = self._make_client().send_action(
                self.device_vars["fan"].get(),
                "setRangeValue",
                {"rangeValue": range_val},
            )
            self._last_sent_speed = target_speed
            return res

        self._start_task(f"FAN auto-speed {target_speed} (rangeValue={range_val})", task)

    def _make_client(self) -> SinricRestClient:
        return SinricRestClient(self.api_key_var.get())

    def _set_busy(self, is_busy: bool) -> None:
        with self._lock:
            self._busy = is_busy
        state = "disabled" if is_busy else "normal"
        for btn in self._action_buttons:
            try:
                btn.config(state=state)
            except Exception:
                pass

    def _set_status(self, text: str) -> None:
        try:
            self.root.after(0, lambda t=str(text): self.status_var.set(t))
        except Exception:
            pass

    def _append_log(self, text: str) -> None:
        def write():
            try:
                self.log_box.insert(tk.END, str(text).rstrip() + "\n")
                self.log_box.see(tk.END)
            except Exception:
                pass

        try:
            self.root.after(0, write)
        except Exception:
            pass

    def _on_load_devices(self) -> None:
        def load_task() -> str:
            devices = self._make_client().list_devices()
            mapping: dict[str, dict] = {}
            labels: list[str] = []
            lines: list[str] = [f"Discovered {len(devices)} device(s):"]

            for idx, dev in enumerate(devices, start=1):
                name = str(dev.get("name") or f"Device {idx}")
                dev_id = str(dev.get("id") or dev.get("_id") or "")
                online = dev.get("isOnline")
                product = dev.get("product") if isinstance(dev.get("product"), dict) else {}
                ptype = product.get("code") or dev.get("type") or "unknown"
                actions = product.get("actions") if isinstance(product.get("actions"), list) else []
                label = f"{name} [{dev_id}]"
                mapping[label] = dev
                labels.append(label)
                lines.append(
                    f"  - {name} | id={dev_id} | online={online} | type={ptype} | actions={actions}"
                )

            def apply_to_ui():
                self.discovered_devices = mapping
                self.cmb_devices["values"] = labels
                if labels:
                    self.cmb_devices.current(0)

            try:
                self.root.after(0, apply_to_ui)
            except Exception:
                pass

            return "\n".join(lines)

        self._start_task("GET /devices", load_task)

    def _on_device_combo_select(self, _event=None) -> None:
        selected = self.cmb_devices.get()
        dev = self.discovered_devices.get(selected)
        if isinstance(dev, dict):
            product = dev.get("product") if isinstance(dev.get("product"), dict) else {}
            actions = product.get("actions") if isinstance(product.get("actions"), list) else []
            self._append_log(f"Selected '{selected}' -> supported actions: {actions}")

    def _on_fill_selected_id(self) -> None:
        selected = self.cmb_devices.get()
        dev = self.discovered_devices.get(selected)
        if not isinstance(dev, dict):
            self._set_status("No discovered device selected. Click 'Load Devices' first.")
            return
        dev_id = str(dev.get("id") or dev.get("_id") or "").strip()
        if not dev_id:
            self._set_status("Selected device has no ID.")
            return

        for key in ("light1", "light2", "light3", "light4", "fan"):
            if not self.device_vars[key].get().strip():
                self.device_vars[key].set(dev_id)
                self._set_status(f"Filled {key} with {dev_id}")
                return

        self.device_vars["light1"].set(dev_id)
        self._set_status(f"Filled light1 with {dev_id}")

    def _start_task(self, label: str, fn) -> None:
        with self._lock:
            if self._busy:
                self._set_status("Busy: wait for current request to finish.")
                return
            self._busy = True

        self._set_busy(True)
        self._set_status(f"Running: {label}...")
        self._append_log(f"--- {label} ---")

        def worker() -> None:
            try:
                result = fn()
                self._append_log(str(result))
                self._set_status(f"OK: {label}")
            except Exception as exc:
                err_msg = f"ERROR [{label}]: {exc}"
                self._append_log(err_msg)
                self._set_status(f"Failed ({label}) - see log below.")
            finally:
                try:
                    self.root.after(0, lambda: self._set_busy(False))
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()


def main() -> None:
    try:
        root = tk.Tk()
        CrashProofSinricGUI(root)
        root.mainloop()
    except Exception as exc:
        print(f"Fatal startup error: {exc}")


if __name__ == "__main__":
    main()
