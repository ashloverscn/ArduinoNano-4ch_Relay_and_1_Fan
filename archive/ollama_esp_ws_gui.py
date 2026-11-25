import asyncio
import io
import json
import math
import os
import queue
import re
import struct
import threading
import time
import tkinter as tk
import wave
from pathlib import Path
from tkinter import messagebox, scrolledtext, ttk

import pyttsx3
import sounddevice as sd
import websockets
from openai import OpenAI

DEFAULT_OLLAMA_URL = "http://sasoftwares-lab:11434/v1"
DEFAULT_OLLAMA_MODEL = "qwen2.5-coder:3b"
AVAILABLE_OLLAMA_MODELS = [
    "qwen2.5-coder:3b",
    "qwen2.5-coder:1.5b",
    "qwen2.5-coder:7b",
    "qwen2.5-coder:14b",
    "qwen2.5-coder:32b",
    "qwen2.5:3b",
    "llama3.2:3b",
    "llama3.2:1b",
    "qwen3:1.7b",
    "qwen3:4b",
    "qwen3.5:2b",
    "qwen3.5:4b",
    "gemma3:4b",
    "gemma4:e4b",
    "lfm3:1.7b",
    "deepseek-r1:1.5b",
    "deepseek-r1:7b",
    "mistral:7b",
]
FALLBACK_OLLAMA_MODELS = [
    "qwen2.5-coder:3b",
    "qwen2.5-coder:1.5b",
    "qwen2.5:3b",
    "llama3.2:3b",
]

SETTINGS_FILE = Path(__file__).with_name("ollama_esp_settings.json")

MIC_SAMPLE_RATE = 16000
MAX_RECORD_SECONDS = 6

_whisper_model = None
_whisper_lock = threading.Lock()


def load_saved_settings() -> dict:
    try:
        if SETTINGS_FILE.exists():
            data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            if isinstance(data, dict):
                return data
    except Exception:
        pass
    return {}


def save_saved_settings(data: dict):
    try:
        SETTINGS_FILE.write_text(json.dumps(data, indent=2), encoding="utf-8")
    except Exception:
        pass


class SpeechWorker:
    """Single background worker for short Windows TTS replies so calls never overlap."""

    def __init__(self, log_fn=None):
        self.log_fn = log_fn
        self._queue: queue.Queue[str] = queue.Queue()
        self._stop_event = threading.Event()
        self._thread = threading.Thread(target=self._worker, daemon=True)
        self._thread.start()

    def speak(self, text: str, max_chars: int = 140):
        cleaned = " ".join((text or "").replace("*", "").replace("#", "").split()).strip()
        if not cleaned:
            return
        if len(cleaned) > max_chars:
            cleaned = cleaned[:max_chars].rsplit(" ", 1)[0].strip() + "."
        self._queue.put(cleaned)

    def stop(self):
        self._stop_event.set()
        self._queue.put("")

    def _worker(self):
        engine = None
        while not self._stop_event.is_set():
            text = self._queue.get()
            if self._stop_event.is_set() or not text:
                continue
            try:
                if engine is None:
                    engine = pyttsx3.init()
                    engine.setProperty("rate", 175)
                engine.say(text)
                engine.runAndWait()
            except Exception as e:
                engine = None
                if self.log_fn:
                    self.log_fn(f"[TTS Error] {e}\n")


class ESPWebSocketClient:
    """Persistent WebSocket client with immediate JSON auth and thread-safe command sending."""

    def __init__(
        self,
        esp_ip: str = "192.168.29.110",
        ws_port: int = 81,
        web_user: str = "admin",
        web_pass: str = "ThunderSno0w@,,#",
        on_log=None,
        on_status_change=None,
        on_state_change=None,
        hide_console_echo: bool = True,
        pending_grace_sec: float = 0.65,
    ):
        self.esp_ip = esp_ip
        self.ws_port = ws_port
        self.web_user = web_user
        self.web_pass = web_pass
        self.on_log = on_log
        self.on_status_change = on_status_change
        self.on_state_change = on_state_change
        self.hide_console_echo = hide_console_echo
        self.pending_grace_sec = pending_grace_sec

        self._ws = None
        self._loop = None
        self._thread = None
        self._stop_event = threading.Event()
        self._connected_event = threading.Event()
        self._send_lock = threading.Lock()
        self._state_lock = threading.Lock()

        self.relay_states = [False, False, False, False]
        self.dimmer_level = 0
        self.last_feedback = {}
        self._last_logged_raw = ""

        # Maps relay index 0..3 -> (desired_bool, deadline_monotonic)
        self._pending_relays: dict[int, tuple[bool, float]] = {}
        # (desired_level, deadline_monotonic) or None
        self._pending_dimmer: tuple[int, float] | None = None

    def _log(self, msg: str):
        if self.on_log:
            self.on_log(msg)
        else:
            print(msg, end="")

    def _set_status(self, connected: bool):
        if self.on_status_change:
            self.on_status_change(connected)

    def _notify_state(self):
        if self.on_state_change:
            with self._state_lock:
                r_copy = list(self.relay_states)
                d_copy = int(self.dimmer_level)
            self.on_state_change(r_copy, d_copy)

    def start(self, timeout: float = 5.0) -> bool:
        self.stop()
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()
        return self._connected_event.wait(timeout=timeout)

    def stop(self):
        self._stop_event.set()
        if self._loop and self._loop.is_running():
            if self._ws:
                try:
                    asyncio.run_coroutine_threadsafe(self._ws.close(), self._loop)
                except Exception:
                    pass
            try:
                self._loop.call_soon_threadsafe(self._loop.stop)
            except Exception:
                pass
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        self._connected_event.clear()
        self._set_status(False)

    def _run_loop(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._client_handler())
        finally:
            self._connected_event.clear()
            self._set_status(False)
            self._loop.close()

    async def _client_handler(self):
        uri = f"ws://{self.esp_ip}:{self.ws_port}/"
        while not self._stop_event.is_set():
            try:
                async with websockets.connect(uri, ping_interval=20, ping_timeout=10) as websocket:
                    self._ws = websocket
                    self._connected_event.set()
                    self._set_status(True)
                    self._log(f">> Connected to {uri}. Sending auth...\n")

                    auth_payload = json.dumps({
                        "auth": {
                            "user": self.web_user,
                            "pass": self.web_pass,
                        }
                    })
                    await websocket.send(auth_payload)
                    await websocket.send(json.dumps({"command": "STATUS"}))

                    async for message in websocket:
                        self._parse_feedback(message)
                        if self._stop_event.is_set():
                            break
            except Exception as e:
                self._connected_event.clear()
                self._ws = None
                self._set_status(False)
                if not self._stop_event.is_set():
                    self._log(f">> WS error/disconnected ({e}), retrying in 2s...\n")
                    await asyncio.sleep(2.0)

    @staticmethod
    def _parse_bool_state(val) -> bool | None:
        if isinstance(val, bool):
            return val
        if isinstance(val, (int, float)):
            return bool(val)
        if isinstance(val, str):
            norm = val.strip().upper()
            if norm in ("ON", "TRUE", "1"):
                return True
            if norm in ("OFF", "FALSE", "0"):
                return False
        return None

    def _register_pending_command(self, cmd: str):
        norm = re.sub(r"\s+", " ", (cmd or "").strip().upper())
        deadline = time.monotonic() + self.pending_grace_sec

        with self._state_lock:
            m_pwr = re.fullmatch(r"POWER([1-4])\s+(ON|OFF)", norm)
            if m_pwr:
                idx = int(m_pwr.group(1)) - 1
                self._pending_relays[idx] = (m_pwr.group(2) == "ON", deadline)
                return

            if norm == "ALL_ON":
                for idx in range(4):
                    self._pending_relays[idx] = (True, deadline)
                return

            if norm == "ALL_OFF":
                for idx in range(4):
                    self._pending_relays[idx] = (False, deadline)
                return

            m_dim = re.fullmatch(r"DIMMER:([0-9])", norm)
            if m_dim:
                lvl = max(0, min(9, int(m_dim.group(1))))
                self._pending_dimmer = (lvl, deadline)
                return

            if norm == "STATUS":
                self._pending_relays.clear()
                self._pending_dimmer = None

    def _apply_relay_reading(self, idx: int, incoming_on: bool, now: float) -> bool:
        pending = self._pending_relays.get(idx)
        if pending is not None:
            desired, deadline = pending
            if now <= deadline and incoming_on != desired:
                return False
            del self._pending_relays[idx]

        if self.relay_states[idx] != incoming_on:
            self.relay_states[idx] = incoming_on
            return True
        return False

    def _apply_dimmer_reading(self, incoming_lvl: int, now: float) -> bool:
        clamped = max(0, min(9, int(incoming_lvl)))
        if self._pending_dimmer is not None:
            desired, deadline = self._pending_dimmer
            if now <= deadline and clamped != desired:
                return False
            self._pending_dimmer = None

        if self.dimmer_level != clamped:
            self.dimmer_level = clamped
            return True
        return False

    def _parse_feedback(self, raw_msg: str):
        trimmed = (raw_msg or "").strip()
        try:
            data = json.loads(trimmed)
        except json.JSONDecodeError:
            self._log(f"<< {trimmed}\n")
            return

        if not isinstance(data, dict):
            return

        if self.hide_console_echo and data.get("type") == "console_log":
            return

        if trimmed != self._last_logged_raw:
            self._last_logged_raw = trimmed
            self._log(f"<< {trimmed}\n")

        now = time.monotonic()
        state_updated = False

        with self._state_lock:
            self.last_feedback = data

            for i in range(1, 5):
                key = f"POWER{i}"
                if key in data:
                    parsed = self._parse_bool_state(data[key])
                    if parsed is not None:
                        if self._apply_relay_reading(i - 1, parsed, now):
                            state_updated = True

            if "Dimmer" in data:
                try:
                    if self._apply_dimmer_reading(int(data["Dimmer"]), now):
                        state_updated = True
                except (ValueError, TypeError):
                    pass

            if data.get("type") == "state_update":
                if isinstance(data.get("relays"), list):
                    for i, state in enumerate(data["relays"][:4]):
                        parsed = self._parse_bool_state(state)
                        if parsed is not None:
                            if self._apply_relay_reading(i, parsed, now):
                                state_updated = True
                if "dimmer" in data:
                    try:
                        if self._apply_dimmer_reading(int(data["dimmer"]), now):
                            state_updated = True
                    except (ValueError, TypeError):
                        pass

        if state_updated:
            self._notify_state()

    def send_command(self, cmd: str, wait_feedback: float = 0.25) -> str:
        norm_cmd = re.sub(r"\s+", " ", (cmd or "").strip().upper())
        if not self._connected_event.is_set() or not self._ws or not self._loop:
            msg = f"Failed to send '{norm_cmd}': ESP WebSocket is not connected."
            self._log(f">> {msg}\n")
            return msg

        self._register_pending_command(norm_cmd)
        payload = json.dumps({"command": norm_cmd})
        with self._send_lock:
            try:
                fut = asyncio.run_coroutine_threadsafe(self._ws.send(payload), self._loop)
                fut.result(timeout=3.0)
                self._log(f">> {norm_cmd}\n")
                if wait_feedback > 0:
                    time.sleep(wait_feedback)
                with self._state_lock:
                    r_snap = list(self.relay_states)
                    d_snap = int(self.dimmer_level)
                return (
                    f"Sent '{norm_cmd}'. Current state -> "
                    f"relays={r_snap}, dimmer={d_snap}"
                )
            except Exception as e:
                err = f"Error sending '{norm_cmd}': {e}"
                self._log(f">> {err}\n")
                return err


AUTONOMOUS_RESPONSE_FORMAT = {
    "type": "json_schema",
    "json_schema": {
        "name": "smart_room_plan",
        "strict": True,
        "schema": {
            "type": "object",
            "properties": {
                "actions": {
                    "type": "array",
                    "description": "Ordered list of hardware actions to execute on the ESP controller.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "type": {
                                "type": "string",
                                "enum": [
                                    "set_relay",
                                    "set_dimmer",
                                    "set_all_relays",
                                    "get_status",
                                ],
                            },
                            "channel": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 4,
                                "description": "Relay channel 1..4 (used when type is set_relay).",
                            },
                            "turn_on": {
                                "type": "boolean",
                                "description": "True to turn ON, False to turn OFF (used for set_relay and set_all_relays).",
                            },
                            "level": {
                                "type": "integer",
                                "minimum": 0,
                                "maximum": 9,
                                "description": "Fan speed 0..9 (used ONLY when type is set_dimmer).",
                            },
                        },
                        "required": ["type"],
                        "additionalProperties": False,
                    },
                },
                "reply": {
                    "type": "string",
                    "description": "Natural, concise confirmation under 15 words matching the actual actions.",
                },
            },
            "required": ["actions", "reply"],
            "additionalProperties": False,
        },
    },
}


def resolve_ollama_url(url: str | None) -> str:
    raw = (url or "").strip() or os.environ.get("OLLAMA_BASE_URL", "").strip() or DEFAULT_OLLAMA_URL
    if not raw.startswith(("http://", "https://")):
        raw = f"http://{raw}"
    raw = raw.rstrip("/")
    if not raw.endswith("/v1"):
        raw = f"{raw}/v1"
    return raw


def _make_ollama_client(url: str | None) -> OpenAI:
    return OpenAI(
        base_url=resolve_ollama_url(url),
        api_key=os.environ.get("OLLAMA_API_KEY", "ollama").strip() or "ollama",
        timeout=25.0,
    )


def _coerce_turn_on(val, default: bool = False) -> bool:
    if isinstance(val, bool):
        return val
    if isinstance(val, (int, float)):
        return bool(val)
    if isinstance(val, str):
        norm = val.strip().lower()
        if norm in ("true", "on", "1", "yes"):
            return True
        if norm in ("false", "off", "0", "no"):
            return False
    return default


def execute_action(action: dict, esp: ESPWebSocketClient) -> str:
    act_type = str(action.get("type", "")).strip()
    if act_type == "set_relay":
        ch = int(action.get("channel", 1))
        ch = max(1, min(4, ch))
        turn_on = _coerce_turn_on(action.get("turn_on"), default=False)
        state = "ON" if turn_on else "OFF"
        return esp.send_command(f"POWER{ch} {state}")
    if act_type == "set_dimmer":
        lvl = max(0, min(9, int(action.get("level", 0))))
        return esp.send_command(f"DIMMER:{lvl}")
    if act_type == "set_all_relays":
        turn_on = _coerce_turn_on(action.get("turn_on"), default=False)
        cmd = "ALL_ON" if turn_on else "ALL_OFF"
        return esp.send_command(cmd)
    if act_type == "get_status":
        return esp.send_command("STATUS")
    return f"Ignored unknown action: {act_type}"


def _parse_structured_plan(raw_text: str) -> dict | None:
    if not raw_text:
        return None

    cleaned = re.sub(r"<think>.*?</think>", "", raw_text, flags=re.DOTALL | re.IGNORECASE).strip()
    cleaned = re.sub(r"^```(?:json)?\s*|\s*```$", "", cleaned, flags=re.IGNORECASE).strip()

    for candidate in (cleaned, raw_text.strip()):
        if not candidate:
            continue
        try:
            data = json.loads(candidate)
            if isinstance(data, dict) and isinstance(data.get("actions"), list):
                return data
        except json.JSONDecodeError:
            pass

        decoder = json.JSONDecoder()
        idx = candidate.find("{")
        while idx != -1:
            try:
                data, _ = decoder.raw_decode(candidate, idx)
                if isinstance(data, dict) and isinstance(data.get("actions"), list):
                    return data
            except json.JSONDecodeError:
                pass
            idx = candidate.find("{", idx + 1)
    return None


def pcm_to_wav_bytes(pcm_bytes: bytes, sample_rate: int = MIC_SAMPLE_RATE, channels: int = 1) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


def _pcm_rms(pcm_bytes: bytes) -> float:
    if len(pcm_bytes) < 2:
        return 0.0
    n = len(pcm_bytes) // 2
    samples = struct.unpack(f"<{n}h", pcm_bytes[: n * 2])
    return math.sqrt(sum(s * s for s in samples) / max(1, n))


def transcribe_wav_local(wav_bytes: bytes, log_fn=None) -> str:
    """Local STT via faster-whisper if installed, with SpeechRecognition fallback."""
    global _whisper_model
    try:
        if _whisper_model is None:
            with _whisper_lock:
                if _whisper_model is None:
                    from faster_whisper import WhisperModel  # type: ignore

                    _whisper_model = WhisperModel("tiny.en", device="cpu", compute_type="int8")
        segments, _ = _whisper_model.transcribe(
            io.BytesIO(wav_bytes),
            language="en",
            beam_size=1,
            vad_filter=True,
        )
        transcript = " ".join(s.text.strip() for s in segments).strip()
        if log_fn:
            log_fn(f"[Whisper-Local] {transcript or '(empty)'}\n")
        if transcript:
            return transcript
    except ImportError:
        pass
    except Exception as e:
        if log_fn:
            log_fn(f"[Whisper-Local Note] {e}\n")

    try:
        import speech_recognition as sr  # type: ignore

        rec = sr.Recognizer()
        with sr.AudioFile(io.BytesIO(wav_bytes)) as src:
            audio = rec.record(src)
        transcript = (rec.recognize_google(audio) or "").strip()
        if log_fn:
            log_fn(f"[STT] {transcript or '(empty)'}\n")
        return transcript
    except Exception as e:
        if log_fn:
            log_fn(f"[STT Error] {e}\n")
        return ""


def list_ollama_models(url: str | None) -> list[str]:
    client = _make_ollama_client(url)
    models = client.models.list()
    return [getattr(m, "id", "") for m in models.data if getattr(m, "id", "")]


def run_ollama_intent(
    prompt_text: str,
    esp: ESPWebSocketClient,
    ollama_url: str | None,
    model_name: str,
    log_fn=None,
) -> str:
    client = _make_ollama_client(ollama_url)
    primary = (model_name or "").strip() or DEFAULT_OLLAMA_MODEL
    models = [primary] + [m for m in FALLBACK_OLLAMA_MODELS if m != primary]

    live_state = {
        "relay_1": "ON" if esp.relay_states[0] else "OFF",
        "relay_2": "ON" if esp.relay_states[1] else "OFF",
        "relay_3": "ON" if esp.relay_states[2] else "OFF",
        "relay_4": "ON" if esp.relay_states[3] else "OFF",
        "dimmer_fan_speed": esp.dimmer_level,
    }

    system_prompt = (
        "You are an autonomous smart room controller AI. Convert the user's request into "
        "exact hardware actions based on the rules and examples below.\n"
        "Hardware rules:\n"
        "- Relays 1 to 4 are ON/OFF AC switches for lights and appliances (Relay 1 is primary room light; light N or channel N maps to relay N).\n"
        "- For 'turn off' / 'switch off' / 'off': set turn_on to false.\n"
        "- For 'turn on' / 'switch on' / 'on': set turn_on to true.\n"
        "- Dimmer (0 to 9) is strictly the AC fan speed regulator (0 = fan off, 1 = slowest, 9 = fastest).\n"
        "- Never use set_dimmer for lights; always use set_relay for lights or appliances.\n"
        "Examples:\n"
        "- User: 'Turn off light 2' -> {\"actions\":[{\"type\":\"set_relay\",\"channel\":2,\"turn_on\":false}],\"reply\":\"Light 2 turned off.\"}\n"
        "- User: 'Turn on light 3' -> {\"actions\":[{\"type\":\"set_relay\",\"channel\":3,\"turn_on\":true}],\"reply\":\"Light 3 turned on.\"}\n"
        "- User: 'Turn off bedroom light 2 and set fan speed to 6' -> "
        "{\"actions\":[{\"type\":\"set_relay\",\"channel\":2,\"turn_on\":false},{\"type\":\"set_dimmer\",\"level\":6}],\"reply\":\"Light 2 off, fan set to 6.\"}\n"
        f"Current live state: {json.dumps(live_state)}\n"
        "Return ONLY valid JSON matching the schema with an 'actions' list and a short 'reply' under 15 words."
    )

    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": prompt_text},
    ]

    last_err = None
    for m_id in models:
        try:
            resp = client.chat.completions.create(
                model=m_id,
                messages=messages,
                response_format=AUTONOMOUS_RESPONSE_FORMAT,
                temperature=0.0,
            )
            raw_content = (resp.choices[0].message.content or "").strip()
            plan = _parse_structured_plan(raw_content)
            if plan is None:
                raise ValueError(f"Unparseable model output: {raw_content}")

            for act in plan.get("actions", []):
                if isinstance(act, dict):
                    execute_action(act, esp)

            reply = str(plan.get("reply") or "").strip()
            return reply or f"Done. Relays: {esp.relay_states}, dimmer: {esp.dimmer_level}."
        except Exception as e:
            last_err = e
            if log_fn:
                log_fn(f"[Ollama {m_id} Error] {e}\n")

    raise last_err if last_err is not None else RuntimeError("All Ollama models failed.")


class OllamaESPControllerGUI:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Ollama Autonomous ESP WebSocket Voice & Hardware Controller")
        self.root.geometry("820x830")
        self.root.minsize(740, 750)

        self.saved_settings = load_saved_settings()

        self.is_running_prompt = False
        self.is_recording = False
        self._record_stop_event = threading.Event()
        self._record_thread = None

        self.relay_states = [False, False, False, False]
        self.relay_buttons = []
        self.last_sent_dimmer = 0

        self.speech_worker = SpeechWorker(log_fn=self.safe_log)
        self.esp_client = ESPWebSocketClient(
            on_log=self.safe_log,
            on_status_change=self.safe_status_update,
            on_state_change=self.safe_state_update,
        )

        self._build_ui()
        self._restore_saved_state()
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.connect_ws()

    def _restore_saved_state(self):
        saved_url = str(self.saved_settings.get("ollama_url") or "").strip()
        if saved_url:
            self.ollama_url_entry.delete(0, tk.END)
            self.ollama_url_entry.insert(0, saved_url)

        saved_model = str(self.saved_settings.get("selected_model") or "").strip()
        if saved_model:
            current_vals = list(self.model_combo["values"])
            if saved_model not in current_vals:
                self.model_combo["values"] = [saved_model] + current_vals
            self.model_combo.set(saved_model)
        else:
            self.model_combo.set(DEFAULT_OLLAMA_MODEL)

    def _persist_model_selection(self, *_args):
        model_val = self.model_combo.get().strip() or DEFAULT_OLLAMA_MODEL
        url_val = self.ollama_url_entry.get().strip() or resolve_ollama_url(None)
        self.saved_settings["selected_model"] = model_val
        self.saved_settings["ollama_url"] = url_val
        save_saved_settings(self.saved_settings)

    def _build_ui(self):
        pad = {"padx": 10, "pady": 5}

        # --- WebSocket Connection Frame ---
        ws_frame = ttk.LabelFrame(self.root, text="ESP WebSocket Connection", padding=8)
        ws_frame.pack(fill="x", **pad)

        ttk.Label(ws_frame, text="IP:").grid(row=0, column=0, sticky="w")
        self.ip_entry = ttk.Entry(ws_frame, width=16)
        self.ip_entry.insert(0, "192.168.29.110")
        self.ip_entry.grid(row=0, column=1, padx=(4, 10))

        ttk.Label(ws_frame, text="Port:").grid(row=0, column=2, sticky="w")
        self.port_entry = ttk.Entry(ws_frame, width=7)
        self.port_entry.insert(0, "81")
        self.port_entry.grid(row=0, column=3, padx=(4, 10))

        ttk.Label(ws_frame, text="User:").grid(row=0, column=4, sticky="w")
        self.user_entry = ttk.Entry(ws_frame, width=10)
        self.user_entry.insert(0, "admin")
        self.user_entry.grid(row=0, column=5, padx=(4, 10))

        ttk.Label(ws_frame, text="Pass:").grid(row=0, column=6, sticky="w")
        self.pass_entry = ttk.Entry(ws_frame, width=16, show="*")
        self.pass_entry.insert(0, "ThunderSno0w@,,#")
        self.pass_entry.grid(row=0, column=7, padx=(4, 10))

        self.ws_btn = ttk.Button(ws_frame, text="Reconnect", command=self.connect_ws)
        self.ws_btn.grid(row=0, column=8, padx=(4, 6))

        self.status_lbl = ttk.Label(ws_frame, text="Disconnected", foreground="#b00020")
        self.status_lbl.grid(row=0, column=9, sticky="w")

        # --- Relays Frame (Relays 1-4) ---
        relay_frame = ttk.LabelFrame(self.root, text="Relay Controls", padding=8)
        relay_frame.pack(fill="x", **pad)

        for i in range(1, 5):
            btn = tk.Button(
                relay_frame,
                text=f"Relay {i}\nOFF",
                font=("Arial", 10, "bold"),
                width=12,
                height=2,
                bg="#e0e0e0",
                fg="#000000",
                relief="raised",
                command=lambda r=i: self.toggle_relay(r),
            )
            btn.pack(side="left", expand=True, padx=5, pady=2)
            self.relay_buttons.append(btn)

        # --- Dimmer Frame (0-9 + MIN / Step - / Slider / Step + / MAX) ---
        dimmer_frame = ttk.LabelFrame(self.root, text="Dimmer Control (0-9)", padding=8)
        dimmer_frame.pack(fill="x", **pad)

        d_sub = ttk.Frame(dimmer_frame)
        d_sub.pack(fill="x", padx=5)

        ttk.Button(d_sub, text="MIN (0)", command=lambda: self.set_dimmer_direct(0)).pack(side="left", padx=4)
        ttk.Button(d_sub, text="Step -", command=self.step_dimmer_down).pack(side="left", padx=4)

        self.dimmer_slider = ttk.Scale(d_sub, from_=0, to=9, orient="horizontal")
        self.dimmer_slider.pack(side="left", expand=True, fill="x", padx=8, pady=4)
        self.dimmer_slider.bind("<ButtonRelease-1>", self.on_slider_release)

        self.dimmer_val_lbl = ttk.Label(d_sub, text="Level: 0", width=8, anchor="e")
        self.dimmer_val_lbl.pack(side="left", padx=(0, 6))

        ttk.Button(d_sub, text="Step +", command=self.step_dimmer_up).pack(side="left", padx=4)
        ttk.Button(d_sub, text="MAX (9)", command=lambda: self.set_dimmer_direct(9)).pack(side="left", padx=4)

        # --- Global & Utils Frame ---
        global_frame = ttk.LabelFrame(self.root, text="Global Commands & Status", padding=8)
        global_frame.pack(fill="x", **pad)

        g_sub = ttk.Frame(global_frame)
        g_sub.pack(fill="x")

        ttk.Button(g_sub, text="ALL ON", command=self.send_all_on).pack(side="left", expand=True, padx=5)
        ttk.Button(g_sub, text="ALL OFF", command=self.send_all_off).pack(side="left", expand=True, padx=5)
        ttk.Button(g_sub, text="Get Status", command=self.send_status_cmd).pack(side="left", expand=True, padx=5)

        # --- Ollama URL & Model Frame ---
        ollama_frame = ttk.LabelFrame(self.root, text="Ollama Configuration", padding=8)
        ollama_frame.pack(fill="x", **pad)

        ttk.Label(ollama_frame, text="Base URL:").pack(side="left")
        self.ollama_url_entry = ttk.Entry(ollama_frame, width=30)
        self.ollama_url_entry.insert(0, resolve_ollama_url(None))
        self.ollama_url_entry.pack(side="left", fill="x", expand=True, padx=(6, 10))
        self.ollama_url_entry.bind("<FocusOut>", self._persist_model_selection)
        self.ollama_url_entry.bind("<Return>", self._persist_model_selection)

        ttk.Label(ollama_frame, text="Model:").pack(side="left")
        self.model_combo = ttk.Combobox(
            ollama_frame,
            values=list(AVAILABLE_OLLAMA_MODELS),
            width=24,
            state="normal",
        )
        self.model_combo.set(DEFAULT_OLLAMA_MODEL)
        self.model_combo.pack(side="left", padx=(6, 6))
        self.model_combo.bind("<<ComboboxSelected>>", self._persist_model_selection)
        self.model_combo.bind("<FocusOut>", self._persist_model_selection)
        self.model_combo.bind("<Return>", self._persist_model_selection)

        ttk.Button(ollama_frame, text="List Models", command=self.check_models).pack(side="left")

        # --- Natural Language & Default Mic Frame ---
        prompt_frame = ttk.LabelFrame(self.root, text="Intent Command (Text or Default Mic)", padding=8)
        prompt_frame.pack(fill="x", **pad)

        self.prompt_entry = ttk.Entry(prompt_frame, font=("Arial", 10))
        self.prompt_entry.insert(0, "Turn on bedroom light 2 and set the fan speed to 6.")
        self.prompt_entry.pack(side="left", fill="x", expand=True, padx=(0, 8))
        self.prompt_entry.bind("<Return>", lambda _e: self.send_text_intent())

        self.send_btn = ttk.Button(prompt_frame, text="Send Intent", command=self.send_text_intent)
        self.send_btn.pack(side="left", padx=(0, 6))

        self.mic_btn = ttk.Button(prompt_frame, text="Speak (Default Mic)", command=self.toggle_voice_recording)
        self.mic_btn.pack(side="left")

        # --- Speaker Toggle ---
        quick_frame = ttk.Frame(self.root)
        quick_frame.pack(fill="x", padx=10, pady=(0, 4))

        self.speak_reply_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            quick_frame,
            text="Speak Short Reply (Default Speaker)",
            variable=self.speak_reply_var,
        ).pack(side="right")

        # --- Console / Assistant Output Log ---
        log_frame = ttk.LabelFrame(self.root, text="Assistant Response & WebSocket Log", padding=8)
        log_frame.pack(fill="both", expand=True, **pad)

        self.log_box = scrolledtext.ScrolledText(
            log_frame, height=11, state="disabled", font=("Consolas", 9), wrap="word"
        )
        self.log_box.pack(fill="both", expand=True)

        ttk.Button(log_frame, text="Clear Log", command=self.clear_log).pack(anchor="e", pady=(6, 0))

    # --- Manual Hardware Controls ---
    def toggle_relay(self, relay_num: int):
        idx = relay_num - 1
        target_state = not self.relay_states[idx]
        cmd = f"POWER{relay_num} {'ON' if target_state else 'OFF'}"
        threading.Thread(target=lambda: self.esp_client.send_command(cmd), daemon=True).start()

    def set_dimmer_direct(self, level: int):
        clamped = max(0, min(9, int(level)))
        self.dimmer_slider.set(clamped)
        self.dimmer_val_lbl.config(text=f"Level: {clamped}")
        self.last_sent_dimmer = clamped
        threading.Thread(target=lambda: self.esp_client.send_command(f"DIMMER:{clamped}"), daemon=True).start()

    def on_slider_release(self, _event):
        val_int = max(0, min(9, int(round(self.dimmer_slider.get()))))
        self.dimmer_slider.set(val_int)
        self.dimmer_val_lbl.config(text=f"Level: {val_int}")
        if val_int != self.last_sent_dimmer:
            self.last_sent_dimmer = val_int
            threading.Thread(target=lambda: self.esp_client.send_command(f"DIMMER:{val_int}"), daemon=True).start()

    def step_dimmer_up(self):
        current_val = int(round(self.dimmer_slider.get()))
        self.set_dimmer_direct(min(current_val + 1, 9))

    def step_dimmer_down(self):
        current_val = int(round(self.dimmer_slider.get()))
        self.set_dimmer_direct(max(current_val - 1, 0))

    def send_all_on(self):
        threading.Thread(target=lambda: self.esp_client.send_command("ALL_ON"), daemon=True).start()

    def send_all_off(self):
        threading.Thread(target=lambda: self.esp_client.send_command("ALL_OFF"), daemon=True).start()

    def send_status_cmd(self):
        threading.Thread(target=lambda: self.esp_client.send_command("STATUS"), daemon=True).start()

    def safe_state_update(self, relays: list[bool], dimmer: int):
        def _apply():
            for idx, is_on in enumerate(relays[:4]):
                self.relay_states[idx] = bool(is_on)
                btn = self.relay_buttons[idx]
                if self.relay_states[idx]:
                    btn.config(text=f"Relay {idx+1}\nON", bg="#4CAF50", fg="#FFFFFF")
                else:
                    btn.config(text=f"Relay {idx+1}\nOFF", bg="#e0e0e0", fg="#000000")

            clamped = max(0, min(9, int(dimmer)))
            self.last_sent_dimmer = clamped
            self.dimmer_slider.set(clamped)
            self.dimmer_val_lbl.config(text=f"Level: {clamped}")

        try:
            self.root.after(0, _apply)
        except Exception:
            pass

    # --- Voice & Ollama Intent Controls ---
    def toggle_voice_recording(self):
        if self.is_running_prompt:
            return
        if not self.is_recording:
            self._start_mic_capture()
        else:
            self._stop_mic_capture()

    def _start_mic_capture(self):
        self.is_recording = True
        self._record_stop_event.clear()
        self.send_btn.config(state="disabled")
        self.mic_btn.config(text="Stop & Send")
        self.safe_log(f"\n[Mic] Listening on default mic (up to {MAX_RECORD_SECONDS}s)...\n")

        def record_worker():
            frames = []
            block_dur = 0.1
            block_samples = int(MIC_SAMPLE_RATE * block_dur)
            start_t = time.monotonic()

            try:
                with sd.InputStream(
                    samplerate=MIC_SAMPLE_RATE,
                    channels=1,
                    dtype="int16",
                    blocksize=block_samples,
                ) as stream:
                    while not self._record_stop_event.is_set():
                        if time.monotonic() - start_t >= MAX_RECORD_SECONDS:
                            break
                        data, _overflow = stream.read(block_samples)
                        frames.append(data.tobytes())
            except Exception as e:
                self.safe_log(f"[Mic Error] {e}\n")
                self.root.after(0, self._reset_mic_ui)
                return

            raw_pcm = b"".join(frames)
            wav_bytes = pcm_to_wav_bytes(raw_pcm, sample_rate=MIC_SAMPLE_RATE, channels=1)
            self.root.after(0, self._reset_mic_ui)

            if len(frames) == 0 or _pcm_rms(raw_pcm) < 80:
                self.safe_log("[Mic] No audio captured or too quiet.\n")
                return

            self._persist_model_selection()
            ollama_url = self.ollama_url_entry.get().strip()
            model_name = self.model_combo.get().strip()

            def _voice_flow():
                transcript = transcribe_wav_local(wav_bytes, log_fn=self.safe_log)
                if not transcript:
                    return "Could not hear clear speech."
                return run_ollama_intent(transcript, self.esp_client, ollama_url, model_name, log_fn=self.safe_log)

            self._run_worker(label="[You - Voice]", fn=_voice_flow)

        self._record_thread = threading.Thread(target=record_worker, daemon=True)
        self._record_thread.start()

    def _stop_mic_capture(self):
        self._record_stop_event.set()

    def _reset_mic_ui(self):
        self.is_recording = False
        self.mic_btn.config(text="Speak (Default Mic)")
        if not self.is_running_prompt:
            self.send_btn.config(state="normal")

    def check_models(self):
        self._persist_model_selection()
        ollama_url = self.ollama_url_entry.get().strip()
        self.safe_log(f">> Fetching available Ollama models from {resolve_ollama_url(ollama_url)}...\n")

        def worker():
            try:
                models = list_ollama_models(ollama_url)
                if models:
                    def _update_combo():
                        current = self.model_combo.get().strip()
                        merged = []
                        for m in [DEFAULT_OLLAMA_MODEL] + models + list(AVAILABLE_OLLAMA_MODELS):
                            if m not in merged:
                                merged.append(m)
                        self.model_combo["values"] = merged
                        if current in merged:
                            self.model_combo.set(current)
                        elif DEFAULT_OLLAMA_MODEL in merged:
                            self.model_combo.set(DEFAULT_OLLAMA_MODEL)
                        else:
                            self.model_combo.set(merged[0])
                        self._persist_model_selection()

                    self.root.after(0, _update_combo)
                preview = ", ".join(models[:15])
                self.safe_log(f"[Ollama Models] {preview}\n")
            except Exception as e:
                self.safe_log(f"[Error listing models] {e}\n")

        threading.Thread(target=worker, daemon=True).start()

    def connect_ws(self):
        ip = self.ip_entry.get().strip()
        port_str = self.port_entry.get().strip()
        user = self.user_entry.get().strip()
        passwd = self.pass_entry.get()

        if not ip:
            messagebox.showerror("Missing IP", "Enter the ESP IP address.")
            return
        try:
            port = int(port_str)
        except ValueError:
            messagebox.showerror("Invalid Port", "WebSocket port must be a number.")
            return

        self.esp_client.esp_ip = ip
        self.esp_client.ws_port = port
        self.esp_client.web_user = user
        self.esp_client.web_pass = passwd
        self.safe_log(f">> Starting WebSocket connection to ws://{ip}:{port}/...\n")
        threading.Thread(target=lambda: self.esp_client.start(timeout=5.0), daemon=True).start()

    def send_text_intent(self):
        prompt = self.prompt_entry.get().strip()
        if not prompt or self.is_running_prompt or self.is_recording:
            return
        self._persist_model_selection()
        ollama_url = self.ollama_url_entry.get().strip()
        model_name = self.model_combo.get().strip()
        self._run_worker(
            label=f"[You] {prompt}",
            fn=lambda: run_ollama_intent(prompt, self.esp_client, ollama_url, model_name, log_fn=self.safe_log),
        )

    def _run_worker(self, label: str, fn):
        should_speak = bool(self.speak_reply_var.get())
        self.is_running_prompt = True
        self.send_btn.config(state="disabled")
        self.mic_btn.config(state="disabled")
        self.safe_log(f"\n{label}\n")

        def worker():
            try:
                reply = fn()
                self.safe_log(f"[Assistant] {reply}\n")
                if should_speak and reply:
                    self.speech_worker.speak(reply)
            except Exception as e:
                self.safe_log(f"[Error] {e}\n")
            finally:
                def _done():
                    self.is_running_prompt = False
                    if not self.is_recording:
                        self.send_btn.config(state="normal")
                    self.mic_btn.config(state="normal")

                try:
                    self.root.after(0, _done)
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()

    def safe_log(self, msg: str):
        def _append():
            try:
                self.log_box.config(state="normal")
                self.log_box.insert(tk.END, msg)
                self.log_box.see(tk.END)
                self.log_box.config(state="disabled")
            except Exception:
                pass

        try:
            self.root.after(0, _append)
        except Exception:
            pass

    def safe_status_update(self, connected: bool):
        def _update():
            try:
                if connected:
                    self.status_lbl.config(text="Connected", foreground="#1b5e20")
                else:
                    self.status_lbl.config(text="Disconnected", foreground="#b00020")
            except Exception:
                pass

        try:
            self.root.after(0, _update)
        except Exception:
            pass

    def clear_log(self):
        self.log_box.config(state="normal")
        self.log_box.delete("1.0", tk.END)
        self.log_box.config(state="disabled")

    def on_closing(self):
        self._persist_model_selection()
        self._record_stop_event.set()
        self.speech_worker.stop()
        self.esp_client.stop()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    app = OllamaESPControllerGUI(root)
    root.mainloop()