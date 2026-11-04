#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>

Preferences preferences;

// --- Default Fallback Credentials (Pre-filled on first boot) ---
const char* DEFAULT_SSID = "ash_jio_2G";
const char* DEFAULT_PASSWORD = "ThundersnO0w@,,#";

const char* DEFAULT_MQTT_SERVER = "i11b0026.ala.eu-central-1.emqxsl.com";
const int   DEFAULT_MQTT_PORT = 8883;
const char* DEFAULT_MQTT_USER = "admin";
const char* DEFAULT_MQTT_PASS = "admin1234S";
const char* DEFAULT_MQTT_TOPIC_CMD = "home/esp8266/command";
const char* DEFAULT_MQTT_TOPIC_STATE = "home/esp8266/state";

// --- Runtime Configuration Variables ---
String mqtt_server;
int mqtt_port;
String mqtt_user;
String mqtt_pass;
String mqtt_topic_cmd;
String mqtt_topic_state;

// --- Controller State ---
bool relayStates[4] = {false, false, false, false};
int dimmerValue = 0; // 0 to 9

// --- Synchronous Web Server & MQTT ---
ESP8266WebServer server(80);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// --- Serial Buffering State (50ms Silence Timeout) ---
String serialBuffer = "";
unsigned long lastCharTime = 0;
const unsigned long SERIAL_TIMEOUT = 50;

// Verbose log queue for web feedback console
String consoleLogBuffer = "";

// Forward declarations
void loadConfig();
void saveConfig(String srv, int prt, String usr, String pwd, String tCmd, String tState);
void handleCommand(String cmd);
void processBufferedSerial(String data);
void reconnectMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
String getStateJson(String logMsg = "");
void logMessage(String msg);

// --- Main Dashboard HTML Template (Includes Verbose Console & Config Link) ---
const char MAIN_PAGE[] PROGMEM = R"rawliteral(
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
            position: relative;
        }
        .config-link {
            position: absolute;
            top: 20px;
            right: 20px;
            text-decoration: none;
            background-color: #6c757d;
            color: white;
            padding: 8px 12px;
            font-size: 13px;
            border-radius: 4px;
            font-weight: bold;
            transition: 0.2s;
        }
        .config-link:hover { background-color: #5a6268; }
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
            white-space: pre-wrap;
        }
        .flex-fill { flex: 1; }
        #connectionStatus { font-weight: bold; padding: 5px 10px; border-radius: 4px; display: inline-block; }
        .status-disconnected { background: #ffebee; color: #c62828; }
        .status-connected { background: #e8f5e9; color: #2e7d32; }
    </style>
</head>
<body>

<div class="container">
    <a href="/config" class="config-link">⚙️ Config</a>
    <h2>ESP8266 Home Automation</h2>

    <!-- Connection Status Banner -->
    <div class="card">
        <div class="card-title">ESP8266 Status</div>
        <div id="connectionStatus" class="status-connected">Status: Connected (ESP8266 Direct)</div>
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
            <button onclick="sendAction('DIMMER 0')">MIN (0)</button>
            <button onclick="sendAction('DIMM_DN')">Step -</button>
            <input type="range" id="dimmerSlider" class="flex-fill" min="0" max="9" value="0" onchange="onSliderChange(this.value)">
            <button onclick="sendAction('DIMM_UP')">Step +</button>
            <button onclick="sendAction('DIMMER 9')">MAX (9)</button>
        </div>
    </div>

    <!-- Global & Utilities -->
    <div class="card">
        <div class="card-title">Global Commands & Status</div>
        <div class="row">
            <button class="flex-fill" onclick="sendAction('ALL_ON')">ALL ON</button>
            <button class="flex-fill" onclick="sendAction('ALL_OFF')">ALL OFF</button>
            <button class="flex-fill" onclick="sendAction('STATUS')">Get Status</button>
        </div>
    </div>

    <!-- Console Log -->
    <div class="card">
        <div class="card-title">Serial Console / Feedback</div>
        <div id="console"></div>
    </div>
</div>

<script>
    let relayStates = [false, false, false, false];

    function toggleRelay(relayNum) {
        const currentState = relayStates[relayNum - 1];
        sendAction(`POWER${relayNum} ${currentState ? 'OFF' : 'ON'}`);
    }

    function onSliderChange(val) {
        sendAction(`DIMMER ${val}`);
    }

    function sendAction(cmd) {
        fetch('/cmd?action=' + encodeURIComponent(cmd))
            .then(res => res.json())
            .then(data => updateUIState(data));
    }

    function pollState() {
        fetch('/status-json')
            .then(res => res.json())
            .then(data => updateUIState(data));
    }

    function updateUIState(data) {
        if (data.relays) {
            data.relays.forEach((state, i) => {
                relayStates[i] = state;
                const btn = document.getElementById(`relay-${i + 1}`);
                if (state) {
                    btn.innerText = `Relay ${i + 1}\nON`;
                    btn.classList.add('on');
                } else {
                    btn.innerText = `Relay ${i + 1}\nOFF`;
                    btn.classList.remove('on');
                }
            });
        }
        if (data.dimmer !== undefined) {
            document.getElementById('dimmerSlider').value = data.dimmer;
        }
        if (data.log) {
            appendConsole(data.log);
        }
    }

    function appendConsole(msg) {
        const consoleEl = document.getElementById('console');
        consoleEl.innerHTML += msg + '\n';
        consoleEl.scrollTop = consoleEl.scrollHeight;
    }

    window.onload = function() {
        pollState();
        setInterval(pollState, 2000);
    };
</script>

</body>
</html>
)rawliteral";

// --- Dedicated MQTT Configuration Page HTML ---
const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>MQTT Configuration</title>
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
            max-width: 500px;
            background: #ffffff;
            border-radius: 8px;
            padding: 20px;
            box-shadow: 0 4px 10px rgba(0,0,0,0.1);
        }
        h2 { text-align: center; margin-top: 0; color: #333; }
        .back-link {
            display: inline-block;
            margin-bottom: 15px;
            text-decoration: none;
            color: #007bff;
            font-weight: bold;
            font-size: 14px;
        }
        .back-link:hover { text-decoration: underline; }
        label { font-weight: bold; font-size: 13px; color: #555; display: block; margin-top: 10px; }
        input[type="text"], input[type="password"], input[type="number"] {
            padding: 8px;
            border: 1px solid #ccc;
            border-radius: 4px;
            width: 100%;
            box-sizing: border-box;
            margin-top: 5px;
            margin-bottom: 5px;
        }
        button {
            width: 100%;
            padding: 12px;
            font-size: 15px;
            border-radius: 4px;
            border: none;
            cursor: pointer;
            background-color: #28a745;
            color: white;
            font-weight: bold;
            margin-top: 15px;
            transition: 0.2s;
        }
        button:hover { background-color: #218838; }
    </style>
</head>
<body>

<div class="container">
    <a href="/" class="back-link">&larr; Back to Dashboard</a>
    <h2>MQTT Configuration</h2>
    
    <form action="/save-settings" method="POST">
        <label>MQTT Server:</label>
        <input type="text" name="mqtt_server" id="cfg_server">
        
        <label>MQTT Port:</label>
        <input type="number" name="mqtt_port" id="cfg_port">
        
        <label>MQTT Username:</label>
        <input type="text" name="mqtt_user" id="cfg_user">
        
        <label>MQTT Password:</label>
        <input type="password" name="mqtt_pass" id="cfg_pass">
        
        <label>Command Topic:</label>
        <input type="text" name="mqtt_topic_cmd" id="cfg_tcmd">
        
        <label>State Topic:</label>
        <input type="text" name="mqtt_topic_state" id="cfg_tstate">
        
        <button type="submit">Save & Reboot</button>
    </form>
</div>

<script>
    fetch('/get-config')
        .then(res => res.json())
        .then(data => {
            document.getElementById('cfg_server').value = data.server;
            document.getElementById('cfg_port').value = data.port;
            document.getElementById('cfg_user').value = data.user;
            document.getElementById('cfg_pass').value = data.pass;
            document.getElementById('cfg_tcmd').value = data.tcmd;
            document.getElementById('cfg_tstate').value = data.tstate;
        });
</script>

</body>
</html>
)rawliteral";

// --- Load and Save Configurations via Preferences ---
void loadConfig() {
    preferences.begin("config", false);
    mqtt_server = preferences.getString("server", DEFAULT_MQTT_SERVER);
    mqtt_port = preferences.getInt("port", DEFAULT_MQTT_PORT);
    mqtt_user = preferences.getString("user", DEFAULT_MQTT_USER);
    mqtt_pass = preferences.getString("pass", DEFAULT_MQTT_PASS);
    mqtt_topic_cmd = preferences.getString("tcmd", DEFAULT_MQTT_TOPIC_CMD);
    mqtt_topic_state = preferences.getString("tstate", DEFAULT_MQTT_TOPIC_STATE);
    preferences.end();
}

void saveConfig(String srv, int prt, String usr, String pwd, String tCmd, String tState) {
    preferences.begin("config", false);
    preferences.putString("server", srv);
    preferences.putInt("port", prt);
    preferences.putString("user", usr);
    preferences.putString("pass", pwd);
    preferences.putString("tcmd", tCmd);
    preferences.putString("tstate", tState);
    preferences.end();
}

// --- Verbose Console Logger ---
void logMessage(String msg) {
    Serial.println(msg);
    if (consoleLogBuffer.length() > 2000) {
        consoleLogBuffer = consoleLogBuffer.substring(consoleLogBuffer.length() - 1500);
    }
    consoleLogBuffer += msg + "\n";
}

// --- MQTT Callback ---
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    message.trim();
    
    logMessage("<< [MQTT Sync] " + message);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (!error && doc["type"] == "state_update") {
        if (doc.containsKey("relays")) {
            JsonArray relays = doc["relays"];
            for (int i = 0; i < 4; i++) {
                relayStates[i] = relays[i];
            }
        }
        if (doc.containsKey("dimmer")) {
            dimmerValue = doc["dimmer"];
        }
    }
}

// --- MQTT Reconnect ---
void reconnectMQTT() {
    if (!mqttClient.connected()) {
        logMessage("Connecting to MQTT Broker " + mqtt_server + ":" + String(mqtt_port) + "...");
        String clientId = "ESP8266Client-" + String(ESP.getChipId());
        if (mqttClient.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
            logMessage("Connected to EMQX Broker successfully!");
            mqttClient.subscribe(mqtt_topic_state.c_str());
            logMessage("Subscribed to state topic: " + mqtt_topic_state);
        } else {
            logMessage("MQTT connection failed, rc=" + String(mqttClient.state()) + ". Retrying in 5s...");
        }
    }
}

// --- State JSON Helper with Verbose Logs ---
String getStateJson(String logMsg) {
    if (logMsg.length() > 0) {
        logMessage(logMsg);
    }

    JsonDocument doc;
    JsonArray relays = doc["relays"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
        relays.add(relayStates[i]);
    }
    doc["dimmer"] = dimmerValue;
    
    // Send accumulated verbose console logs to UI
    doc["log"] = consoleLogBuffer;
    consoleLogBuffer = ""; // Clear buffer after sending to prevent duplication

    String output;
    serializeJson(doc, output);
    return output;
}

// --- Translate Web UI Actions ---
void handleCommand(String cmd) {
    cmd.trim();
    logMessage("Processing command: " + cmd);

    JsonDocument cmdDoc;
    cmdDoc["command"] = cmd;
    String cmdPayload;
    serializeJson(cmdDoc, cmdPayload);
    mqttClient.publish(mqtt_topic_cmd.c_str(), cmdPayload.c_str());
    logMessage("Published to MQTT [" + mqtt_topic_cmd + "]: " + cmdPayload);

    for (int i = 1; i <= 4; i++) {
        if (cmd == "POWER" + String(i) + " ON") {
            relayStates[i - 1] = true;
            String out = "{\"POWER" + String(i) + "\":\"ON\"}";
            Serial.println(out);
            logMessage(">> " + out);
            delay(10);
            Serial.println("STATUS");
            logMessage(">> STATUS");
            return;
        } else if (cmd == "POWER" + String(i) + " OFF") {
            relayStates[i - 1] = false;
            String out = "{\"POWER" + String(i) + "\":\"OFF\"}";
            Serial.println(out);
            logMessage(">> " + out);
            delay(10);
            Serial.println("STATUS");
            logMessage(">> STATUS");
            return;
        }
    }

    if (cmd.startsWith("DIMMER ")) {
        dimmerValue = cmd.substring(7).toInt();
        String out = "{\"Dimmer\":" + String(dimmerValue) + "}";
        Serial.println(out);
        logMessage(">> " + out);
    } else if (cmd == "DIMM_UP") {
        if (dimmerValue < 9) dimmerValue++;
        String out = "{\"Dimmer\":" + String(dimmerValue) + "}";
        Serial.println(out);
        logMessage(">> " + out);
    } else if (cmd == "DIMM_DN") {
        if (dimmerValue > 0) dimmerValue--;
        String out = "{\"Dimmer\":" + String(dimmerValue) + "}";
        Serial.println(out);
        logMessage(">> " + out);
    } else if (cmd == "ALL_ON") {
        for (int i = 0; i < 4; i++) relayStates[i] = true;
        dimmerValue = 9;
        Serial.println("{\"ALL\":\"ON\"}");
        logMessage(">> {\"ALL\":\"ON\"}");
    } else if (cmd == "ALL_OFF") {
        for (int i = 0; i < 4; i++) relayStates[i] = false;
        dimmerValue = 0;
        Serial.println("{\"ALL\":\"OFF\"}");
        logMessage(">> {\"ALL\":\"OFF\"}");
    } else if (cmd == "STATUS") {
        Serial.println("STATUS");
        logMessage(">> STATUS");
    }
}

// --- Serial Batch Processing ---
void processBufferedSerial(String data) {
    String bufferCopy = data;
    bufferCopy.trim();
    if (bufferCopy.length() == 0) return;

    logMessage("<< [Serial Batch Received (" + String(bufferCopy.length()) + " bytes)]");

    int startIdx = 0;
    while (startIdx < (int)bufferCopy.length()) {
        int nextNewline = bufferCopy.indexOf('\n', startIdx);
        if (nextNewline == -1) nextNewline = bufferCopy.length();

        String line = bufferCopy.substring(startIdx, nextNewline);
        line.trim();

        if (line.length() > 0) {
            logMessage("   Line: " + line);
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, line);
            if (!error) {
                for (int i = 1; i <= 4; i++) {
                    String key = "POWER" + String(i);
                    if (doc.containsKey(key)) {
                        relayStates[i - 1] = (doc[key].as<String>() == "ON");
                    }
                }
                if (doc.containsKey("Dimmer")) {
                    dimmerValue = doc["Dimmer"].as<int>();
                }
            }
        }
        startIdx = nextNewline + 1;
    }
}

// --- Setup & Loop ---
void setup() {
    Serial.begin(115200);
    delay(1000);
    logMessage("\nBooting ESP8266 Home Automation Controller...");

    // 1. Load Stored MQTT Configurations
    loadConfig();
    logMessage("Loaded MQTT Config: Server=" + mqtt_server + ", Port=" + String(mqtt_port));

    // 2. WiFiManager Configuration Portal Setup (Passwordless Open AP)
    WiFiManager wm;
    
    wm.setAPCallback([](WiFiManager *myWiFiManager) {
        logMessage("Entered AP Mode (Captive Portal active)");
        logMessage("Connect to SSID: " + String(myWiFiManager->getConfigPortalSSID()));
    });

    logMessage("Attempting Wi-Fi auto-connection...");
    bool res = wm.autoConnect("ESP8266-ConfigPortal");
    if(!res) {
        logMessage("Failed to connect or hit timeout. Restarting...");
        delay(3000);
        ESP.restart();
    }

    logMessage("Connected to WiFi successfully! IP Address: " + WiFi.localIP().toString());

    // 3. Configure Secure MQTT Client
    espClient.setInsecure();
    mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
    mqttClient.setCallback(callbackMQTT);
    mqttClient.setBufferSize(512);

    // 4. Setup Synchronous Web Server Routes
    server.on("/", [](){
        server.send_P(200, "text/html", MAIN_PAGE);
    });

    server.on("/config", [](){
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    server.on("/get-config", [](){
        JsonDocument doc;
        doc["server"] = mqtt_server;
        doc["port"] = mqtt_port;
        doc["user"] = mqtt_user;
        doc["pass"] = mqtt_pass;
        doc["tcmd"] = mqtt_topic_cmd;
        doc["tstate"] = mqtt_topic_state;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/cmd", [](){
        if (server.hasArg("action")) {
            String action = server.arg("action");
            handleCommand(action);
            server.send(200, "application/json", getStateJson(">> Executed: " + action));
        } else {
            server.send(400, "application/json", "{\"error\":\"Missing action\"}");
        }
    });

    server.on("/status-json", [](){
        server.send(200, "application/json", getStateJson());
    });

    server.on("/save-settings", HTTP_POST, [](){
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;

        saveConfig(srv, prt, usr, pwd, tCmd, tState);
        logMessage("New MQTT settings saved to Preferences! Rebooting...");

        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP8266...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });

    server.begin();
    logMessage("HTTP Web Server started on port 80.");
}

void loop() {
    if (!mqttClient.connected()) {
        reconnectMQTT();
    }
    mqttClient.loop();

    server.handleClient();

    while (Serial.available()) {
        char c = Serial.read();
        lastCharTime = millis();
        if (serialBuffer.length() < 512) {
            serialBuffer += c;
        } else {
            serialBuffer = "";
        }
    }

    if (serialBuffer.length() > 0 && (millis() - lastCharTime >= SERIAL_TIMEOUT)) {
        processBufferedSerial(serialBuffer);
        serialBuffer = "";
    }
}
