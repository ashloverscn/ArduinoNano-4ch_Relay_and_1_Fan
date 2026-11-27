#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <map>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include <Capabilities/RangeController.h>

// Single-file custom RangeController device (0-9) for SINRICPRO_5_1_0
class CustomRangeFan : public SinricProDevice,
                       public SINRICPRO_5_1_0::RangeController<CustomRangeFan> {
  friend class SINRICPRO_5_1_0::RangeController<CustomRangeFan>;
public:
  CustomRangeFan(const String &deviceId) : SinricProDevice(deviceId, "CustomRangeFan") {}
};

// Helper to invoke protected SinricProClass::add<T>() from a single .ino file
class SinricProHelper : public SINRICPRO_5_1_0::SinricProClass {
public:
  template <typename DeviceType>
  DeviceType& registerDevice(const String &deviceId) {
    return add<DeviceType>(deviceId);
  }
};

Preferences preferences;

// --- Default Fallback Credentials & Hostname ---
const char* DEFAULT_SSID = "ash_jio_2G";
const char* DEFAULT_PASSWORD = "ThundersnO0w@,,#";
const char* DEFAULT_MDNS_NAME = "homeautomationcontroller";

const char* DEFAULT_MQTT_SERVER = "i11b0026.ala.eu-central-1.emqxsl.com";
const int   DEFAULT_MQTT_PORT = 8883;
const char* DEFAULT_MQTT_USER = "admin";
const char* DEFAULT_MQTT_PASS = "admin1234S";
const char* DEFAULT_MQTT_TOPIC_CMD = "home/esp32/command";
const char* DEFAULT_MQTT_TOPIC_STATE = "home/esp32/state";
const bool  DEFAULT_MQTT_ENABLED = true;

const bool  DEFAULT_SINRIC_ENABLED = true;
const char* DEFAULT_SINRIC_APP_KEY = "167aca6d-6a18-4130-87a0-4eb35903e60b";
const char* DEFAULT_SINRIC_APP_SECRET = "fc5c8d46-ac77-4f95-86ea-a4720be13ab6-ed999d04-2a0b-4335-ba52-7112f10d2f4d";
const char* DEFAULT_SINRIC_LIGHT1_ID = "6b087b4b7tg4ce9fd668ec20";
const char* DEFAULT_SINRIC_LIGHT2_ID = "6b087b4b7d04ce9fd668ec20";
const char* DEFAULT_SINRIC_LIGHT3_ID = "6b087b4b7gy4ce9fd668ec20";
const char* DEFAULT_SINRIC_LIGHT4_ID = "6b087b5a814fdde88978ef40";
const char* DEFAULT_SINRIC_FAN_ID = "6b08870e7d04ce9fd669007d";
const char* SINRIC_FAN_RANGE_INSTANCE = "rangeInstance1";
const int   SINRIC_FAN_RANGE_MIN = 0;
const int   SINRIC_FAN_RANGE_MAX = 9;
const int   SINRIC_FAN_DEFAULT_STEP = 5;

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
bool sinric_enabled;
String sinric_app_key;
String sinric_app_secret;
String sinric_light_ids[4];
String sinric_fan_id;
String stored_web_user;
String stored_web_pass;

// --- Modern Web Server, WebSocket & MQTT ---
WebServer server(80);
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
const unsigned long VERY_LONG_PRESS_DURATION = 10000; // 10 seconds for NVS Factory Reset
bool apTriggered = false;
bool factoryResetTriggered = false;
bool apModeActive = false;
bool sinricInitialized = false;
bool mdnsStarted = false;
int lastFanSpeed = SINRIC_FAN_DEFAULT_STEP;
std::map<String, int> globalRangeValues = {{SINRIC_FAN_RANGE_INSTANCE, SINRIC_FAN_DEFAULT_STEP}};
CustomRangeFan* sinricFanDevice = nullptr;

// Forward declarations
void loadConfig();
void saveConfig(String ssid, String pass, String mdnsName, String srv, int prt, String usr, String pwd, String tCmd, String tState, bool mEnabled, bool sEnabled, String sAppKey, String sAppSecret, String sL1, String sL2, String sL3, String sL4, String sFan, String wUser, String wPass);
void logMessage(const String& msg);
void handleCommand(const String& cmd);
void processBufferedSerial(const String& data);
void reconnectMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void checkWiFiConnection();
void checkButton();
void enterAPMode();
void setupOTARoutes(bool requireAuth);
void setupSinricPro();
void registerConfigAndActionRoutes(bool inAPMode);
void startMDNS(bool inAPMode);

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

    <div class="card">
        <div class="card-title">ESP32 Gateway Status</div>
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
            <button onclick="sendCommand('DIMMER:0')">MIN (0)</button>
            <button onclick="stepDimmer(-1)">Step -</button>
            <input type="range" id="dimmerSlider" class="flex-fill" min="0" max="9" value="0" onchange="onSliderChange(this.value)">
            <button onclick="stepDimmer(1)">Step +</button>
            <button onclick="sendCommand('DIMMER:9')">MAX (9)</button>
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
    let ws;
    let relayStates = [false, false, false, false];
    let reconnectAttempts = 0;

    function initWebSocket() {
        ws = new WebSocket('ws://' + window.location.hostname + ':81/');

        ws.onopen = function(event) {
            reconnectAttempts = 0;
            updateConnectionBanner(true);
            appendConsole("Connected. Authenticating WebSocket...");

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

        <label>Device Hostname:</label>
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

        <div class="section-header-row">
            <h3>SinricPro Settings</h3>
            <label class="checkbox-container">
                <input type="checkbox" name="sinric_enabled" id="cfg_sinric_enabled"> Enable Sinric Mode
            </label>
        </div>

        <label>Sinric App Key:</label>
        <input type="text" name="sinric_app_key" id="cfg_sinric_app_key">

        <label>Sinric App Secret:</label>
        <input type="password" name="sinric_app_secret" id="cfg_sinric_app_secret">

        <label>Light 1 Device ID:</label>
        <input type="text" name="sinric_light1_id" id="cfg_sinric_l1">

        <label>Light 2 Device ID:</label>
        <input type="text" name="sinric_light2_id" id="cfg_sinric_l2">

        <label>Light 3 Device ID:</label>
        <input type="text" name="sinric_light3_id" id="cfg_sinric_l3">

        <label>Light 4 Device ID:</label>
        <input type="text" name="sinric_light4_id" id="cfg_sinric_l4">

        <label>Fan (0-9) Device ID:</label>
        <input type="text" name="sinric_fan_id" id="cfg_sinric_fan">

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
            document.getElementById('cfg_sinric_enabled').checked = data.sinric_enabled === 1 || data.sinric_enabled === true;
            document.getElementById('cfg_sinric_app_key').value = data.sinric_app_key || '';
            document.getElementById('cfg_sinric_app_secret').value = data.sinric_app_secret || '';
            document.getElementById('cfg_sinric_l1').value = data.sinric_light1_id || '';
            document.getElementById('cfg_sinric_l2').value = data.sinric_light2_id || '';
            document.getElementById('cfg_sinric_l3').value = data.sinric_light3_id || '';
            document.getElementById('cfg_sinric_l4').value = data.sinric_light4_id || '';
            document.getElementById('cfg_sinric_fan').value = data.sinric_fan_id || '';
            document.getElementById('cfg_wuser').value = data.wuser || 'admin';
            document.getElementById('cfg_wpass').value = data.wpass || 'admin1234S';
        });
</script>

</body>
</html>
)rawliteral";

// --- Load and Save Configurations via Preferences (NVS) ---
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
    sinric_enabled = preferences.getBool("s_enabled", DEFAULT_SINRIC_ENABLED);
    sinric_app_key = preferences.getString("s_app_key", DEFAULT_SINRIC_APP_KEY);
    sinric_app_secret = preferences.getString("s_app_sec", DEFAULT_SINRIC_APP_SECRET);
    sinric_light_ids[0] = preferences.getString("s_l1", DEFAULT_SINRIC_LIGHT1_ID);
    sinric_light_ids[1] = preferences.getString("s_l2", DEFAULT_SINRIC_LIGHT2_ID);
    sinric_light_ids[2] = preferences.getString("s_l3", DEFAULT_SINRIC_LIGHT3_ID);
    sinric_light_ids[3] = preferences.getString("s_l4", DEFAULT_SINRIC_LIGHT4_ID);
    sinric_fan_id = preferences.getString("s_fan", DEFAULT_SINRIC_FAN_ID);
    stored_web_user = preferences.getString("wuser", DEFAULT_WEB_USER);
    stored_web_pass = preferences.getString("wpass", DEFAULT_WEB_PASS);

    stored_mdns.trim();
    stored_mdns.toLowerCase();
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

void saveConfig(String ssid, String pass, String mdnsName, String srv, int prt, String usr, String pwd, String tCmd, String tState, bool mEnabled, bool sEnabled, String sAppKey, String sAppSecret, String sL1, String sL2, String sL3, String sL4, String sFan, String wUser, String wPass) {
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
    preferences.putBool("s_enabled", sEnabled);
    preferences.putString("s_app_key", sAppKey);
    preferences.putString("s_app_sec", sAppSecret);
    preferences.putString("s_l1", sL1);
    preferences.putString("s_l2", sL2);
    preferences.putString("s_l3", sL3);
    preferences.putString("s_l4", sL4);
    preferences.putString("s_fan", sFan);
    preferences.putString("wuser", wUser);
    preferences.putString("wpass", wPass);
    preferences.end();
}

void performFactoryResetNVS() {
    preferences.begin("config", false);
    preferences.clear();
    preferences.end();
    loadConfig();
}

// --- Start / Advertise Hostname via mDNS ---
void startMDNS(bool inAPMode) {
    if (inAPMode) {
        WiFi.softAPsetHostname(stored_mdns.c_str());
    } else {
        WiFi.setHostname(stored_mdns.c_str());
    }

    MDNS.end();
    mdnsStarted = false;

    if (MDNS.begin(stored_mdns.c_str())) {
        MDNS.addService("http", "tcp", 80);
        if (!inAPMode) {
            MDNS.addService("ws", "tcp", 81);
        }
        mdnsStarted = true;
        if (inAPMode) {
            logMessage(">> mDNS responder started in AP mode: http://" + stored_mdns + ".local");
        } else {
            logMessage(">> mDNS responder started: http://" + stored_mdns + ".local");
        }
    } else {
        if (inAPMode) {
            logMessage(">> Error setting up MDNS responder in AP mode!");
        } else {
            Serial.println(">> Error setting up MDNS responder!");
        }
    }
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

// --- SinricPro Callbacks & Setup ---
bool onSinricLightPowerState(const String &deviceId, bool &state) {
    for (int i = 0; i < 4; i++) {
        if (sinric_light_ids[i].length() > 0 && deviceId == sinric_light_ids[i]) {
            String cmd = "POWER" + String(i + 1) + (state ? " ON" : " OFF");
            handleCommand(cmd);
            SinricProSwitch &lightDev = SinricPro[sinric_light_ids[i]];
            lightDev.sendPowerStateEvent(state);
            return true;
        }
    }
    return false;
}

void applyFanStep(const String &deviceId, const String &instance, int rawValue) {
    int fanStep = constrain(rawValue, SINRIC_FAN_RANGE_MIN, SINRIC_FAN_RANGE_MAX);
    globalRangeValues[instance] = fanStep;
    if (fanStep > 0) {
        lastFanSpeed = fanStep;
    }
    logMessage("[Device: " + deviceId + "]: Value for \"" + instance + "\" set to " + String(fanStep));
    handleCommand("DIMMER:" + String(fanStep));

    if (sinricFanDevice != nullptr) {
        sinricFanDevice->sendRangeValueEvent(instance, fanStep);
    }
}

bool onSinricFanRangeValue(const String &deviceId, const String &instance, int &rangeValue) {
    if (sinric_fan_id.length() == 0 || deviceId != sinric_fan_id) return false;
    if (instance != SINRIC_FAN_RANGE_INSTANCE) return false;

    applyFanStep(deviceId, instance, rangeValue);
    rangeValue = globalRangeValues[instance];
    return true;
}

bool onSinricFanAdjustRangeValue(const String &deviceId, const String &instance, int &valueDelta) {
    if (sinric_fan_id.length() == 0 || deviceId != sinric_fan_id) return false;
    if (instance != SINRIC_FAN_RANGE_INSTANCE) return false;

    int baseVal = globalRangeValues.count(instance) ? globalRangeValues[instance] : SINRIC_FAN_DEFAULT_STEP;
    int nextVal = baseVal + valueDelta;
    applyFanStep(deviceId, instance, nextVal);
    valueDelta = globalRangeValues[instance];
    return true;
}

void setupSinricPro() {
    if (!sinric_enabled || sinricInitialized) return;
    if (sinric_app_key.length() == 0 || sinric_app_secret.length() == 0) {
        logMessage(">> Sinric Mode enabled, but App Key or App Secret is empty.");
        return;
    }

    for (int i = 0; i < 4; i++) {
        if (sinric_light_ids[i].length() > 0) {
            SinricProSwitch &lightDev = SinricPro[sinric_light_ids[i]];
            lightDev.onPowerState(onSinricLightPowerState);
        }
    }

    if (sinric_fan_id.length() > 0) {
        sinricFanDevice = &reinterpret_cast<SinricProHelper&>(SinricPro).registerDevice<CustomRangeFan>(sinric_fan_id);
        sinricFanDevice->onRangeValue(SINRIC_FAN_RANGE_INSTANCE, onSinricFanRangeValue);
        sinricFanDevice->onAdjustRangeValue(SINRIC_FAN_RANGE_INSTANCE, onSinricFanAdjustRangeValue);
    }

    SinricPro.onConnected([]() {
        logMessage(">> SinricPro Connected!");
    });
    SinricPro.onDisconnected([]() {
        logMessage(">> SinricPro Disconnected!");
    });

    SinricPro.begin(sinric_app_key, sinric_app_secret);
    sinricInitialized = true;
    logMessage(">> SinricPro initialized.");
}

// --- Continuous Non-Blocking Wi-Fi Reconnection & 10-Min AP Fallback ---
void checkWiFiConnection() {
    if (!apModeActive) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiDisconnectedStartTime = 0;
            if (!mdnsStarted) {
                startMDNS(false);
            }
            if (sinric_enabled && !sinricInitialized) {
                setupSinricPro();
            }
        } else {
            mdnsStarted = false;
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
                WiFi.setHostname(stored_mdns.c_str());
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

        if (heldTime >= VERY_LONG_PRESS_DURATION && !factoryResetTriggered) {
            factoryResetTriggered = true;
            logMessage(">> [FACTORY RESET] 10s Very Long Press detected! Erasing NVS storage & entering AP Mode...");
            performFactoryResetNVS();
            delay(500);
            enterAPMode();
        }
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

// --- ESP32 Web OTA Firmware Update Handler ---
void setupOTARoutes(bool requireAuth) {
    server.on("/update", HTTP_POST, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            server.send(200, "text/html", "<h3>OTA Update Failed! Rebooting...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        } else {
            server.send(200, "text/html", "<h3>OTA Update Successful! Rebooting ESP32...</h3><script>setTimeout(()=>{window.location='/'}, 6000);</script>");
        }
        delay(1000);
        ESP.restart();
    }, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return;
        }
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf(">> [OTA] Update Start: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
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
        }
    });
}

// --- Shared Config & Action Routes ---
void registerConfigAndActionRoutes(bool inAPMode) {
    bool requireAuth = !inAPMode;

    server.on("/config", HTTP_GET, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    server.on("/get-config", HTTP_GET, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
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
        doc["sinric_enabled"] = sinric_enabled;
        doc["sinric_app_key"] = sinric_app_key;
        doc["sinric_app_secret"] = sinric_app_secret;
        doc["sinric_light1_id"] = sinric_light_ids[0];
        doc["sinric_light2_id"] = sinric_light_ids[1];
        doc["sinric_light3_id"] = sinric_light_ids[2];
        doc["sinric_light4_id"] = sinric_light_ids[3];
        doc["sinric_fan_id"] = sinric_fan_id;
        doc["wuser"] = stored_web_user;
        doc["wpass"] = stored_web_pass;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/save-settings", HTTP_POST, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        String s_ssid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : DEFAULT_SSID;
        String s_pass = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : DEFAULT_PASSWORD;
        String s_mdns = server.hasArg("mdns_name") && server.arg("mdns_name") != "" ? server.arg("mdns_name") : DEFAULT_MDNS_NAME;
        s_mdns.trim();
        s_mdns.toLowerCase();
        if (s_mdns.length() == 0 || s_mdns.length() > 32) {
            s_mdns = DEFAULT_MDNS_NAME;
        }
        bool mEnabled = server.hasArg("mqtt_enabled");
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;
        bool sEnabled = server.hasArg("sinric_enabled");
        String sAppKey = server.hasArg("sinric_app_key") ? server.arg("sinric_app_key") : DEFAULT_SINRIC_APP_KEY;
        String sAppSecret = server.hasArg("sinric_app_secret") ? server.arg("sinric_app_secret") : DEFAULT_SINRIC_APP_SECRET;
        String sL1 = server.hasArg("sinric_light1_id") ? server.arg("sinric_light1_id") : DEFAULT_SINRIC_LIGHT1_ID;
        String sL2 = server.hasArg("sinric_light2_id") ? server.arg("sinric_light2_id") : DEFAULT_SINRIC_LIGHT2_ID;
        String sL3 = server.hasArg("sinric_light3_id") ? server.arg("sinric_light3_id") : DEFAULT_SINRIC_LIGHT3_ID;
        String sL4 = server.hasArg("sinric_light4_id") ? server.arg("sinric_light4_id") : DEFAULT_SINRIC_LIGHT4_ID;
        String sFan = server.hasArg("sinric_fan_id") ? server.arg("sinric_fan_id") : DEFAULT_SINRIC_FAN_ID;
        String wUser = server.hasArg("web_user") && server.arg("web_user") != "" ? server.arg("web_user") : DEFAULT_WEB_USER;
        String wPass = server.hasArg("web_pass") && server.arg("web_pass") != "" ? server.arg("web_pass") : DEFAULT_WEB_PASS;

        saveConfig(s_ssid, s_pass, s_mdns, srv, prt, usr, pwd, tCmd, tState, mEnabled, sEnabled, sAppKey, sAppSecret, sL1, sL2, sL3, sL4, sFan, wUser, wPass);

        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP32...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });

    setupOTARoutes(requireAuth);

    server.on("/reboot-ap", HTTP_GET, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        server.send(200, "text/html", "<h3>Rebooting to AP Mode...</h3>");
        delay(1000);
        enterAPMode();
    });

    server.on("/factory-reset", HTTP_GET, [requireAuth]() {
        if (requireAuth && !server.authenticate(stored_web_user.c_str(), stored_web_pass.c_str())) {
            return server.requestAuthentication();
        }
        performFactoryResetNVS();
        server.send(200, "text/html", "<h3>Factory Reset Complete! Entering AP Mode...</h3>");
        delay(1000);
        enterAPMode();
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

    startMDNS(true);

    webSocket.close();
    server.stop();

    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html", CONFIG_PAGE);
    });

    registerConfigAndActionRoutes(true);

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

            if (num < 10 && !wsAuthenticated[num]) {
                JsonDocument resp;
                resp["type"] = "console_log";
                resp["message"] = ">> Unauthorized: Please authenticate first.";
                String out;
                serializeJson(resp, out);
                webSocket.sendTXT(num, out);
                break;
            }

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

    logMessage(">> [MQTT Received on " + String(topic) + "]: " + message);

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
            String clientId = "ESP32Gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);
            if (mqttClient.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
                logMessage(">> MQTT Connected!");
                mqttClient.subscribe(mqtt_topic_cmd.c_str());
                lastMqttRetry = 0;
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

    WiFi.setHostname(stored_mdns.c_str());
    WiFi.begin(stored_ssid.c_str(), stored_password.c_str());
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        if (millis() - startAttempt > 15000) {
            break;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        startMDNS(false);
        setupSinricPro();
    }

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

    registerConfigAndActionRoutes(false);

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

    if (sinric_enabled && sinricInitialized && WiFi.status() == WL_CONNECTED) {
        SinricPro.handle();
    }

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