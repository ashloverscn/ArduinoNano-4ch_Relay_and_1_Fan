#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Update.h>

Preferences preferences;

// --- Default Fallback Credentials (Pre-filled on first boot) ---
const char* DEFAULT_SSID = "ash_jio_2G";
const char* DEFAULT_PASSWORD = "ThundersnO0w@,,#";

const char* DEFAULT_MQTT_SERVER = "i11b0026.ala.eu-central-1.emqxsl.com";
const int   DEFAULT_MQTT_PORT = 8883;
const char* DEFAULT_MQTT_USER = "admin";
const char* DEFAULT_MQTT_PASS = "admin1234S";
const char* DEFAULT_MQTT_TOPIC_CMD = "home/esp32/command";
const char* DEFAULT_MQTT_TOPIC_STATE = "home/esp32/state";

// --- Runtime Configuration Variables ---
String stored_ssid;
String stored_password;
String mqtt_server;
int mqtt_port;
String mqtt_user;
String mqtt_pass;
String mqtt_topic_cmd;
String mqtt_topic_state;
bool mqtt_enabled = true;

const char* CONFIG_LOGIN_USER = "admin";
const char* CONFIG_LOGIN_PASS = "admin1234S";

// --- Modern Web Server, WebSocket & MQTT ---
WebServer server(80);
bool requireConfigAuth() {
    if (server.authenticate(CONFIG_LOGIN_USER, CONFIG_LOGIN_PASS)) return true;
    server.requestAuthentication(BASIC_AUTH, "ESP32 Configuration");
    return false;
}

WebSocketsServer webSocket(81);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// --- Serial Buffering State (50ms Silence Timeout) ---
String serialBuffer = "";
unsigned long lastCharTime = 0;
const unsigned long SERIAL_TIMEOUT = 50;

// --- Wi-Fi Reconnection & AP Fallback State ---
unsigned long lastWiFiCheckTime = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000; 
unsigned long wifiDisconnectedStartTime = 0;        
const unsigned long WIFI_AP_FALLBACK_TIMEOUT = 600000; // 10 minutes

// --- Non-Blocking MQTT Reconnect State ---
unsigned long lastMqttRetry = 0;
const unsigned long MQTT_RETRY_INTERVAL = 10000; // 10 seconds retry delay

// --- GPIO 0 Button Press States (3s AP Mode & 10s Factory Reset) ---
const int CONFIG_BUTTON_PIN = 0;
unsigned long buttonPressStartTime = 0;
bool buttonStateLast = HIGH;
const unsigned long LONG_PRESS_DURATION = 3000;       // 3 seconds for AP Mode
const unsigned long VERY_LONG_PRESS_DURATION = 10000; // 10 seconds for NVS Factory Reset
bool apTriggered = false;
bool factoryResetTriggered = false;
bool apModeActive = false;

// Forward declarations
void loadConfig();
void saveConfig(String ssid, String pass, String srv, int prt, String usr, String pwd, String tCmd, String tState, bool mqttEnabled);
void logMessage(const String& msg);
void handleCommand(const String& cmd);
void processBufferedSerial(const String& data);
void reconnectMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void checkWiFiConnection();
void checkButton();
void enterAPMode();
void setupOTARoutes();

// --- HTML Template (Dashboard) ---
const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Home Automation Controller</title>
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
    <h2>Home Automation Controller</h2>

    <!-- Connection Status Banner -->
    <div class="card">
        <div class="card-title">ESP32 Gateway Status</div>
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
            <button onclick="sendCommand('DIMMER:0')">MIN (0)</button>
            <button onclick="stepDimmer(-1)">Step -</button>
            <input type="range" id="dimmerSlider" class="flex-fill" min="0" max="9" value="0" onchange="onSliderChange(this.value)">
            <button onclick="stepDimmer(1)">Step +</button>
            <button onclick="sendCommand('DIMMER:9')">MAX (9)</button>
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
    let reconnectAttempts = 0;

    function initWebSocket() {
        ws = new WebSocket('ws://' + window.location.hostname + ':81/');
        
        ws.onopen = function(event) {
            reconnectAttempts = 0;
            updateConnectionBanner(true);
            appendConsole("Connected to ESP32 WebSocket server.");
            sendCommand('STATUS');
        };
        
        ws.onclose = function(event) {
            updateConnectionBanner(false);
            let timeout = Math.min(1000 * Math.pow(1.5, reconnectAttempts), 10000);
            appendConsole(`Disconnected. Reconnecting in ${Math.round(timeout/1000)}s...`);
            setTimeout(initWebSocket, timeout);
            reconnectAttempts++;
        };
        
        ws.onmessage = function(event) {
            try {
                const data = JSON.parse(event.data);
                if (data.type === 'console_log') {
                    appendConsole(data.message);
                } else {
                    if (data.POWER1 !== undefined) updateRelayUI(1, data.POWER1 === 'ON');
                    if (data.POWER2 !== undefined) updateRelayUI(2, data.POWER2 === 'ON');
                    if (data.POWER3 !== undefined) updateRelayUI(3, data.POWER3 === 'ON');
                    if (data.POWER4 !== undefined) updateRelayUI(4, data.POWER4 === 'ON');
                    if (data.Dimmer !== undefined) {
                        document.getElementById('dimmerSlider').value = data.Dimmer;
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
            banner.innerText = "Status: Connected";
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
        sendCommand(`DIMMER:${val}`);
    }

    function stepDimmer(direction) {
        const slider = document.getElementById('dimmerSlider');
        let newVal = parseInt(slider.value) + direction;
        if (newVal < 0) newVal = 0;
        if (newVal > 9) newVal = 9;
        slider.value = newVal;
        sendCommand(`DIMMER:${newVal}`);
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
        consoleEl.innerHTML += msg + '\n';
        consoleEl.scrollTop = consoleEl.scrollHeight;
    }

    window.onload = initWebSocket;
</script>

</body>
</html>
)rawliteral";

// --- Configuration Page HTML (Includes OTA Firmware Upgrade Section) ---
const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Device Configuration</title>
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
        input[type="text"], input[type="password"], input[type="number"], input[type="file"] {
            padding: 8px;
            border: 1px solid #ccc;
            border-radius: 4px;
            width: 100%;
            box-sizing: border-box;
            margin-top: 5px;
            margin-bottom: 5px;
        }
        hr { border: 0; border-top: 1px solid #eee; margin: 20px 0; }
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
        .ota-btn { background-color: #17a2b8; }
        .ota-btn:hover { background-color: #138496; }
        #progressBarContainer {
            width: 100%;
            background-color: #e0e0e0;
            border-radius: 4px;
            margin-top: 10px;
            display: none;
            overflow: hidden;
        }
        #progressBar {
            width: 0%;
            height: 20px;
            background-color: #28a745;
            text-align: center;
            line-height: 20px;
            color: white;
            font-size: 12px;
        }
    </style>
</head>
<body>

<div class="container">
    <a href="/" class="back-link">&larr; Back to Dashboard</a>
    <h2>Device Configuration</h2>
    
    <form action="/save-settings" method="POST">
        <h3>Wi-Fi Settings</h3>
        <label>Wi-Fi SSID:</label>
        <input type="text" name="wifi_ssid" id="cfg_ssid">
        
        <label>Wi-Fi Password:</label>
        <input type="password" name="wifi_pass" id="cfg_pass_wifi">

        <hr>
        <h3>MQTT Settings</h3>
        <label style="display:flex;align-items:center;gap:10px;">
            <input type="checkbox" name="mqtt_enabled" id="cfg_mqtt_enabled" value="1" style="width:auto;">
            Enable MQTT
        </label>
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

    <hr>
    <h3>Firmware Update (OTA)</h3>
    <form id="otaForm" onsubmit="uploadFirmware(event)">
        <label>Select Compiled Firmware (.bin):</label>
        <input type="file" id="firmwareFile" accept=".bin" required>
        <button type="submit" class="ota-btn">Upload & Update Firmware</button>
        <div id="progressBarContainer">
            <div id="progressBar">0%</div>
        </div>
    </form>

    <hr>
    <h3>Device Recovery</h3>
    <button type="button" onclick="bootAP()" style="background:#fd7e14;">Reboot into AP Configuration Mode</button>
    <button type="button" onclick="factoryReset()" style="background:#dc3545;">Factory Reset & Boot into AP Mode</button>
</div>

<script>
    function postAction(path, message) {
        if (!confirm(message)) return;
        fetch(path, {method:'POST'}).then(async r => {
            const t = await r.text();
            if (!r.ok) throw new Error(t || 'Request failed');
            alert(t);
        }).catch(e => alert('Action failed: ' + e.message));
    }
    function bootAP() {
        postAction('/boot-ap', 'Switch the device to AP configuration mode?');
    }
    function factoryReset() {
        postAction('/factory-reset', 'Erase all saved Wi-Fi and MQTT settings and boot into AP mode?');
    }

    fetch('/get-config')
        .then(res => res.json())
        .then(data => {
            document.getElementById('cfg_mqtt_enabled').checked = data.mqtt_enabled !== false;
            document.getElementById('cfg_ssid').value = data.ssid || '';
            document.getElementById('cfg_pass_wifi').value = data.pass_wifi || '';
            document.getElementById('cfg_server').value = data.server;
            document.getElementById('cfg_port').value = data.port;
            document.getElementById('cfg_user').value = data.user;
            document.getElementById('cfg_pass').value = data.pass;
            document.getElementById('cfg_tcmd').value = data.tcmd;
            document.getElementById('cfg_tstate').value = data.tstate;
        });

    function uploadFirmware(event) {
        event.preventDefault();
        const fileInput = document.getElementById('firmwareFile');
        if (fileInput.files.length === 0) return;

        const file = fileInput.files[0];
        const formData = new FormData();
        formData.append('firmware', file);

        const progressBarContainer = document.getElementById('progressBarContainer');
        const progressBar = document.getElementById('progressBar');
        progressBarContainer.style.display = 'block';
        progressBar.style.width = '0%';
        progressBar.innerText = '0%';

        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/update', true);

        xhr.upload.onprogress = function (e) {
            if (e.lengthComputable) {
                const percentComplete = Math.round((e.loaded / e.total) * 100);
                progressBar.style.width = percentComplete + '%';
                progressBar.innerText = percentComplete + '%';
            }
        };

        xhr.onload = function () {
            if (xhr.status === 200) {
                alert('Firmware update successful! Restarting device...');
                setTimeout(() => { window.location.reload(); }, 5000);
            } else {
                alert('Firmware update failed: ' + xhr.responseText);
                progressBarContainer.style.display = 'none';
            }
        };

        xhr.onerror = function () {
            alert('An error occurred during the firmware upload.');
            progressBarContainer.style.display = 'none';
        };

        xhr.send(formData);
    }
</script>

</body>
</html>
)rawliteral";

// --- Load and Save Configurations via Preferences ---
void loadConfig() {
    preferences.begin("config", false);
    stored_ssid = preferences.getString("ssid", DEFAULT_SSID);
    stored_password = preferences.getString("password", DEFAULT_PASSWORD);
    mqtt_server = preferences.getString("server", DEFAULT_MQTT_SERVER);
    mqtt_port = preferences.getInt("port", DEFAULT_MQTT_PORT);
    mqtt_user = preferences.getString("user", DEFAULT_MQTT_USER);
    mqtt_pass = preferences.getString("pass", DEFAULT_MQTT_PASS);
    mqtt_topic_cmd = preferences.getString("tcmd", DEFAULT_MQTT_TOPIC_CMD);
    mqtt_topic_state = preferences.getString("tstate", DEFAULT_MQTT_TOPIC_STATE);
    mqtt_enabled = preferences.getBool("mqtt_enabled", true);
    preferences.end();
}

void saveConfig(String ssid, String pass, String srv, int prt, String usr, String pwd, String tCmd, String tState, bool mqttEnabled) {
    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", pass);
    preferences.putString("server", srv);
    preferences.putInt("port", prt);
    preferences.putString("user", usr);
    preferences.putString("pass", pwd);
    preferences.putString("tcmd", tCmd);
    preferences.putString("tstate", tState);
    preferences.putBool("mqtt_enabled", mqttEnabled);
    preferences.end();
}

// --- Verbose Console Logger ---
void logMessage(const String& msg) {
    Serial.println(msg);
    JsonDocument doc;
    doc["type"] = "console_log";
    doc["message"] = msg;
    String output;
    serializeJson(doc, output);
    webSocket.broadcastTXT(output);
}

// --- Continuous Non-Blocking Wi-Fi Reconnection & 10-Min AP Fallback ---
void checkWiFiConnection() {
    if (!apModeActive) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiDisconnectedStartTime = 0; 
        } else {
            if (wifiDisconnectedStartTime == 0) {
                wifiDisconnectedStartTime = millis();
                logMessage(">> WiFi lost. Starting 10-minute AP fallback timer...");
            } 
            else if (millis() - wifiDisconnectedStartTime >= WIFI_AP_FALLBACK_TIMEOUT) {
                logMessage(">> WiFi reconnection timed out (10 mins exceeded). Entering AP Mode automatically...");
                enterAPMode();
                return;
            }

            unsigned long currentMillis = millis();
            if (currentMillis - lastWiFiCheckTime >= WIFI_RETRY_INTERVAL) {
                lastWiFiCheckTime = currentMillis;
                logMessage(">> WiFi Disconnected. Attempting reconnection...");
                WiFi.disconnect();
                WiFi.begin(stored_ssid.c_str(), stored_password.c_str());
            }
        }
    }
}

// --- GPIO 0 Button Handler (3s AP Mode & 10s Factory Reset) ---
void checkButton() {
    int currentState = digitalRead(CONFIG_BUTTON_PIN);
    
    if (currentState == LOW && buttonStateLast == HIGH) {
        buttonPressStartTime = millis();
        buttonStateLast = LOW;
        apTriggered = false;
        factoryResetTriggered = false;
    } 
    else if (currentState == LOW && buttonStateLast == LOW) {
        unsigned long heldTime = millis() - buttonPressStartTime;
        
        // 10-Second Very Long Press: Erase NVS / Factory Reset
        if (heldTime >= VERY_LONG_PRESS_DURATION && !factoryResetTriggered) {
            factoryResetTriggered = true;
            logMessage(">> [FACTORY RESET] 10s Very Long Press detected! Erasing NVS storage...");
            
            preferences.begin("config", false);
            preferences.clear();
            preferences.end();
            
            logMessage(">> NVS erased successfully. Restarting as fresh first-boot device...");
            delay(1500);
            ESP.restart();
        }
        // 3-Second Long Press: Enter AP Configuration Mode
        else if (heldTime >= LONG_PRESS_DURATION && heldTime < VERY_LONG_PRESS_DURATION && !apTriggered && !apModeActive) {
            apTriggered = true;
            logMessage(">> 3s Long Press detected! Entering AP Configuration Mode...");
            enterAPMode();
        }
    } 
    else if (currentState == HIGH && buttonStateLast == LOW) {
        buttonStateLast = HIGH;
    }
}

// --- Setup OTA Update HTTP Handlers ---
void setupOTARoutes() {
    server.on("/update", HTTP_POST, []() {
        if (!requireConfigAuth()) return;
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        delay(1000);
        ESP.restart();
    }, []() {
        if (!server.authenticate(CONFIG_LOGIN_USER, CONFIG_LOGIN_PASS)) return;
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { 
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) { 
                Serial.printf("Update Success: %u bytes\nRebooting...\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
    });
}

// --- Enter Access Point Mode for Reconfiguration ---
void enterAPMode() {
    apModeActive = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    
    bool apStarted = WiFi.softAP("ESP32-Setup-AP", "12345678");
    if (apStarted) {
        logMessage(">> AP Mode Started Successfully!");
        logMessage(">> SSID: ESP32-Setup-AP | Password: 12345678");
        logMessage(">> Connect and open http://192.168.4.1");
    } else {
        logMessage(">> Failed to start AP Mode!");
    }

    webSocket.close();
    server.stop();

    server.on("/", HTTP_GET, []() {
        if (!requireConfigAuth()) return;
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    server.on("/get-config", HTTP_GET, []() {
        if (!requireConfigAuth()) return;
        JsonDocument doc;
        doc["ssid"] = stored_ssid;
        doc["pass_wifi"] = stored_password;
        doc["server"] = mqtt_server;
        doc["port"] = mqtt_port;
        doc["user"] = mqtt_user;
        doc["pass"] = mqtt_pass;
        doc["tcmd"] = mqtt_topic_cmd;
        doc["tstate"] = mqtt_topic_state;
        doc["mqtt_enabled"] = mqtt_enabled;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/save-settings", HTTP_POST, []() {
        if (!requireConfigAuth()) return;
        String s_ssid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : DEFAULT_SSID;
        String s_pass = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : DEFAULT_PASSWORD;
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;
        bool mqttEnabled = server.hasArg("mqtt_enabled");

        saveConfig(s_ssid, s_pass, srv, prt, usr, pwd, tCmd, tState, mqttEnabled);

        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP32...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });

    server.on("/boot-ap", HTTP_POST, []() {
        if (!requireConfigAuth()) return;
        server.send(200, "text/plain", "Switching to AP configuration mode...");
        delay(300);
        enterAPMode();
    });

    server.on("/factory-reset", HTTP_POST, []() {
        if (!requireConfigAuth()) return;
        preferences.begin("config", false);
        preferences.clear();
        preferences.end();
        stored_ssid = DEFAULT_SSID;
        stored_password = DEFAULT_PASSWORD;
        mqtt_server = DEFAULT_MQTT_SERVER;
        mqtt_port = DEFAULT_MQTT_PORT;
        mqtt_user = DEFAULT_MQTT_USER;
        mqtt_pass = DEFAULT_MQTT_PASS;
        mqtt_topic_cmd = DEFAULT_MQTT_TOPIC_CMD;
        mqtt_topic_state = DEFAULT_MQTT_TOPIC_STATE;
        mqtt_enabled = true;
        server.send(200, "text/plain", "Factory reset complete. Starting AP mode...");
        delay(300);
        enterAPMode();
    });

    setupOTARoutes();
    server.begin();
}

// --- WebSocket Event Callback ---
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            break;
        case WStype_CONNECTED:
            Serial.println("STATUS");
            break;
        case WStype_TEXT: {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload, length);
            if (!error && doc.containsKey("command")) {
                String cmd = doc["command"].as<String>();
                handleCommand(cmd);
            }
            break;
        }
        default:
            break;
    }
}

// --- MQTT Callback for Inbound Commands ---
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
    if (!mqtt_enabled) return;
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    message.trim();
    
    logMessage("<< [MQTT Received on " + String(topic) + "]: " + message);

    if (String(topic) == mqtt_topic_cmd) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, message);
        if (!error && doc.containsKey("command")) {
            String cmd = doc["command"].as<String>();
            handleCommand(cmd);
        } else {
            handleCommand(message);
        }
    }
}

// --- Non-Blocking Reconnect MQTT ---
void reconnectMQTT() {
    if (mqtt_enabled && !apModeActive && !mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        unsigned long currentMillis = millis();
        if (currentMillis - lastMqttRetry >= MQTT_RETRY_INTERVAL) {
            lastMqttRetry = currentMillis;
            logMessage(">> Attempting MQTT connection...");
            String clientId = "ESP32Gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);
            
            // Note: PubSubClient connect() handles its own socket timeout, but spacing it out 
            // via intervals prevents tight blocking loops that starve the web server and websockets.
            if (mqttClient.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
                logMessage(">> MQTT Connected!");
                mqttClient.subscribe(mqtt_topic_cmd.c_str());
            } else {
                logMessage(">> MQTT Connection failed, rc=" + String(mqttClient.state()) + ". Retrying later...");
            }
        }
    }
}

// --- Bridge Command Dispatcher ---
void handleCommand(const String& cmd) {
    String trimmedCmd = cmd;
    trimmedCmd.trim();

    JsonDocument cmdDoc;
    cmdDoc["command"] = trimmedCmd;
    String pubPayload;
    serializeJson(cmdDoc, pubPayload);
    if (mqtt_enabled && mqttClient.connected()) {
        mqttClient.publish(mqtt_topic_cmd.c_str(), pubPayload.c_str());
    }

    Serial.println(trimmedCmd);
    logMessage(">> " + trimmedCmd);
}

// --- Serial Batch Parser (50ms Silence Timeout) ---
void processBufferedSerial(const String& data) {
    String bufferCopy = data;
    bufferCopy.trim();
    if (bufferCopy.length() == 0) return;

    int startIdx = 0;
    while (startIdx < (int)bufferCopy.length()) {
        int nextNewline = bufferCopy.indexOf('\n', startIdx);
        if (nextNewline == -1) nextNewline = bufferCopy.length();

        String line = bufferCopy.substring(startIdx, nextNewline);
        line.trim();

        if (line.length() > 0) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, line);
            if (!error) {
                webSocket.broadcastTXT(line);
                if (mqtt_enabled && mqttClient.connected()) {
                    mqttClient.publish(mqtt_topic_state.c_str(), line.c_str());
                }
            } else {
                logMessage(line);
            }
        }
        startIdx = nextNewline + 1;
    }
}

// --- Setup ---
void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

    loadConfig();

    WiFi.begin(stored_ssid.c_str(), stored_password.c_str());
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        if (millis() - startAttempt > 15000) {
            break;
        }
    }

    espClient.setInsecure();
    mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
    mqttClient.setCallback(callbackMQTT);
    mqttClient.setBufferSize(512);

    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html", HTML_TEMPLATE);
    });

    server.on("/config", HTTP_GET, []() {
        if (!requireConfigAuth()) return;
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    server.on("/get-config", HTTP_GET, []() {
        if (!requireConfigAuth()) return;
        JsonDocument doc;
        doc["ssid"] = stored_ssid;
        doc["pass_wifi"] = stored_password;
        doc["server"] = mqtt_server;
        doc["port"] = mqtt_port;
        doc["user"] = mqtt_user;
        doc["pass"] = mqtt_pass;
        doc["tcmd"] = mqtt_topic_cmd;
        doc["tstate"] = mqtt_topic_state;
        doc["mqtt_enabled"] = mqtt_enabled;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/save-settings", HTTP_POST, []() {
        if (!requireConfigAuth()) return;
        String s_ssid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : DEFAULT_SSID;
        String s_pass = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : DEFAULT_PASSWORD;
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;
        bool mqttEnabled = server.hasArg("mqtt_enabled");

        saveConfig(s_ssid, s_pass, srv, prt, usr, pwd, tCmd, tState, mqttEnabled);

        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP32...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });

    server.on("/boot-ap", HTTP_POST, []() {
        if (!requireConfigAuth()) return;
        server.send(200, "text/plain", "Switching to AP configuration mode...");
        delay(300);
     enterAPMode();
    });

    server.on("/factory-reset", HTTP_POST, []() {
        if (!requireConfigAuth()) return;
        preferences.begin("config", false);
        preferences.clear();
        preferences.end();
        stored_ssid = DEFAULT_SSID;
        stored_password = DEFAULT_PASSWORD;
        mqtt_server = DEFAULT_MQTT_SERVER;
        mqtt_port = DEFAULT_MQTT_PORT;
        mqtt_user = DEFAULT_MQTT_USER;
        mqtt_pass = DEFAULT_MQTT_PASS;
        mqtt_topic_cmd = DEFAULT_MQTT_TOPIC_CMD;
        mqtt_topic_state = DEFAULT_MQTT_TOPIC_STATE;
        mqtt_enabled = true;
        server.send(200, "text/plain", "Factory reset complete. Starting AP mode...");
        delay(300);
        enterAPMode();
    });

    setupOTARoutes();
    server.begin();

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
}

// --- Main Loop ---
void loop() {
    checkButton();

    if (apModeActive) {
        server.handleClient();
        return;
    }

    checkWiFiConnection();

    if (mqtt_enabled) {
        if (!mqttClient.connected()) {
            reconnectMQTT(); // Non-blocking gated retry check
        } else {
            mqttClient.loop();
        }
    } else if (mqttClient.connected()) {
        mqttClient.disconnect();
    }

    webSocket.loop();
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
