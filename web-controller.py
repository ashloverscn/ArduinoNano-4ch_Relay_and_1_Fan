import json
import os
import threading
from flask import Flask, render_template_string, jsonify, request
from flask_socketio import SocketIO
import serial
import serial.tools.list_ports

CONFIG_FILE = "arduino_config.json"

app = Flask(__name__)
app.config["SECRET_KEY"] = "arduino_secret_key"
socketio = SocketIO(app, cors_allowed_origins="*")

# --- GLOBAL CONTROLLER STATE ---
class ArduinoController:
    def __init__(self):
        self.ser = None
        self.is_connected = False
        self.read_thread = None
        self.relay_states = [False, False, False, False]
        self.last_sent_dimmer = 0
        self.selected_port = ""
        self.selected_baud = 115200
        self.load_saved_settings()

    def load_config(self):
        if os.path.exists(CONFIG_FILE):
            try:
                with open(CONFIG_FILE, "r") as f:
                    return json.load(f)
            except Exception:
                pass
        return {}

    def save_config(self, port, baud):
        try:
            with open(CONFIG_FILE, "w") as f:
                json.dump({"port": port, "baud": baud}, f)
        except Exception:
            pass

    def load_saved_settings(self):
        config = self.load_config()
        self.selected_port = config.get("port", "")
        self.selected_baud = config.get("baud", 115200)

    def get_ports(self):
        try:
            return [port.device for port in serial.tools.list_ports.comports()]
        except Exception as e:
            print(f"Error listing ports: {e}")
            return []

    def connect(self, port, baudrate):
        if self.is_connected:
            self.disconnect()

        if not port:
            return False, "No COM port specified."

        try:
            self.ser = serial.Serial(port, int(baudrate), timeout=1)
            self.is_connected = True
            self.selected_port = port
            self.selected_baud = int(baudrate)
            self.save_config(port, int(baudrate))

            self.read_thread = threading.Thread(target=self.read_serial, daemon=True)
            self.read_thread.start()

            self.log_message(f"Connected to {port} at {baudrate} baud.")
            self.send_command("STATUS")
            return True, "Connected successfully."
        except Exception as e:
            self.is_connected = False
            return False, str(e)

    def disconnect(self):
        self.is_connected = False
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception as e:
                print(e)
        self.log_message("Disconnected.")

    def send_command(self, cmd):
        if not self.is_connected or not self.ser or not self.ser.is_open:
            self.log_message("Warning: Not connected to any serial device!")
            return False

        try:
            full_cmd = cmd + "\n"
            self.ser.write(full_cmd.encode("utf-8"))
            self.log_message(f">> {cmd}")
            return True
        except Exception as e:
            self.log_message(f"Serial Write Error: {e}")
            return False

    def read_serial(self):
        while self.is_connected:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                    if line:
                        self.log_message(f"<< {line}")
                        self.parse_arduino_feedback(line)
            except Exception:
                break

    def parse_arduino_feedback(self, line):
        try:
            data = json.loads(line)
            for i in range(1, 5):
                key = f"POWER{i}"
                if key in data:
                    is_on = data[key] == "ON"
                    self.relay_states[i - 1] = is_on
                    socketio.emit("relay_update", {"relay": i, "state": is_on})

            if "Dimmer" in data:
                dim_val = data["Dimmer"]
                self.last_sent_dimmer = dim_val
                socketio.emit("dimmer_update", {"value": dim_val})
        except json.JSONDecodeError:
            pass

    def log_message(self, msg):
        print(msg)
        socketio.emit("console_log", {"message": msg})


controller = ArduinoController()

# --- EMBEDDED HTML/JS UI ---
HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Arduino Home Automation Web Controller</title>
    <script src="https://cdnjs.cloudflare.com/ajax/libs/socket.io/4.5.4/socket.io.min.js"></script>
    <style>
        body {
            font-family: Arial, sans-serif;
            background-color: #f4f6f8;
            margin: 0;
            padding: 20px;
            display: flex;
            justify-content: center;
        }
        .container {
            width: 100%;
            max-width: 650px;
            background: #ffffff;
            border-radius: 8px;
            padding: 20px;
            box-shadow: 0 4px 10px rgba(0,0,0,0.1);
        }
        h2 { text-align: center; margin-top: 0; color: #333; }
        .card {
            border: 1px solid #ddd;
            border-radius: 6px;
            padding: 15px;
            margin-bottom: 15px;
            background: #fafafa;
        }
        .card-title {
            font-weight: bold;
            margin-bottom: 10px;
            color: #555;
            font-size: 14px;
            text-transform: uppercase;
        }
        .row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
        select, input, button {
            padding: 8px 12px;
            font-size: 14px;
            border-radius: 4px;
            border: 1px solid #ccc;
        }
        button {
            cursor: pointer;
            background-color: #007bff;
            color: white;
            border: none;
            transition: 0.2s;
        }
        button:hover { background-color: #0056b3; }
        button.disconnect { background-color: #dc3545; }
        button.disconnect:hover { background-color: #bd2130; }
        
        .relay-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
        .relay-btn {
            height: 70px;
            font-weight: bold;
            background-color: #e0e0e0;
            color: #000;
            border-radius: 6px;
        }
        .relay-btn.on {
            background-color: #4CAF50;
            color: #fff;
        }

        #console {
            width: 100%;
            height: 150px;
            background-color: #1e1e1e;
            color: #00ff00;
            font-family: monospace;
            font-size: 12px;
            padding: 10px;
            box-sizing: border-box;
            overflow-y: scroll;
            border-radius: 4px;
            resize: vertical;
        }
        .flex-fill { flex: 1; }
    </style>
</head>
<body>

<div class="container">
    <h2>Arduino Home Automation</h2>

    <div class="card">
        <div class="card-title">Serial Connection</div>
        <div class="row">
            <label>Port:</label>
            <select id="portSelect">
                <option value="">-- Select Port --</option>
            </select>
            <button onclick="refreshPorts()">Refresh</button>

            <label>Baudrate:</label>
            <input type="number" id="baudRate" value="115200" style="width: 80px;">

            <button id="connectBtn" onclick="toggleConnection()">Connect</button>
        </div>
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
        <div class="card-title">Global Commands & Status</div>
        <div class="row">
            <button class="flex-fill" onclick="sendCommand('ALL_ON')">ALL ON</button>
            <button class="flex-fill" onclick="sendCommand('ALL_OFF')">ALL OFF</button>
            <button class="flex-fill" onclick="sendCommand('STATUS')">Get Status</button>
        </div>
    </div>

    <div class="card">
        <div class="card-title">Serial Console / Feedback</div>
        <div id="console"></div>
    </div>
</div>

<script>
    const socket = io();
    let isConnected = false;
    let relayStates = [false, false, false, false];

    async function refreshPorts() {
        try {
            const res = await fetch('/api/ports');
            const data = await res.json();
            const select = document.getElementById('portSelect');
            select.innerHTML = '<option value="">-- Select Port --</option>';
            
            data.ports.forEach(port => {
                const opt = document.createElement('option');
                opt.value = port;
                opt.innerText = port;
                if (port === data.saved_port) opt.selected = true;
                select.appendChild(opt);
            });
            if (data.saved_baud) {
                document.getElementById('baudRate').value = data.saved_baud;
            }
            updateConnectionState(data.is_connected);
        } catch (err) {
            console.error("Failed to fetch ports:", err);
        }
    }

    async function toggleConnection() {
        const port = document.getElementById('portSelect').value;
        const baud = document.getElementById('baudRate').value;

        if (!isConnected && !port) {
            alert("Please select a COM port first!");
            return;
        }

        try {
            if (!isConnected) {
                const res = await fetch('/api/connect', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({port, baud})
                });
                const data = await res.json();
                if (!data.success) {
                    alert("Connection Error: " + data.message);
                }
            } else {
                await fetch('/api/disconnect', {method: 'POST'});
            }
            checkStatus();
        } catch (err) {
            alert("Network or Server Error: " + err);
        }
    }

    async function checkStatus() {
        try {
            const res = await fetch('/api/status');
            const data = await res.json();
            updateConnectionState(data.is_connected);
            data.relays.forEach((state, i) => updateRelayUI(i + 1, state));
            document.getElementById('dimmerSlider').value = data.dimmer;
        } catch (err) {
            console.error("Failed to check status:", err);
        }
    }

    function updateConnectionState(connected) {
        isConnected = connected;
        const btn = document.getElementById('connectBtn');
        const portSelect = document.getElementById('portSelect');
        const baudRate = document.getElementById('baudRate');

        if (connected) {
            btn.innerText = "Disconnect";
            btn.classList.add("disconnect");
            portSelect.disabled = true;
            baudRate.disabled = true;
        } else {
            btn.innerText = "Connect";
            btn.classList.remove("disconnect");
            portSelect.disabled = false;
            baudRate.disabled = false;
        }
    }

    function toggleRelay(relayNum) {
        const currentState = relayStates[relayNum - 1];
        const cmd = `POWER${relayNum} ${currentState ? 'OFF' : 'ON'}`;
        sendCommand(cmd);
    }

    function onSliderChange(val) {
        sendCommand(`DIMMER ${val}`);
    }

    async function sendCommand(cmd) {
        try {
            await fetch('/api/command', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({command: cmd})
            });
        } catch (err) {
            console.error("Failed to send command:", err);
        }
    }

    function updateRelayUI(relayNum, isOn) {
        relayStates[relayNum - 1] = isOn;
        const btn = document.getElementById(`relay-${relayNum}`);
        if (isOn) {
            btn.innerText = `Relay ${relayNum}\nON`;
            btn.classList.add('on');
        } else {
            btn.innerText = `Relay ${relayNum}\nOFF`;
            btn.classList.remove('on');
        }
    }

    function appendConsole(msg) {
        const consoleEl = document.getElementById('console');
        consoleEl.innerHTML += msg + '<br>';
        consoleEl.scrollTop = consoleEl.scrollHeight;
    }

    socket.on('console_log', data => appendConsole(data.message));
    socket.on('relay_update', data => updateRelayUI(data.relay, data.state));
    socket.on('dimmer_update', data => {
        document.getElementById('dimmerSlider').value = data.value;
    });

    refreshPorts();
    checkStatus();
</script>

</body>
</html>
"""

@app.route("/")
def index():
    return render_template_string(HTML_TEMPLATE)

@app.route("/api/ports")
def get_ports():
    return jsonify({
        "ports": controller.get_ports(),
        "saved_port": controller.selected_port,
        "saved_baud": controller.selected_baud,
        "is_connected": controller.is_connected
    })

@app.route("/api/status")
def get_status():
    return jsonify({
        "is_connected": controller.is_connected,
        "relays": controller.relay_states,
        "dimmer": controller.last_sent_dimmer
    })

@app.route("/api/connect", methods=["POST"])
def connect_serial():
    data = request.json or {}
    success, message = controller.connect(data.get("port"), data.get("baud"))
    return jsonify({"success": success, "message": message})

@app.route("/api/disconnect", methods=["POST"])
def disconnect_serial():
    controller.disconnect()
    return jsonify({"success": True})

@app.route("/api/command", methods=["POST"])
def send_command():
    data = request.json or {}
    cmd = data.get("command")
    if cmd:
        success = controller.send_command(cmd)
        return jsonify({"success": success})
    return jsonify({"success": False, "message": "No command provided."})

if __name__ == "__main__":
    socketio.run(app, host="0.0.0.0", port=80, debug=False)
