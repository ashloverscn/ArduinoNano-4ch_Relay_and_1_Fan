import asyncio
import json
import os
import logging
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk
import websockets
import paho.mqtt.client as mqtt

# --- Logging Setup ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s | %(levelname)s | %(message)s')
logger = logging.getLogger("MQTT-Slave-GUI")

# --- Configuration ---
CONFIG_FILE = "arduino_config.json"
EEPROM_FILE = "eeprom_state.json"

BROKER_ADDRESS = "i11b0026.ala.eu-central-1.emqxsl.com"
PORT = 8883
MQTT_USERNAME = "admin"
MQTT_PASSWORD = "admin1234S"

TOPIC_CMD = "home/esp8266/command"
TOPIC_STATE = "home/esp8266/state"

# --- Controller State ---
connected_websockets = set()
relay_states = [False, False, False, False]
dimmer_value = 0

# --- State Management (EEPROM Emulation) ---
def load_eeprom_state():
    global relay_states, dimmer_value
    if os.path.exists(EEPROM_FILE):
        try:
            with open(EEPROM_FILE, "r") as f:
                data = json.load(f)
                relay_states = data.get("relays", [False, False, False, False])
                dimmer_value = data.get("dimmer", 0)
                logger.info(f"Loaded persistent state from {EEPROM_FILE}: Relays={relay_states}, Dimmer={dimmer_value}")
        except Exception as e:
            logger.error(f"Error loading state file: {e}")

def save_eeprom_state():
    data = {"relays": relay_states, "dimmer": dimmer_value}
    try:
        with open(EEPROM_FILE, "w") as f:
            json.dump(data, f)
            logger.info(f"State saved to {EEPROM_FILE} (EEPROM simulated).")
    except Exception as e:
        logger.error(f"Error saving state file: {e}")

load_eeprom_state()

# --- MQTT Setup ---
mqtt_client = mqtt.Client(
    client_id="python_eeprom_slave",
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    protocol=mqtt.MQTTv5
)
mqtt_client.username_pw_set(username=MQTT_USERNAME, password=MQTT_PASSWORD)
mqtt_client.tls_set()

def broadcast_ws_state():
    if not connected_websockets:
        return
    state_payload = {
        "type": "state_update",
        "relays": relay_states,
        "dimmer": dimmer_value
    }
    msg = json.dumps(state_payload)
    try:
        loop = asyncio.get_event_loop()
        if loop.is_running():
            asyncio.run_coroutine_threadsafe(
                websockets.broadcast(connected_websockets, msg),
                loop
            )
    except Exception as e:
        logger.error(f"WS broadcast error: {e}")

def broadcast_ws_log(message):
    if not connected_websockets:
        return
    log_payload = {
        "type": "console_log",
        "message": message
    }
    msg = json.dumps(log_payload)
    try:
        loop = asyncio.get_event_loop()
        if loop.is_running():
            asyncio.run_coroutine_threadsafe(
                websockets.broadcast(connected_websockets, msg),
                loop
            )
    except Exception as e:
        logger.error(f"WS log error: {e}")

def process_command(command, source="mqtt"):
    global relay_states, dimmer_value
    command = command.strip()
    logger.info(f"Processing Command [{source}]: {command}")
    broadcast_ws_log(f">> [{source}] {command}")

    state_changed = False

    for i in range(1, 5):
        if command == f"POWER{i} ON":
            if not relay_states[i - 1]:
                relay_states[i - 1] = True
                state_changed = True
        elif command == f"POWER{i} OFF":
            if relay_states[i - 1]:
                relay_states[i - 1] = False
                state_changed = True

    if command.startswith("DIMMER "):
        try:
            val = int(command.split(" ")[1])
            if dimmer_value != val:
                dimmer_value = max(0, min(9, val))
                state_changed = True
        except:
            pass
    elif command == "DIMM_UP":
        if dimmer_value < 9:
            dimmer_value += 1
            state_changed = True
    elif command == "DIMM_DN":
        if dimmer_value > 0:
            dimmer_value -= 1
            state_changed = True
    elif command == "ALL_ON":
        relay_states = [True, True, True, True]
        dimmer_value = 9
        state_changed = True
    elif command == "ALL_OFF":
        relay_states = [False, False, False, False]
        dimmer_value = 0
        state_changed = True
    elif command == "STATUS":
        state_changed = True

    if state_changed or command == "STATUS":
        save_eeprom_state()
        broadcast_ws_state()
        publish_mqtt_state()
        if app_instance:
            app_instance.root.after(0, app_instance.update_gui_from_state)

def publish_mqtt_state():
    state_payload = {
        "type": "state_update",
        "relays": relay_states,
        "dimmer": dimmer_value
    }
    payload_str = json.dumps(state_payload)
    mqtt_client.publish(TOPIC_STATE, payload_str, qos=1)
    logger.info(f"Published MQTT State: {payload_str}")

def on_mqtt_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        logger.info("Connected to EMQX Broker successfully!")
        client.subscribe(TOPIC_CMD)
        publish_mqtt_state()
    else:
        logger.error(f"MQTT connection failed with code {reason_code}")

def on_mqtt_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        if "command" in payload:
            process_command(payload["command"], source="MQTT")
            if app_instance:
                app_instance.log_message(f"<< [MQTT] {payload['command']}\n")
    except Exception as e:
        logger.error(f"Error parsing MQTT message: {e}")

mqtt_client.on_connect = on_mqtt_connect
mqtt_client.on_message = on_mqtt_message

# --- Tkinter GUI Application Class ---
class ArduinoControllerApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Python MQTT Slave Controller")
        self.root.geometry("620x580")
        self.root.resizable(False, False)

        self.relay_buttons = []
        self.last_sent_dimmer = dimmer_value

        self.create_widgets()
        self.update_gui_from_state()

    def create_widgets(self):
        # --- CONNECTION STATUS FRAME ---
        conn_frame = ttk.LabelFrame(self.root, text="MQTT System Status", padding=10)
        conn_frame.pack(fill="x", padx=10, pady=10)

        self.status_lbl = ttk.Label(conn_frame, text="Status: Connected to EMQX Broker", font=("Arial", 10, "bold"), foreground="green")
        self.status_lbl.pack(anchor="w", padx=5)

        # --- RELAYS FRAME ---
        relay_frame = ttk.LabelFrame(self.root, text="Relay Controls", padding=10)
        relay_frame.pack(fill="x", padx=10, pady=5)

        for i in range(1, 5):
            btn = tk.Button(
                relay_frame,
                text=f"Relay {i}\nOFF",
                font=("Arial", 10, "bold"),
                width=12,
                height=3,
                bg="#e0e0e0",
                fg="#000000",
                relief="raised",
                command=lambda r=i: self.toggle_relay(r)
            )
            btn.pack(side="left", expand=True, padx=5, pady=5)
            self.relay_buttons.append(btn)

        # --- DIMMER FRAME ---
        dimmer_frame = ttk.LabelFrame(self.root, text="Dimmer Control (0-9)", padding=10)
        dimmer_frame.pack(fill="x", padx=10, pady=5)

        d_sub = ttk.Frame(dimmer_frame)
        d_sub.pack(fill="x", padx=5)

        ttk.Button(d_sub, text="MIN (0)", command=lambda: self.send_command("DIMMER 0")).pack(side="left", padx=5)
        ttk.Button(d_sub, text="Step -", command=lambda: self.send_command("DIMM_DN")).pack(side="left", padx=5)

        self.dimmer_slider = ttk.Scale(d_sub, from_=0, to=9, orient="horizontal")
        self.dimmer_slider.pack(side="left", expand=True, fill="x", padx=10, pady=5)
        self.dimmer_slider.bind("<ButtonRelease-1>", self.on_slider_release)

        ttk.Button(d_sub, text="Step +", command=lambda: self.send_command("DIMM_UP")).pack(side="left", padx=5)
        ttk.Button(d_sub, text="MAX (9)", command=lambda: self.send_command("DIMMER 9")).pack(side="left", padx=5)

        # --- GLOBAL & UTILS FRAME ---
        global_frame = ttk.LabelFrame(self.root, text="Global Commands & Status", padding=10)
        global_frame.pack(fill="x", padx=10, pady=5)

        g_sub = ttk.Frame(global_frame)
        g_sub.pack(fill="x")

        ttk.Button(g_sub, text="ALL ON", command=lambda: self.send_command("ALL_ON")).pack(side="left", expand=True, padx=5)
        ttk.Button(g_sub, text="ALL OFF", command=lambda: self.send_command("ALL_OFF")).pack(side="left", expand=True, padx=5)
        ttk.Button(g_sub, text="Get Status", command=lambda: self.send_command("STATUS")).pack(side="left", expand=True, padx=5)

        # --- LOG / CONSOLE OUTPUT ---
        log_frame = ttk.LabelFrame(self.root, text="Console / Feedback Log", padding=10)
        log_frame.pack(fill="both", expand=True, padx=10, pady=5)

        self.console_log = scrolledtext.ScrolledText(log_frame, height=7, state="disabled", font=("Courier", 9))
        self.console_log.pack(fill="both", expand=True)

    def toggle_relay(self, relay_num):
        idx = relay_num - 1
        current_state = relay_states[idx]
        cmd = f"POWER{relay_num} {'OFF' if current_state else 'ON'}"
        self.send_command(cmd)

    def send_command(self, cmd):
        threading.Thread(target=lambda: [
            process_command(cmd, source="GUI"),
            mqtt_client.publish(TOPIC_CMD, json.dumps({"command": cmd}))
        ], daemon=True).start()
        self.log_message(f">> {cmd}\n")

    def on_slider_release(self, event):
        val_int = int(round(self.dimmer_slider.get()))
        self.dimmer_slider.set(val_int)
        if val_int != self.last_sent_dimmer:
            self.last_sent_dimmer = val_int
            self.send_command(f"DIMMER {val_int}")

    def update_gui_from_state(self):
        for i in range(4):
            isOn = relay_states[i]
            btn = self.relay_buttons[i]
            if isOn:
                btn.config(text=f"Relay {i+1}\nON", bg="#4CAF50", fg="#FFFFFF")
            else:
                btn.config(text=f"Relay {i+1}\nOFF", bg="#e0e0e0", fg="#000000")
        self.dimmer_slider.set(dimmer_value)
        self.last_sent_dimmer = dimmer_value

    def log_message(self, msg):
        self.console_log.config(state="normal")
        self.console_log.insert(tk.END, msg)
        self.console_log.see(tk.END)
        self.console_log.config(state="disabled")

# --- HTML Template for Web Client ---
HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Python MQTT Slave Home Automation Controller</title>
    <style>
        body { font-family: Arial, sans-serif; background-color: #f4f6f8; margin: 0; padding: 20px; display: flex; justify-content: center; }
        .container { width: 100%; max-width: 650px; background: #ffffff; border-radius: 8px; padding: 20px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }
        h2 { text-align: center; margin-top: 0; color: #333; }
        .card { border: 1px solid #ddd; border-radius: 6px; padding: 15px; margin-bottom: 15px; background: #fafafa; }
        .card-title { font-weight: bold; margin-bottom: 10px; color: #555; font-size: 14px; text-transform: uppercase; }
        .row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
        button { padding: 10px 15px; font-size: 14px; border-radius: 4px; border: none; cursor: pointer; background-color: #007bff; color: white; transition: 0.2s; }
        button:hover { background-color: #0056b3; }
        .relay-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
        .relay-btn { height: 70px; font-weight: bold; background-color: #e0e0e0; color: #000; border-radius: 6px; }
        .relay-btn.on { background-color: #4CAF50; color: #fff; }
        #console { width: 100%; height: 150px; background-color: #1e1e1e; color: #00ff00; font-family: monospace; font-size: 12px; padding: 10px; box-sizing: border-box; overflow-y: scroll; border-radius: 4px; }
        .flex-fill { flex: 1; }
        #connectionStatus { font-weight: bold; padding: 5px 10px; border-radius: 4px; display: inline-block; }
        .status-disconnected { background: #ffebee; color: #c62828; }
        .status-connected { background: #e8f5e9; color: #2e7d32; }
    </style>
</head>
<body>
<div class="container">
    <h2>Python MQTT Slave Controller</h2>
    <div class="card">
        <div class="card-title">System Status</div>
        <div id="connectionStatus" class="status-disconnected">Status: Connecting...</div>
    </div>
    <div class="card">
        <div class="card-title">Relay Controls</div>
        <div class="relay-grid">
            <button id="relay-1" class="relay-btn" onclick="toggleRelay(1)">Relay 1<br>OFF</button>
            <button id="relay-2" class="relay-btn" onclick="toggleRelay(2)">Relay 2<br>OFF</button>
            <button id="relay-3" class="relay-btn" onclick="toggleRelay(3)">Relay 3<br>OFF</button>
            <button id="relay-4" class="relay-btn" onclick="toggleRelay(4)">Relay 4<br>OFF</button>
        </div>
    </div>
    <div class="card">
        <div class="card-title">Dimmer Control (0-9)</div>
        <div class="row">
            <button onclick="sendCommand('DIMMER 0')">MIN (0)</button>
            <button onclick="sendCommand('DIMM_DN')">Step -</button>
            <input type="range" id="dimmerSlider" class="flex-fill" min="0" max="9" value="0" onchange="onSliderChange(this.value)">
            <button onclick="sendCommand('DIMM_UP')">Step +</button>
            <button onclick="sendCommand('DIMMER 9')">MAX (9)</button>
        </div>
    </div>
    <div class="card">
        <div class="card-title">Console Log / Feedback</div>
        <div id="console"></div>
    </div>
</div>
<script>
    let ws;
    let relayStates = [false, false, false, false];
    function initWebSocket() {
        ws = new WebSocket('ws://' + window.location.host + '/ws');
        ws.onopen = (e) => { document.getElementById('connectionStatus').innerText = "Connected"; document.getElementById('connectionStatus').className = "status-connected"; };
        ws.onclose = (e) => { document.getElementById('connectionStatus').innerText = "Disconnected"; document.getElementById('connectionStatus').className = "status-disconnected"; setTimeout(initWebSocket, 2000); };
        ws.onmessage = function(event) {
            let data = JSON.parse(event.data);
            if (data.type === 'state_update') {
                data.relays.forEach((state, i) => {
                    relayStates[i] = state;
                    let btn = document.getElementById(`relay-${i+1}`);
                    btn.innerText = `Relay ${i+1}\n` + (state ? "ON" : "OFF");
                    if (state) btn.classList.add('on'); else btn.classList.remove('on');
                });
                if (data.dimmer !== undefined) document.getElementById('dimmerSlider').value = data.dimmer;
            } else if (data.type === 'console_log') {
                let c = document.getElementById('console');
                c.innerHTML += data.message + '<br>';
                c.scrollTop = c.scrollHeight;
            }
        };
    }
    function toggleRelay(i) { sendCommand(`POWER${i} ${relayStates[i-1] ? 'OFF' : 'ON'}`); }
    function onSliderChange(v) { sendCommand(`DIMMER ${v}`); }
    function sendCommand(cmd) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({command: cmd})); }
    window.onload = initWebSocket;
</script>
</body>
</html>"""

# --- WebSocket & Web Server Router ---
async def router(websocket):
    path = websocket.path
    if path == "/":
        await websocket.send(HTML_TEMPLATE)
    elif path == "/ws":
        connected_websockets.add(websocket)
        try:
            init_msg = json.dumps({"type": "state_update", "relays": relay_states, "dimmer": dimmer_value})
            await websocket.send(init_msg)
            async for message in websocket:
                data = json.loads(message)
                if "command" in data:
                    cmd = data["command"]
                    process_command(cmd, source="WebSocket")
                    mqtt_client.publish(TOPIC_CMD, json.dumps({"command": cmd}))
        finally:
            connected_websockets.remove(websocket)

async def start_async_loop():
    mqtt_client.connect(BROKER_ADDRESS, PORT, keepalive=60)
    mqtt_client.loop_start()
    async with websockets.serve(router, "0.0.0.0", 80):
        await asyncio.Future()

def run_async_loop(loop):
    asyncio.set_event_loop(loop)
    loop.run_until_complete(start_async_loop())

if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    threading.Thread(target=run_async_loop, args=(loop,), daemon=True).start()
    
    root = tk.Tk()
    app_instance = ArduinoControllerApp(root)
    root.mainloop()
