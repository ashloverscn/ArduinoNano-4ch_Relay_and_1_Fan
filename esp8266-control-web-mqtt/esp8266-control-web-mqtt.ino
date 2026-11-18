#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>

// --- ESP8266 Preferences Wrapper (Emulated via EEPROM) ---
class ESP8266Preferences {
public:
    void begin(const char* name, bool readOnly) {
        EEPROM.begin(1024);
    }
    String getString(const char* key, const char* defaultValue) {
        char buffer[64];
        int addr = getAddress(key);
        for (int i = 0; i < 64; i++) {
            buffer[i] = EEPROM.read(addr + i);
            if (buffer[i] == '\0') break;
            // Reject any non-printable ASCII or uninitialized 0xFF flash bytes
            if ((uint8_t)buffer[i] < 32 || (uint8_t)buffer[i] > 126) {
                return String(defaultValue);
            }
        }
        buffer[63] = '\0';
        String val = String(buffer);
        val.trim();
        if (val.length() == 0) return String(defaultValue);
        return val;
    }
    void putString(const char* key, String val) {
        int addr = getAddress(key);
        for (int i = 0; i < 64; i++) {
            if (i < (int)val.length()) EEPROM.write(addr + i, val[i]);
            else EEPROM.write(addr + i, 0);
        }
        EEPROM.commit();
    }
    int getInt(const char* key, int defaultValue) {
        int addr = getAddress(key);
        int val;
        EEPROM.get(addr, val);
        if (val <= 0 || val > 65535) return defaultValue;
        return val;
    }
    void putInt(const char* key, int val) {
        int addr = getAddress(key);
        EEPROM.put(addr, val);
        EEPROM.commit();
    }
    bool getBool(const char* key, bool defaultValue) {
        int addr = getAddress(key);
        uint8_t val = EEPROM.read(addr);
        if (val == 1) return true;
        if (val == 2) return false;
        return defaultValue;
    }
    void putBool(const char* key, bool val) {
        int addr = getAddress(key);
        EEPROM.write(addr, val ? 1 : 2);
        EEPROM.commit();
    }
    void clear() {
        for (int i = 0; i < 1024; i++) EEPROM.write(i, 0);
        EEPROM.commit();
    }
    void end() {}
private:
    int getAddress(const char* key) {
        if (strcmp(key, "ssid") == 0) return 0;
        if (strcmp(key, "password") == 0) return 64;
        if (strcmp(key, "mdns") == 0) return 128;
        if (strcmp(key, "server") == 0) return 192;
        if (strcmp(key, "port") == 0) return 256;
        if (strcmp(key, "user") == 0) return 264;
        if (strcmp(key, "pass") == 0) return 328;
        if (strcmp(key, "tcmd") == 0) return 392;
        if (strcmp(key, "tstate") == 0) return 456;
        if (strcmp(key, "enabled") == 0) return 520;
        if (strcmp(key, "wuser") == 0) return 528;
        if (strcmp(key, "wpass") == 0) return 592;
        return 660;
    }
};

ESP8266Preferences preferences;

// --- Default Fallback Credentials & Hostname ---
const char* DEFAULT_SSID = "ash_jio_2G";
const char* DEFAULT_PASSWORD = "ThundersnO0w@,,#";
const char* DEFAULT_MDNS_NAME = "homeautomationcontroller";

const char* DEFAULT_MQTT_SERVER = "i11b0026.ala.eu-central-1.emqxsl.com";
const int   DEFAULT_MQTT_PORT = 8883;
const char* DEFAULT_MQTT_USER = "admin";
const char* DEFAULT_MQTT_PASS = "admin1234S";
const char* DEFAULT_MQTT_TOPIC_CMD = "home/esp8266/command";
const char* DEFAULT_MQTT_TOPIC_STATE = "home/esp8266/state";
const bool  DEFAULT_MQTT_ENABLED = true;

const char* DEFAULT_WEB_USER = "admin";
const char* DEFAULT_WEB_PASS = "admin1234S";

// --- Runtime Configuration Variables ---
String stored_ssid;
String stored_password;
String stored_mdns;
String mqtt_server;
int mqtt_port;
String mqtt_user;
String mqtt_pass;
String mqtt_topic_cmd;
String mqtt_topic_state;
bool mqtt_enabled;
String stored_web_user;
String stored_web_pass;

// --- Modern Web Server, WebSocket & MQTT ---
ESP8266WebServer server(80);
WebSocketsServer webSocket(81);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// --- WebSocket Authentication State Tracking ---
bool wsAuthenticated[10] = {false};

// --- Serial Buffering State (50ms Silence Timeout) ---
String serialBuffer = "";
unsigned long lastCharTime = 0;
const unsigned long SERIAL_TIMEOUT = 50;

// --- Wi-Fi Reconnection & AP Fallback State ---
unsigned long lastWiFiCheckTime = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000; 
unsigned long wifiDisconnectedStartTime = 0;        
const unsigned long WIFI_AP_FALLBACK_TIMEOUT = 600000; // 10 minutes

// --- GPIO 0 Button Press States (3s AP Mode & 10s Factory Reset) ---
const int CONFIG_BUTTON_PIN = 0;
unsigned long buttonPressStartTime = 0;
bool buttonStateLast = HIGH;
const unsigned long LONG_PRESS_DURATION = 3000;       // 3 seconds for AP Mode
const unsigned long VERY_LONG_PRESS_DURATION = 10000; // 10 seconds for EEPROM Factory Reset
bool apTriggered = false;
bool factoryResetTriggered = false;
bool apModeActive = false;

// Forward declarations
void loadConfig();
void saveConfig(String ssid, String pass, String mdnsName, String srv, int prt, String usr, String pwd, String tCmd, String tState, bool mEnabled, String wUser, String wPass);
void logMessage(const String& msg);
void handleCommand(const String& cmd);
void processBufferedSerial(const String& data);
void reconnectMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void checkWiFiConnection();
void checkButton();
void enterAPMode();
void setupOTARoutes(bool requireAuth);

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
        <div class="card-title">ESP8266 Gateway Status</div>
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
            appendConsole("Connected. Authenticating WebSocket...");
            
            // Send automatic credentials handshake
            ws.send(JSON.stringify({
                auth: {
                    user: "{{WEB_USER}}",
                    pass: "{{WEB_PASS}}"
                }
            }));
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

    function stepDimmer(delta) {
        const slider = document.getElementById('dimmerSlider');
        let val = parseInt(slider.value) + delta;
        if (val < 0) val = 0;
        if (val > 9) val = 9;
        slider.value = val;
        sendCommand('DIMMER:' + val);
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

// --- Configuration Page HTML ---
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
        .section-header-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-top: 20px;
            margin-bottom: 10px;
        }
        .section-header-row h3 {
            margin: 0;
            color: #333;
        }
        .checkbox-container {
            display: flex;
            align-items: center;
            gap: 6px;
            font-weight: bold;
            font-size: 13px;
            color: #333;
            cursor: pointer;
        }
        .checkbox-container input {
            transform: scale(1.2);
            cursor: pointer;
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
        .btn-primary { background-color: #007bff; color: #fff; }
        .btn-primary:hover { background-color: #0056b3; }
        .btn-warning { background-color: #ffc107; color: #000; }
        .btn-warning:hover { background-color: #e0a800; }
        .btn-danger { background-color: #dc3545; color: #fff; }
        .btn-danger:hover { background-color: #c82333; }
        #otaStatus {
            margin-top: 10px;
            font-weight: bold;
            font-size: 13px;
            color: #007bff;
            text-align: center;
        }
    </style>
</head>
<body>

<div class="container">
    <a href="/" class="back-link">&larr; Back to Dashboard</a>
    <h2>Device Configuration</h2>
    
    <form action="/save-settings" method="POST">
        <h3>Wi-Fi & Network Settings</h3>
        <label>Wi-Fi SSID:</label>
        <input type="text" name="wifi_ssid" id="cfg_ssid">
        
        <label>Wi-Fi Password:</label>
        <input type="password" name="wifi_pass" id="cfg_pass_wifi">

        <label>Device Hostname (mDNS):</label>
        <input type="text" name="mdns_name" id="cfg_mdns">

        <hr>
        
        <div class="section-header-row">
            <h3>MQTT Settings</h3>
            <label class="checkbox-container">
                <input type="checkbox" name="mqtt_enabled" id="cfg_enabled"> Enable MQTT
            </label>
        </div>

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

        <hr>
        <h3>Web Login Security</h3>
        <label>Web Username:</label>
        <input type="text" name="web_user" id="cfg_wuser">

        <label>Web Password:</label>
        <input type="password" name="web_pass" id="cfg_wpass">
        
        <button type="submit">Save & Reboot</button>
    </form>

    <hr>
    <h3>Firmware Update (OTA)</h3>
    <form method="POST" action="/update" enctype="multipart/form-data" onsubmit="document.getElementById('otaStatus').innerText = 'Uploading & Flashing Firmware... Do not power off!';">
        <label>Select Compiled Firmware (.bin):</label>
        <input type="file" name="update" accept=".bin" required>
        <button type="submit" class="btn-primary">Upload & Flash Firmware</button>
        <div id="otaStatus"></div>
    </form>

    <hr>
    <h3>Device Actions</h3>
    <button type="button" class="btn-warning" onclick="if(confirm('Reboot into AP Mode?')) location.href='/reboot-ap'">Reboot to AP Mode</button>
    <button type="button" class="btn-danger" onclick="if(confirm('WARNING: This will erase all settings and factory reset! Continue?')) location.href='/factory-reset'">Factory Reset</button>
</div>

<script>
    fetch('/get-config')
        .then(res => res.json())
        .then(data => {
            document.getElementById('cfg_ssid').value = data.ssid || '';
            document.getElementById('cfg_pass_wifi').value = data.pass_wifi || '';
            document.getElementById('cfg_mdns').value = data.mdns || 'homeautomationcontroller';
            document.getElementById('cfg_enabled').checked = data.enabled === 1 || data.enabled === true;
            document.getElementById('cfg_server').value = data.server;
            document.getElementById('cfg_port').value = data.port;
            document.getElementById('cfg_user').value = data.user;
            document.getElementById('cfg_pass').value = data.pass;
            document.getElementById('cfg_tcmd').value = data.tcmd;
            document.getElementById('cfg_tstate').value = data.tstate;
            document.getElementById('cfg_wuser').value = data.wuser || 'admin';
            document.getElementById('cfg_wpass').value = data.wpass || 'admin1234S';
        });
</script>

</body>
</html>
)rawliteral";

// --- Load and Save Configurations via Preferences ---
void loadConfig() {
    preferences.begin("config", false);
    stored_ssid = preferences.getString("ssid", DEFAULT_SSID);
    stored_password = preferences.getString("password", DEFAULT_PASSWORD);
    stored_mdns = preferences.getString("mdns", DEFAULT_MDNS_NAME);
    mqtt_server = preferences.getString("server", DEFAULT_MQTT_SERVER);
    mqtt_port = preferences.getInt("port", DEFAULT_MQTT_PORT);
    mqtt_user = preferences.getString("user", DEFAULT_MQTT_USER);
    mqtt_pass = preferences.getString("pass", DEFAULT_MQTT_PASS);
    mqtt_topic_cmd = preferences.getString("tcmd", DEFAULT_MQTT_TOPIC_CMD);
    mqtt_topic_state = preferences.getString("tstate", DEFAULT_MQTT_TOPIC_STATE);
    mqtt_enabled = preferences.getBool("enabled", DEFAULT_MQTT_ENABLED);
    stored_web_user = preferences.getString("wuser", DEFAULT_WEB_USER);
    stored_web_pass = preferences.getString("wpass", DEFAULT_WEB_PASS);

    if (stored_mdns.length() == 0 || stored_mdns.length() > 32) {
        stored_mdns = DEFAULT_MDNS_NAME;
    }
    if (stored_web_user.length() == 0 || stored_web_user.length() > 32) {
        stored_web_user = DEFAULT_WEB_USER;
    }
    if (stored_web_pass.length() == 0 || stored_web_pass.length() > 32) {
        stored_web_pass = DEFAULT_WEB_PASS;
    }
    preferences.end();
}

void saveConfig(String ssid, String pass, String mdnsName, String srv, int prt, String usr, String pwd, String tCmd, String tState, bool mEnabled, String wUser, String wPass) {
    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", pass);
    preferences.putString("mdns", mdnsName);
    preferences.putString("server", srv);
    preferences.putInt("port", prt);
    preferences.putString("user", usr);
    preferences.putString("pass", pwd);
    preferences.putString("tcmd", tCmd);
    preferences.putString("tstate", tState);
    preferences.putBool("enabled", mEnabled);
    preferences.putString("wuser", wUser);
    preferences.putString("wpass", wPass);
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
        
        // 10-Second Very Long Press: Erase EEPROM & Boot into AP Mode
        if (heldTime >= VERY_LONG_PRESS_DURATION && !factoryResetTriggered) {
            factoryResetTriggered = true;
            logMessage(">> [FACTORY RESET] 10s Very Long Press detected! Erasing EEPROM storage & entering AP Mode...");
            
            preferences.begin("config", false);
            preferences.clear();
            preferences.end();
            
            delay(500);
            enterAPMode();
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

// --- Web OTA Firmware Update Handler ---
void setupOTARoutes(bool requireAuth) {
    server.on("/update", HTTP_POST, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            server.send(200, "text/html", "<h3>OTA Update Failed! Rebooting...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        } else {
            server.send(200, "text/html", "<h3>OTA Update Successful! Rebooting ESP8266...</h3><script>setTimeout(()=>{window.location='/'}, 6000);</script>");
        }
        delay(1000);
        ESP.restart();
    }, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return;
        }
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.setDebugOutput(true);
            WiFiUDP::stopAll();
            Serial.printf(">> [OTA] Update Start: %s\n", upload.filename.c_str());
            uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
            if (!Update.begin(maxSketchSpace)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf(">> [OTA] Update Success: %u bytes\nRebooting...\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
            Serial.setDebugOutput(false);
        }
        yield();
    });
}

// --- Enter Access Point Mode for Reconfiguration ---
void enterAPMode() {
    apModeActive = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    
    bool apStarted = WiFi.softAP("ESP8266-Setup-AP", "12345678");
    if (apStarted) {
        logMessage(">> AP Mode Started Successfully!");
        logMessage(">> SSID: ESP8266-Setup-AP | Password: 12345678");
        logMessage(">> Connect and open http://192.168.4.1");
    } else {
        logMessage(">> Failed to start AP Mode!");
    }

    // Initialize mDNS in AP Mode too
    if (MDNS.begin(stored_mdns.c_str())) {
        logMessage(">> mDNS responder started in AP mode: http://" + stored_mdns + ".local");
    }

    webSocket.close();
    server.stop();

    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    server.on("/config", HTTP_GET, []() {
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    server.on("/get-config", HTTP_GET, []() {
        JsonDocument doc;
        doc["ssid"] = stored_ssid;
        doc["pass_wifi"] = stored_password;
        doc["mdns"] = stored_mdns;
        doc["enabled"] = mqtt_enabled;
        doc["server"] = mqtt_server;
        doc["port"] = mqtt_port;
        doc["user"] = mqtt_user;
        doc["pass"] = mqtt_pass;
        doc["tcmd"] = mqtt_topic_cmd;
        doc["tstate"] = mqtt_topic_state;
        doc["wuser"] = stored_web_user;
        doc["wpass"] = stored_web_pass;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/save-settings", HTTP_POST, []() {
        String s_ssid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : DEFAULT_SSID;
        String s_pass = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : DEFAULT_PASSWORD;
        String s_mdns = server.hasArg("mdns_name") && server.arg("mdns_name") != "" ? server.arg("mdns_name") : DEFAULT_MDNS_NAME;
        bool mEnabled = server.hasArg("mqtt_enabled");
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;
        String wUser = server.hasArg("web_user") && server.arg("web_user") != "" ? server.arg("web_user") : DEFAULT_WEB_USER;
        String wPass = server.hasArg("web_pass") && server.arg("web_pass") != "" ? server.arg("web_pass") : DEFAULT_WEB_PASS;

        saveConfig(s_ssid, s_pass, s_mdns, srv, prt, usr, pwd, tCmd, tState, mEnabled, wUser, wPass);

        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP8266...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });

    setupOTARoutes(false);

    server.on("/reboot-ap", HTTP_GET, []() {
        server.send(200, "text/html", "<h3>Rebooting to AP Mode...</h3>");
        delay(1000);
        enterAPMode();
    });

    server.on("/factory-reset", HTTP_GET, []() {
        preferences.begin("config", false);
        preferences.clear();
        preferences.end();
        server.send(200, "text/html", "<h3>Factory Reset Complete! Entering AP Mode...</h3>");
        delay(1000);
        enterAPMode();
    });

    server.begin();
}

// --- WebSocket Event Callback (Secured) ---
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            if (num < 10) wsAuthenticated[num] = false;
            break;
        case WStype_CONNECTED:
            if (num < 10) wsAuthenticated[num] = false;
            break;
        case WStype_TEXT: {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload, length);
            if (error) break;

            // 1. Handle Authentication Packet
            if (doc.containsKey("auth")) {
                JsonObject auth = doc["auth"];
                String u = auth["user"] | "";
                String p = auth["pass"] | "";
                if (u == stored_web_user && p == stored_web_pass) {
                    if (num < 10) wsAuthenticated[num] = true;
                    JsonDocument resp;
                    resp["type"] = "console_log";
                    resp["message"] = ">> WebSocket authenticated successfully.";
                    String out;
                    serializeJson(resp, out);
                    webSocket.sendTXT(num, out);
                    
                    Serial.println("STATUS");
                } else {
                    JsonDocument resp;
                    resp["type"] = "console_log";
                    resp["message"] = ">> WebSocket authentication failed!";
                    String out;
                    serializeJson(resp, out);
                    webSocket.sendTXT(num, out);
                }
                break;
            }

            // 2. Reject commands if client is not authenticated
            if (num < 10 && !wsAuthenticated[num]) {
                JsonDocument resp;
                resp["type"] = "console_log";
                resp["message"] = ">> Unauthorized: Please authenticate first.";
                String out;
                serializeJson(resp, out);
                webSocket.sendTXT(num, out);
                break;
            }

            // 3. Process normal commands if authenticated
            if (doc.containsKey("command")) {
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

// --- Reconnect MQTT (5-minute delay on failure, respects enabled flag) ---
void reconnectMQTT() {
    if (mqtt_enabled && !apModeActive && !mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        static unsigned long lastMqttRetry = 0;
        const unsigned long MQTT_RETRY_INTERVAL = 300000; // 5 minutes in milliseconds
        
        if (millis() - lastMqttRetry >= MQTT_RETRY_INTERVAL || lastMqttRetry == 0) { 
            lastMqttRetry = millis();
            logMessage(">> Attempting MQTT connection...");
            String clientId = "ESP8266Gateway-" + String(ESP.getChipId(), HEX);
            if (mqttClient.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
                logMessage(">> MQTT Connected!");
                mqttClient.subscribe(mqtt_topic_cmd.c_str());
                lastMqttRetry = 0; // Reset retry tracking on successful connection
            } else {
                logMessage(">> MQTT Connection failed, rc=" + String(mqttClient.state()) + ". Retrying in 5 minutes...");
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

    // --- mDNS Start Initialization in Normal Mode ---
    if (WiFi.status() == WL_CONNECTED) {
        if (MDNS.begin(stored_mdns.c_str())) {
            Serial.println(">> mDNS responder started: http://" + stored_mdns + ".local");
        } else {
            Serial.println(">> Error setting up MDNS responder!");
        }
    }
    // ------------------------------------------------

    if (mqtt_enabled) {
        espClient.setInsecure();
        mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
        mqttClient.setCallback(callbackMQTT);
        mqttClient.setBufferSize(512);
    }

    server.on("/", HTTP_GET, []() {
        if (!server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        String html = FPSTR(HTML_TEMPLATE);
        html.replace("{{WEB_USER}}", stored_web_user);
        html.replace("{{WEB_PASS}}", stored_web_pass);
        server.send(200, "text/html", html);
    });

    server.on("/config", HTTP_GET, []() {
        if (!server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    server.on("/get-config", HTTP_GET, []() {
        if (!server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        JsonDocument doc;
        doc["ssid"] = stored_ssid;
        doc["pass_wifi"] = stored_password;
        doc["mdns"] = stored_mdns;
        doc["enabled"] = mqtt_enabled;
        doc["server"] = mqtt_server;
        doc["port"] = mqtt_port;
        doc["user"] = mqtt_user;
        doc["pass"] = mqtt_pass;
        doc["tcmd"] = mqtt_topic_cmd;
        doc["tstate"] = mqtt_topic_state;
        doc["wuser"] = stored_web_user;
        doc["wpass"] = stored_web_pass;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/save-settings", HTTP_POST, []() {
        if (!server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        String s_ssid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : DEFAULT_SSID;
        String s_pass = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : DEFAULT_PASSWORD;
        String s_mdns = server.hasArg("mdns_name") && server.arg("mdns_name") != "" ? server.arg("mdns_name") : DEFAULT_MDNS_NAME;
        bool mEnabled = server.hasArg("mqtt_enabled");
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;
        String wUser = server.hasArg("web_user") && server.arg("web_user") != "" ? server.arg("web_user") : DEFAULT_WEB_USER;
        String wPass = server.hasArg("web_pass") && server.arg("web_pass") != "" ? server.arg("web_pass") : DEFAULT_WEB_PASS;

        saveConfig(s_ssid, s_pass, s_mdns, srv, prt, usr, pwd, tCmd, tState, mEnabled, wUser, wPass);

        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP8266...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });

    setupOTARoutes(true);

    server.on("/reboot-ap", HTTP_GET, []() {
        if (!server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        server.send(200, "text/html", "<h3>Rebooting to AP Mode...</h3>");
        delay(1000);
        enterAPMode();
    });

    server.on("/factory-reset", HTTP_GET, []() {
        if (!server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        preferences.begin("config", false);
        preferences.clear();
        preferences.end();
        server.send(200, "text/html", "<h3>Factory Reset Complete! Entering AP Mode...</h3>");
        delay(1000);
        enterAPMode();
    });

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
            reconnectMQTT();
        }
        mqttClient.loop();
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
