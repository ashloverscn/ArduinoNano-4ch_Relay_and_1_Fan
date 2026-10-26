#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// --- WiFi Credentials ---
const char* ssid = "ash_jio_2G";
const char* password = "ThundersnO0w@,,#";

// --- Controller State ---
bool relayStates[4] = {false, false, false, false};
int dimmerValue = 0; // 0 to 9

// --- Web Server & WebSocket ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Forward declarations
void broadcastState();
void broadcastLog(String message);
void handleCommand(String cmd);
void parseSerialFeedback(String line);

// --- HTML Template (Exact match to original UI) ---
const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP8266 Home Automation Web Controller</title>
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
        button {
            padding: 10px 15px;
            font-size: 14px;
            border-radius: 4px;
            border: none;
            cursor: pointer;
            background-color: #007bff;
            color: white;
            transition: 0.2s;
        }
        button:hover { background-color: #0056b3; }
        
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
        }
        .flex-fill { flex: 1; }
        #connectionStatus { font-weight: bold; padding: 5px 10px; border-radius: 4px; display: inline-block; }
        .status-disconnected { background: #ffebee; color: #c62828; }
        .status-connected { background: #e8f5e9; color: #2e7d32; }
    </style>
</head>
<body>

<div class="container">
    <h2>ESP8266 Home Automation</h2>

    <!-- Connection Status Banner -->
    <div class="card">
        <div class="card-title">ESP8266 Status</div>
        <div id="connectionStatus" class="status-disconnected">Status: Connecting...</div>
    </div>

    <!-- Relays Control -->
    <div class="card">
        <div class="card-title">Relay Controls</div>
        <div class="relay-grid">
            <button id="relay-1" class="relay-btn" onclick="toggleRelay(1)">Relay 1<br>OFF</button>
            <button id="relay-2" class="relay-btn" onclick="toggleRelay(2)">Relay 2<br>OFF</button>
            <button id="relay-3" class="relay-btn" onclick="toggleRelay(3)">Relay 3<br>OFF</button>
            <button id="relay-4" class="relay-btn" onclick="toggleRelay(4)">Relay 4<br>OFF</button>
        </div>
    </div>

    <!-- Dimmer Control -->
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

    <!-- Global & Utilities -->
    <div class="card">
        <div class="card-title">Global Commands & Status</div>
        <div class="row">
            <button class="flex-fill" onclick="sendCommand('ALL_ON')">ALL ON</button>
            <button class="flex-fill" onclick="sendCommand('ALL_OFF')">ALL OFF</button>
            <button class="flex-fill" onclick="sendCommand('STATUS')">Get Status</button>
        </div>
    </div>

    <!-- Console Log -->
    <div class="card">
        <div class="card-title">Serial Console / Feedback</div>
        <div id="console"></div>
    </div>
</div>

<script>
    let ws;
    let relayStates = [false, false, false, false];

    function initWebSocket() {
        ws = new WebSocket('ws://' + window.location.hostname + '/ws');
        
        ws.onopen = function(event) {
            updateConnectionBanner(true);
            appendConsole("Connected to ESP8266 WebSocket server.");
            sendCommand('STATUS');
        };
        
        ws.onclose = function(event) {
            updateConnectionBanner(false);
            appendConsole("Disconnected. Reconnecting in 2s...");
            setTimeout(initWebSocket, 2000);
        };
        
        ws.onmessage = function(event) {
            try {
                const data = JSON.parse(event.data);
                if (data.type === 'console_log') {
                    appendConsole(data.message);
                } else if (data.type === 'state_update') {
                    if (data.relays) {
                        data.relays.forEach((state, i) => updateRelayUI(i + 1, state));
                    }
                    if (data.dimmer !== undefined) {
                        document.getElementById('dimmerSlider').value = data.dimmer;
                    }
                }
            } catch (e) {
                appendConsole(event.data);
            }
        };
    }

    function updateConnectionBanner(connected) {
        const banner = document.getElementById('connectionStatus');
        if (connected) {
            banner.innerText = "Status: Connected (ESP8266 Direct)";
            banner.className = "status-connected";
        } else {
            banner.innerText = "Status: Disconnected";
            banner.className = "status-disconnected";
        }
    }

    function toggleRelay(relayNum) {
        const currentState = relayStates[relayNum - 1];
        sendCommand(`POWER${relayNum} ${currentState ? 'OFF' : 'ON'}`);
    }

    function onSliderChange(val) {
        sendCommand(`DIMMER ${val}`);
    }

    function sendCommand(cmd) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({command: cmd}));
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

    window.onload = initWebSocket;
</script>

</body>
</html>
)rawliteral";

// --- WebSocket Event Handler ---
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        broadcastState();
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            data[len] = 0;
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, (char*)data);
            if (!error && doc.containsKey("command")) {
                String cmd = doc["command"].as<String>();
                handleCommand(cmd);
            }
        }
    }
}

// --- Translate Web UI Actions and Append STATUS ---
void handleCommand(String cmd) {
    cmd.trim();

    for (int i = 1; i <= 4; i++) {
        if (cmd == "POWER" + String(i) + " ON") {
            String jsonOut = "{\"POWER" + String(i) + "\":\"ON\"}";
            Serial.println(jsonOut);
            broadcastLog(">> " + jsonOut);
            delay(10);
            Serial.println("STATUS");
            broadcastLog(">> STATUS");
            return;
        } else if (cmd == "POWER" + String(i) + " OFF") {
            String jsonOut = "{\"POWER" + String(i) + "\":\"OFF\"}";
            Serial.println(jsonOut);
            broadcastLog(">> " + jsonOut);
            delay(10);
            Serial.println("STATUS");
            broadcastLog(">> STATUS");
            return;
        }
    }

    if (cmd.startsWith("DIMMER ")) {
        int val = cmd.substring(7).toInt();
        String jsonOut = "{\"Dimmer\":" + String(val) + "}";
        Serial.println(jsonOut);
        broadcastLog(">> " + jsonOut);
    } else if (cmd == "DIMM_UP") {
        if (dimmerValue < 9) dimmerValue++;
        String jsonOut = "{\"Dimmer\":" + String(dimmerValue) + "}";
        Serial.println(jsonOut);
        broadcastLog(">> " + jsonOut);
    } else if (cmd == "DIMM_DN") {
        if (dimmerValue > 0) dimmerValue--;
        String jsonOut = "{\"Dimmer\":" + String(dimmerValue) + "}";
        Serial.println(jsonOut);
        broadcastLog(">> " + jsonOut);
    } else if (cmd == "ALL_ON") {
        for (int i = 1; i <= 4; i++) {
            String jsonOut = "{\"POWER" + String(i) + "\":\"ON\"}";
            Serial.println(jsonOut);
            broadcastLog(">> " + jsonOut);
            delay(20);
            Serial.println("STATUS");
            broadcastLog(">> STATUS");
            delay(20);
        }
        dimmerValue = 9;
        String dimOut = "{\"Dimmer\":9}";
        Serial.println(dimOut);
        broadcastLog(">> " + dimOut);
        delay(20);
        Serial.println("STATUS");
        broadcastLog(">> STATUS");
        return;
    } else if (cmd == "ALL_OFF") {
        for (int i = 1; i <= 4; i++) {
            String jsonOut = "{\"POWER" + String(i) + "\":\"OFF\"}";
            Serial.println(jsonOut);
            broadcastLog(">> " + jsonOut);
            delay(20);
            Serial.println("STATUS");
            broadcastLog(">> STATUS");
            delay(20);
        }
        dimmerValue = 0;
        String dimOut = "{\"Dimmer\":0}";
        Serial.println(dimOut);
        broadcastLog(">> " + dimOut);
        delay(20);
        Serial.println("STATUS");
        broadcastLog(">> STATUS");
        return;
    } else if (cmd == "STATUS") {
        Serial.println("STATUS");
        broadcastLog(">> STATUS");
        return;
    }

    // Append STATUS after standard slider/dimmer updates
    delay(10);
    Serial.println("STATUS");
    broadcastLog(">> STATUS");
}

// --- Parse Incoming Serial Status JSON from Peripheral ---
void parseSerialFeedback(String line) {
    line.trim();
    if (line.length() == 0) return;

    // Log raw incoming feedback to console
    broadcastLog("<< " + line);

    // Parse incoming JSON feedback (e.g. {"POWER1":"ON","POWER2":"OFF",...})
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, line);
    if (!error) {
        bool stateChanged = false;

        for (int i = 1; i <= 4; i++) {
            String key = "POWER" + String(i);
            if (doc.containsKey(key)) {
                bool isOn = (doc[key].as<String>() == "ON");
                if (relayStates[i - 1] != isOn) {
                    relayStates[i - 1] = isOn;
                    stateChanged = true;
                }
            }
        }

        if (doc.containsKey("Dimmer")) {
            int dimVal = doc["Dimmer"].as<int>();
            if (dimmerValue != dimVal) {
                dimmerValue = dimVal;
                stateChanged = true;
            }
        }

        // Sync web GUI immediately upon receiving peripheral status JSON
        if (stateChanged || doc.containsKey("POWER1") || doc.containsKey("Dimmer")) {
            broadcastState();
        }
    }
}

// --- Broadcast Helpers ---
void broadcastState() {
    JsonDocument doc;
    doc["type"] = "state_update";
    JsonArray relays = doc["relays"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
        relays.add(relayStates[i]);
    }
    doc["dimmer"] = dimmerValue;

    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

void broadcastLog(String message) {
    JsonDocument doc;
    doc["type"] = "console_log";
    doc["message"] = message;

    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

// --- Setup & Loop ---
void setup() {
    Serial.begin(115200);

    // Connect to WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

    // Setup Web Server routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", HTML_TEMPLATE);
    });

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    server.begin();
}

void loop() {
    ws.cleanupClients();

    // Listen continuously for incoming JSON feedback lines over Serial from peripheral
    while (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        parseSerialFeedback(line);
    }
}
