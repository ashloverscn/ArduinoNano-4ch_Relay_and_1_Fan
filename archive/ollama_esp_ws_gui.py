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
from tkinter import messagebox, scrolledtext, ttk

import pyttsx3
import sounddevice as sd
import websockets
from openai import OpenAI

DEFAULT_OLLAMA_URL = "http://sasoftwares-lab:11434/v1"
DEFAULT_OLLAMA_MODEL = "qwen2.5-coder:1.5b"
AVAILABLE_OLLAMA_MODELS = [
    "qwen2.5-coder:1.5b",
    "qwen2.5-coder:7b",
    "qwen2.5:3b",
    "llama3.2:3b",
]
FALLBACK_OLLAMA_MODELS = [
    "qwen2.5-coder:1.5b",
    "qwen2.5:3b",
    "llama3.2:3b",
]

MIC_SAMPLE_RATE = 16000
MAX_RECORD_SECONDS = 6

_whisper_model = None
_whisper_lock = threading.Lock()


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
    ):
        self.esp_ip = esp_ip
        self.ws_port = ws_port
        self.web_user = web_user
        self.web_pass = web_pass
        self.on_log = on_log
        self.on_status_change = on_status_change
        self.on_state_change = on_state_change
        self.hide_console_echo = hide_console_echo

        self._ws = None
        self._loop = None
        self._thread = None
        self._stop_event = threading.Event()
        self._connected_event = threading.Event()
        self._send_lock = threading.Lock()

        self.relay_states = [False, False, False, False]
        self.dimmer_level = 0
        self.last_feedback = {}

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
            self.on_state_change(list(self.relay_states), int(self.dimmer_level))

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

    def _parse_feedback(self, raw_msg: str):
        try:
            data = json.loads(raw_msg)
        except json.JSONDecodeError:
            self._log(f"<< {raw_msg}\n")
            return

        if self.hide_console_echo and data.get("type") == "console_log":
            return

        self._log(f"<< {raw_msg}\n")
        self.last_feedback = data
        state_updated = False

        for i in range(1, 5):
            key = f"POWER{i}"
            if key in data:
                new_val = (data[key] == "ON")
                if self.relay_states[i - 1] != new_val:
                    self.relay_states[i - 1] = new_val
                state_updated = True

        if "Dimmer" in data:
            try:
                new_lvl = max(0, min(9, int(data["Dimmer"])))
                self.dimmer_level = new_lvl
                state_updated = True
            except (ValueError, TypeError):
                pass

        if data.get("type") == "state_update":
            if "relays" in data and isinstance(data["relays"], list):
                for i, state in enumerate(data["relays"][:4]):
                    self.relay_states[i] = bool(state)
                    state_updated = True
            if "dimmer" in data:
                try:
                    self.dimmer_level = max(0, min(9, int(data["dimmer"])))
                    state_updated = True
                except (ValueError, TypeError):
                    pass

        if state_updated:
            self._notify_state()

    def send_command(self, cmd: str, wait_feedback: float = 0.25) -> str:
        if not self._connected_event.is_set() or not self._ws or not self._loop:
            msg = f"Failed to send '{cmd}': ESP WebSocket is not connected."
            self._log(f">> {msg}\n")
            return msg

        payload = json.dumps({"command": cmd})
        with self._send_lock:
            try:
                fut = asyncio.run_coroutine_threadsafe(self._ws.send(payload), self._loop)
                fut.result(timeout=3.0)
                self._log(f">> {cmd}\n")
                if wait_feedback > 0:
                    time.sleep(wait_feedback)
                return (
                    f"Sent '{cmd}'. Current state -> "
                    f"relays={self.relay_states}, dimmer={self.dimmer_level}"
                )
            except Exception as e:
                err = f"Error sending '{cmd}': {e}"
                self._log(f">> {err}\n")
                return err


OLLAMA_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "set_relay",
            "description": "Turn relay channel 1..4 ON or OFF.",
            "parameters": {
                "type": "object",
                "properties": {
                    "channel": {"type": "integer", "enum": [1, 2, 3, 4]},
                    "turn_on": {"type": "boolean"},
                },
                "required": ["channel", "turn_on"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "set_dimmer",
            "description": "Set dimmer or fan speed level from 0 (min/off) to 9 (max).",
            "parameters": {
                "type": "object",
                "properties": {
                    "level": {"type": "integer", "minimum": 0, "maximum": 9},
                },
                "required": ["level"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "set_all_relays",
            "description": "Turn all relays ON or OFF.",
            "parameters": {
                "type": "object",
                "properties": {
                    "turn_on": {"type": "boolean"},
                },
                "required": ["turn_on"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "get_device_status",
            "description": "Query current relay and dimmer status from the ESP.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
]


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
        timeout=20.0,
    )


def execute_tool_call(name: str, args: dict, esp: ESPWebSocketClient) -> str:
    if name == "set_relay":
        ch = int(args.get("channel", 0))
        if ch not in (1, 2, 3, 4):
            return "Invalid relay channel."
        state = "ON" if bool(args.get("turn_on")) else "OFF"
        return esp.send_command(f"POWER{ch} {state}")
    if name == "set_dimmer":
        lvl = max(0, min(9, int(args.get("level", 0))))
        return esp.send_command(f"DIMMER:{lvl}")
    if name == "set_all_relays":
        cmd = "ALL_ON" if bool(args.get("turn_on")) else "ALL_OFF"
        return esp.send_command(cmd)
    if name == "get_device_status":
        esp.send_command("STATUS")
        return json.dumps({"relays": esp.relay_states, "dimmer": esp.dimmer_level})
    return f"Unknown tool: {name}"


def _build_clean_assistant_message(msg) -> dict:
    clean_calls = []
    for tc in getattr(msg, "tool_calls", None) or []:
        clean_calls.append({
            "id": tc.id,
            "type": getattr(tc, "type", "function") or "function",
            "function": {
                "name": tc.function.name,
                "arguments": tc.function.arguments or "{}",
            },
        })
    entry = {
        "role": "assistant",
        "content": msg.content if msg.content is not None else "",
    }
    if clean_calls:
        entry["tool_calls"] = clean_calls
    return entry


def _iter_json_objects(text: str):
    decoder = json.JSONDecoder()
    idx = 0
    length = len(text)
    while idx < length:
        brace_pos = text.find("{", idx)
        if brace_pos == -1:
            break
        try:
            obj, end_pos = decoder.raw_decode(text, brace_pos)
            if isinstance(obj, dict):
                yield obj
            idx = end_pos
        except json.JSONDecodeError:
            idx = brace_pos + 1


def _extract_json_commands(text: str, esp: ESPWebSocketClient) -> str | None:
    if not text:
        return None

    executed = False
    custom_reply = ""
    valid_tools = {"set_relay", "set_dimmer", "set_all_relays", "get_device_status"}

    for obj in _iter_json_objects(text):
        fn_name = obj.get("name")
        fn_args = obj.get("arguments")
        if isinstance(fn_name, str) and fn_name in valid_tools:
            if isinstance(fn_args, str):
                try:
                    fn_args = json.loads(fn_args)
                except json.JSONDecodeError:
                    fn_args = {}
            if not isinstance(fn_args, dict):
                fn_args = {}
            execute_tool_call(fn_name, fn_args, esp)
            executed = True
            continue

        cmds = obj.get("commands")
        if isinstance(cmds, list):
            for item in cmds:
                if not isinstance(item, str):
                    continue
                cmd = re.sub(r"\s+", " ", item.strip().upper())
                if re.fullmatch(r"(?:POWER[1-4]\s+(?:ON|OFF)|DIMMER:[0-9]|ALL_ON|ALL_OFF|STATUS)", cmd):
                    esp.send_command(cmd)
                    executed = True
            if isinstance(obj.get("reply"), str) and obj["reply"].strip():
                custom_reply = obj["reply"].strip()

    if executed:
        return custom_reply or f"Done. Relays: {esp.relay_states}, dimmer: {esp.dimmer_level}."
    return None


def local_rule_fallback(text: str, esp: ESPWebSocketClient) -> str | None:
    """Zero-LLM deterministic fallback supporting single and compound commands."""
    t = text.lower().strip()
    actions = []

    if re.search(r"\ball\s+on\b|\bturn\s+on\s+all\b", t):
        esp.send_command("ALL_ON")
        actions.append("all relays on")
    elif re.search(r"\ball\s+off\b|\bturn\s+off\s+all\b", t):
        esp.send_command("ALL_OFF")
        actions.append("all relays off")

    relay_cmds: dict[int, str] = {}
    for m in re.finditer(r"\brelay\s*([1-4])\s*(on|off)\b", t):
        relay_cmds[int(m.group(1))] = m.group(2).upper()
    for m in re.finditer(r"\bturn\s+(on|off)\s+relay\s*([1-4])\b", t):
        relay_cmds[int(m.group(2))] = m.group(1).upper()

    for ch in sorted(relay_cmds):
        state = relay_cmds[ch]
        esp.send_command(f"POWER{ch} {state}")
        actions.append(f"relay {ch} {state.lower()}")

    dm = re.search(
        r"\b(?:dimmer|fan|speed)(?:\s+(?:speed|level))?(?:\s*(?:to|at|level|:))?\s*([0-9])\b",
        t,
    )
    if dm:
        lvl = int(dm.group(1))
        esp.send_command(f"DIMMER:{lvl}")
        actions.append(f"dimmer {lvl}")
    elif re.search(r"\bturn\s+on\s+(?:the\s+)?fan\b", t) and esp.dimmer_level == 0:
        esp.send_command("DIMMER:5")
        actions.append("dimmer 5")
    elif re.search(r"\bturn\s+off\s+(?:the\s+)?fan\b", t):
        esp.send_command("DIMMER:0")
        actions.append("dimmer 0")

    if not actions and ("status" in t or "state" in t):
        esp.send_command("STATUS")
        return f"Relays {esp.relay_states}, dimmer {esp.dimmer_level}."

    if actions:
        return "Done: " + ", ".join(actions) + "."
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

    base_messages = [
        {
            "role": "system",
            "content": (
                "You are a smart room controller. Call tools for relays (1-4), dimmer/fan (0-9), "
                "all-on/off, or status. Reply in one short sentence under 15 words."
            ),
        },
        {"role": "user", "content": prompt_text},
    ]

    last_err = None
    for m_id in models:
        try:
            resp = client.chat.completions.create(
                model=m_id,
                messages=list(base_messages),
                tools=OLLAMA_TOOLS,
                tool_choice="auto",
                temperature=0.0,
            )
            msg = resp.choices[0].message
            tool_calls = getattr(msg, "tool_calls", None) or []

            if tool_calls:
                tool_msgs = []
                for tc in tool_calls:
                    fn_name = tc.function.name
                    try:
                        fn_args = json.loads(tc.function.arguments or "{}")
                    except json.JSONDecodeError:
                        fn_args = {}
                    result_str = execute_tool_call(fn_name, fn_args, esp)
                    tool_msgs.append({
                        "role": "tool",
                        "tool_call_id": tc.id,
                        "content": result_str,
                    })

                followup_messages = list(base_messages) + [_build_clean_assistant_message(msg)] + tool_msgs
                try:
                    followup = client.chat.completions.create(
                        model=m_id,
                        messages=followup_messages,
                        temperature=0.0,
                    )
                    reply = (followup.choices[0].message.content or "").strip()
                    return reply or f"Done. Relays: {esp.relay_states}, dimmer: {esp.dimmer_level}."
                except Exception as follow_err:
                    if log_fn:
                        log_fn(f"[Ollama Follow-up Note] {follow_err}\n")
                    return f"Done. Relays: {esp.relay_states}, dimmer: {esp.dimmer_level}."

            content = (msg.content or "").strip()
            json_reply = _extract_json_commands(content, esp)
            if json_reply:
                return json_reply

            rule_reply = local_rule_fallback(prompt_text, esp)
            if rule_reply:
                return rule_reply

            return content or "Done."
        except Exception as e:
            last_err = e
            if log_fn:
                log_fn(f"[Ollama {m_id} Error] {e}\n")

    local_res = local_rule_fallback(prompt_text, esp)
    if local_res:
        return f"{local_res} (via local fallback)"
    if last_err is not None:
        raise last_err
    return "No action taken."


class OllamaESPControllerGUI:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Ollama ESP WebSocket Voice & Hardware Controller")
        self.root.geometry("820x830")
        self.root.minsize(740, 750)

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
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.connect_ws()

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

        ttk.Label(ollama_frame, text="Model:").pack(side="left")
        self.model_combo = ttk.Combobox(
            ollama_frame,
            values=list(AVAILABLE_OLLAMA_MODELS),
            width=24,
            state="normal",
        )
        self.model_combo.set(DEFAULT_OLLAMA_MODEL)
        self.model_combo.pack(side="left", padx=(6, 6))

        ttk.Button(ollama_frame, text="List Models", command=self.check_models).pack(side="left")

        # --- Natural Language & Default Mic Frame ---
        prompt_frame = ttk.LabelFrame(self.root, text="Intent Command (Text or Default Mic)", padding=8)
        prompt_frame.pack(fill="x", **pad)

        self.prompt_entry = ttk.Entry(prompt_frame, font=("Arial", 10))
        self.prompt_entry.insert(0, "Turn on relay 4 and set the dimmer to level 6.")
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
        self._record_stop_event.set()
        self.speech_worker.stop()
        self.esp_client.stop()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    app = OllamaESPControllerGUI(root)
    root.mainloop()