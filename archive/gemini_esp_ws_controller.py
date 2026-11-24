import asyncio
import io
import json
import os
import threading
import time
import tkinter as tk
import wave
from tkinter import messagebox, scrolledtext, ttk

import pyttsx3
import sounddevice as sd
import websockets
from google import genai
from google.genai import types

DEFAULT_GEMINI_API_KEY = "AQ.Ab8RN6KXlOcuznpIESkRNMMd_nVTcNitfMHv9VfcNmd_9v_UCg"
GEMINI_MODEL = "gemini-3.5-flash-lite"

MIC_SAMPLE_RATE = 16000
MAX_RECORD_SECONDS = 6


class ESPWebSocketClient:
    def __init__(
        self,
        esp_ip: str = "192.168.29.110",
        ws_port: int = 81,
        web_user: str = "admin",
        web_pass: str = "ThunderSno0w@,,#",
        on_log=None,
        on_status_change=None,
        on_state_change=None,
    ):
        self.esp_ip = esp_ip
        self.ws_port = ws_port
        self.web_user = web_user
        self.web_pass = web_pass
        self.on_log = on_log
        self.on_status_change = on_status_change
        self.on_state_change = on_state_change

        self._ws = None
        self._loop = None
        self._thread = None
        self._stop_event = threading.Event()
        self._connected_event = threading.Event()

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
        self._log(f"<< {raw_msg}\n")
        try:
            data = json.loads(raw_msg)
        except json.JSONDecodeError:
            return

        self.last_feedback = data
        state_updated = False

        for i in range(1, 5):
            key = f"POWER{i}"
            if key in data:
                self.relay_states[i - 1] = (data[key] == "ON")
                state_updated = True

        if "Dimmer" in data:
            try:
                self.dimmer_level = max(0, min(9, int(data["Dimmer"])))
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

    def send_command(self, cmd: str, wait_feedback: float = 0.35) -> str:
        if not self._connected_event.is_set() or not self._ws or not self._loop:
            msg = f"Failed to send '{cmd}': ESP WebSocket is not connected."
            self._log(f">> {msg}\n")
            return msg

        payload = json.dumps({"command": cmd})
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


def create_esp_tools(esp: ESPWebSocketClient):
    def set_relay(channel: int, turn_on: bool) -> str:
        if channel not in (1, 2, 3, 4):
            return "Invalid channel. Must be between 1 and 4."
        state_str = "ON" if turn_on else "OFF"
        return esp.send_command(f"POWER{channel} {state_str}")

    def set_dimmer(level: int) -> str:
        clamped = max(0, min(9, int(level)))
        return esp.send_command(f"DIMMER:{clamped}")

    def set_all_relays(turn_on: bool) -> str:
        return esp.send_command("ALL_ON" if turn_on else "ALL_OFF")

    def get_device_status() -> str:
        esp.send_command("STATUS")
        return json.dumps({
            "relays": esp.relay_states,
            "dimmer": esp.dimmer_level,
            "raw": esp.last_feedback,
        })

    return [set_relay, set_dimmer, set_all_relays, get_device_status]


def resolve_api_key(api_key: str | None) -> str:
    return (api_key or "").strip() or os.environ.get("GEMINI_API_KEY") or DEFAULT_GEMINI_API_KEY


def pcm_to_wav_bytes(pcm_bytes: bytes, sample_rate: int = MIC_SAMPLE_RATE, channels: int = 1) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


def speak_short_message(text: str, max_chars: int = 140, log_fn=None):
    cleaned = " ".join((text or "").replace("*", "").replace("#", "").split()).strip()
    if not cleaned:
        return
    if len(cleaned) > max_chars:
        cleaned = cleaned[:max_chars].rsplit(" ", 1)[0].strip() + "."

    def _run_tts():
        try:
            engine = pyttsx3.init()
            engine.setProperty("rate", 175)
            engine.say(cleaned)
            engine.runAndWait()
            engine.stop()
        except Exception as e:
            if log_fn:
                log_fn(f"[TTS Error] {e}\n")

    threading.Thread(target=_run_tts, daemon=True).start()


def run_intent_input(
    contents_payload,
    esp: ESPWebSocketClient,
    api_key: str,
    model_name: str = GEMINI_MODEL,
    log_fn=None,
) -> tuple[str, str]:
    resolved_key = resolve_api_key(api_key)
    active_model = (model_name or "").strip() or GEMINI_MODEL
    client = genai.Client(api_key=resolved_key)
    tools = create_esp_tools(esp)
    delays = [0.8, 1.8, 3.5]
    last_err = None

    system_prompt = (
        "You are a smart room controller. Translate user text or voice commands into tool calls "
        "for relays (1-4), dimmer (0-9), global relay actions, or status checks. "
        "Reply in a single short sentence (under 15 words) confirming the result."
    )

    for attempt, delay in enumerate(delays, start=1):
        try:
            response = client.models.generate_content(
                model=active_model,
                contents=contents_payload,
                config=types.GenerateContentConfig(
                    system_instruction=system_prompt,
                    tools=tools,
                ),
            )
            return (response.text or "Done.").strip(), active_model
        except Exception as e:
            last_err = e
            err_str = str(e)
            is_503 = "503" in err_str or "UNAVAILABLE" in err_str
            if not is_503:
                raise
            if log_fn:
                log_fn(f"[Retry] {active_model} busy (503), attempt {attempt}/{len(delays)}...\n")
            time.sleep(delay)

    raise last_err


class GeminiESPControllerGUI:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Gemini ESP WebSocket Voice & Hardware Controller")
        self.root.geometry("820x820")
        self.root.minsize(720, 740)

        self.is_running_prompt = False
        self.is_recording = False
        self._record_stop_event = threading.Event()
        self._record_thread = None

        self.relay_states = [False, False, False, False]
        self.relay_buttons = []
        self.last_sent_dimmer = 0

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

        dimmer_frame = ttk.LabelFrame(self.root, text="Dimmer Control (0-9)", padding=8)
        dimmer_frame.pack(fill="x", **pad)

        d_sub = ttk.Frame(dimmer_frame)
        d_sub.pack(fill="x", padx=5)

        ttk.Button(
            d_sub, text="MIN (0)", command=lambda: self.set_dimmer_direct(0)
        ).pack(side="left", padx=4)
        ttk.Button(
            d_sub, text="Step -", command=self.step_dimmer_down
        ).pack(side="left", padx=4)

        self.dimmer_slider = ttk.Scale(
            d_sub, from_=0, to=9, orient="horizontal"
        )
        self.dimmer_slider.pack(
            side="left", expand=True, fill="x", padx=8, pady=4
        )
        self.dimmer_slider.bind("<ButtonRelease-1>", self.on_slider_release)

        self.dimmer_val_lbl = ttk.Label(d_sub, text="Level: 0", width=8, anchor="e")
        self.dimmer_val_lbl.pack(side="left", padx=(0, 6))

        ttk.Button(
            d_sub, text="Step +", command=self.step_dimmer_up
        ).pack(side="left", padx=4)
        ttk.Button(
            d_sub, text="MAX (9)", command=lambda: self.set_dimmer_direct(9)
        ).pack(side="left", padx=4)

        global_frame = ttk.LabelFrame(self.root, text="Global Commands & Status", padding=8)
        global_frame.pack(fill="x", **pad)

        g_sub = ttk.Frame(global_frame)
        g_sub.pack(fill="x")

        ttk.Button(
            g_sub, text="ALL ON", command=self.send_all_on
        ).pack(side="left", expand=True, padx=5)
        ttk.Button(
            g_sub, text="ALL OFF", command=self.send_all_off
        ).pack(side="left", expand=True, padx=5)
        ttk.Button(
            g_sub, text="Get Status", command=self.send_status_cmd
        ).pack(side="left", expand=True, padx=5)

        key_frame = ttk.LabelFrame(self.root, text="Gemini API Configuration", padding=8)
        key_frame.pack(fill="x", **pad)

        ttk.Label(key_frame, text="API Key:").pack(side="left")
        self.api_key_entry = ttk.Entry(key_frame, width=24, show="*")
        self.api_key_entry.insert(0, resolve_api_key(None))
        self.api_key_entry.pack(side="left", fill="x", expand=True, padx=(6, 8))

        self.show_key_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            key_frame,
            text="Show",
            variable=self.show_key_var,
            command=lambda: self.api_key_entry.config(show="" if self.show_key_var.get() else "*"),
        ).pack(side="left", padx=(0, 10))

        ttk.Label(key_frame, text="Model:").pack(side="left")
        self.model_entry = ttk.Entry(key_frame, width=24)
        self.model_entry.insert(0, GEMINI_MODEL)
        self.model_entry.pack(side="left", padx=(6, 0))

        prompt_frame = ttk.LabelFrame(self.root, text="Intent Command (Text or Default Mic)", padding=8)
        prompt_frame.pack(fill="x", **pad)

        self.prompt_entry = ttk.Entry(prompt_frame, font=("Arial", 10))
        self.prompt_entry.insert(0, "Turn on relay 4 and set the dimmer to level 6.")
        self.prompt_entry.pack(side="left", fill="x", expand=True, padx=(0, 8))
        self.prompt_entry.bind("<Return>", lambda _e: self.send_text_intent())

        self.send_btn = ttk.Button(prompt_frame, text="Send Intent", command=self.send_text_intent)
        self.send_btn.pack(side="left", padx=(0, 6))

        self.mic_btn = ttk.Button(prompt_frame, text="?? Speak (Default Mic)", command=self.toggle_voice_recording)
        self.mic_btn.pack(side="left")

        quick_frame = ttk.Frame(self.root)
        quick_frame.pack(fill="x", padx=10, pady=(0, 4))

        samples = [
            ("Relay 1 ON", "Turn on relay 1."),
            ("All Relays OFF", "Turn off all relays."),
            ("Dimmer 5", "Set the dimmer to 5."),
            ("Check Status", "What is the current relay and dimmer status?"),
        ]
        for label, sample_prompt in samples:
            ttk.Button(
                quick_frame,
                text=label,
                command=lambda p=sample_prompt: self._use_sample_prompt(p),
            ).pack(side="left", padx=(0, 6))

        self.speak_reply_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            quick_frame,
            text="?? Speak Short Reply (Default Speaker)",
            variable=self.speak_reply_var,
        ).pack(side="right")

        log_frame = ttk.LabelFrame(self.root, text="Assistant Response & WebSocket Log", padding=8)
        log_frame.pack(fill="both", expand=True, **pad)

        self.log_box = scrolledtext.ScrolledText(
            log_frame, height=11, state="disabled", font=("Consolas", 9), wrap="word"
        )
        self.log_box.pack(fill="both", expand=True)

        ttk.Button(log_frame, text="Clear Log", command=self.clear_log).pack(anchor="e", pady=(6, 0))

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

    def _use_sample_prompt(self, text: str):
        self.prompt_entry.delete(0, tk.END)
        self.prompt_entry.insert(0, text)
        self.send_text_intent()

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
        self.mic_btn.config(text="? Stop & Send")
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

            wav_bytes = pcm_to_wav_bytes(b"".join(frames), sample_rate=MIC_SAMPLE_RATE, channels=1)
            self.root.after(0, self._reset_mic_ui)

            if len(frames) == 0:
                self.safe_log("[Mic] No audio captured.\n")
                return

            self._submit_prompt_payload(
                payload=[
                    "Execute the spoken smart-home command and confirm briefly.",
                    types.Part.from_bytes(data=wav_bytes, mime_type="audio/wav"),
                ],
                display_label="[You - Voice]",
            )

        self._record_thread = threading.Thread(target=record_worker, daemon=True)
        self._record_thread.start()

    def _stop_mic_capture(self):
        self._record_stop_event.set()

    def _reset_mic_ui(self):
        self.is_recording = False
        self.mic_btn.config(text="?? Speak (Default Mic)")
        if not self.is_running_prompt:
            self.send_btn.config(state="normal")

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
        if not prompt:
            return
        self._submit_prompt_payload(payload=prompt, display_label=f"[You] {prompt}")

    def _submit_prompt_payload(self, payload, display_label: str):
        if self.is_running_prompt or self.is_recording:
            return
        api_key = self.api_key_entry.get().strip()
        model_name = self.model_entry.get().strip()
        should_speak = bool(self.speak_reply_var.get())

        self.is_running_prompt = True
        self.send_btn.config(state="disabled")
        self.mic_btn.config(state="disabled")
        self.safe_log(f"\n{display_label}\n")

        def worker():
            try:
                reply, used_model = run_intent_input(
                    payload,
                    self.esp_client,
                    api_key,
                    model_name=model_name,
                    log_fn=self.safe_log,
                )
                self.safe_log(f"[Gemini ({used_model})] {reply}\n")
                if should_speak:
                    speak_short_message(reply, max_chars=140, log_fn=self.safe_log)
            except Exception as e:
                self.safe_log(f"[Error] {e}\n")
            finally:
                self.root.after(0, self._finish_prompt)

        threading.Thread(target=worker, daemon=True).start()

    def _finish_prompt(self):
        self.is_running_prompt = False
        if not self.is_recording:
            self.send_btn.config(state="normal")
            self.mic_btn.config(state="normal")

    def safe_status_update(self, connected: bool):
        def _apply():
            if connected:
                self.status_lbl.config(text="Connected", foreground="#1b7f2a")
            else:
                self.status_lbl.config(text="Disconnected", foreground="#b00020")

        try:
            self.root.after(0, _apply)
        except Exception:
            pass

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

    def clear_log(self):
        self.log_box.config(state="normal")
        self.log_box.delete("1.0", tk.END)
        self.log_box.config(state="disabled")

    def on_closing(self):
        self._record_stop_event.set()
        self.esp_client.stop()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    app = GeminiESPControllerGUI(root)
    root.mainloop()
