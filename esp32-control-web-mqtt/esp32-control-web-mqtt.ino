#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <IRremote.hpp>
#include <AceButton.h>
#include <arduino-timer.h>

using namespace ace_button;

Preferences preferences;

// --- Default Fallback Credentials ---
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

// --- Pin Assignments ---
const int RelayPin1    = 23;      
const int RelayPin2    = 22;      
const int RelayPin3    = 21;      
const int RelayPin4    = 19;      
const int TriacPin     = 18;         
const int ZeroCrossPin = 34;    
const int IR_RECV_PIN  = 35;     
const int CONFIG_BUTTON_PIN = 0; // Boot button for manual AP mode trigger

// --- Wall Switch / Button Pins (AceButton) ---
#define SwitchPin1 13 // Relay 1 Toggle
#define SwitchPin2 12 // Relay 2 Toggle
#define SwitchPin3 14 // Relay 3 Toggle
#define SwitchPin4 27 // Relay 4 Toggle
#define SwitchPin5 32 // Dimmer Down / Fan Off
#define SwitchPin6 33 // Dimmer Up / Fan On

AceButton button1(SwitchPin1);
AceButton button2(SwitchPin2);
AceButton button3(SwitchPin3);
AceButton button4(SwitchPin4);
AceButton button5(SwitchPin5);
AceButton button6(SwitchPin6);

// --- IR Remote Hex Codes ---
#define IR_Button_1    0xF50A7F80
#define IR_Button_2    0xE41B7F80
#define IR_Button_3    0xE01F7F80
#define IR_Button_4    0xF30C7F80
#define IR_Button_5    0xF20D7F80
#define IR_Button_Up   0xF9067F80
#define IR_Button_Dn   0xFA057F80
#define IR_All_Off     0xE11E7F80
#define IR_All_On      0xED127F80

// --- Phase Control Timing Map (Microseconds from Zero-Cross) ---
const uint32_t spd[10] = {
  9500, 
  8500, 
  7500, 
  6500, 
  5500, 
  4500, 
  3500, 
  2500, 
  1500, 
  500   
};

#define DIMMER_MIN 0
#define DIMMER_MAX 9

// --- Controller State ---
bool relayStates[4] = {false, false, false, false};
uint8_t dimmerValue = 0; 
volatile bool triacState = false;

auto timer = timer_create_default();

// --- ESP32 Hardware Timer for Dimmer ---
hw_timer_t *dimmerTimer = NULL;
portMUX_TYPE dimmerMux = portMUX_INITIALIZER_UNLOCKED;

// --- Web Server & MQTT ---
WebServer server(80);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// --- Serial & Reconnection State ---
String serialBuffer = "";
unsigned long lastCharTime = 0;
const unsigned long SERIAL_TIMEOUT = 50;

unsigned long lastWiFiCheckTime = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000; 
unsigned long wifiDisconnectedStartTime = 0;         
const unsigned long WIFI_AP_FALLBACK_TIMEOUT = 600000; // 10 minutes
bool apModeActive = false;

// Forward Declarations
void loadConfig();
void saveConfig(String ssid, String pass, String srv, int prt, String usr, String pwd, String tCmd, String tState);
void broadcastState();
void logMessage(const String& msg);
void handleCommand(const String& cmd);
void setRelayState(int relay, bool turnOn);
void relayToggle(int relay);
void setDimmerLevel(uint8_t val);
void triacOn();
void triacOff();
void triacOnOff();
void dimm_Up();
void dimm_Dn();
void ir_remote();
void reconnectMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void checkWiFiConnection();
void checkButton();
void enterAPMode();
bool sendPeriodicStatus(void*);
void handleEvent(AceButton* button, uint8_t eventType, uint8_t buttonState);
String getStateJSON();

// --- Main Dashboard HTML Template with Web Console Log Stream ---
const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Home Automation</title>
    <style>
        body { font-family: Arial, sans-serif; background-color: #f4f6f8; margin: 0; padding: 20px; display: flex; justify-content: center; }
        .container { width: 100%; max-width: 650px; background: #ffffff; border-radius: 8px; padding: 20px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); position: relative; }
        .config-link { position: absolute; top: 20px; right: 20px; text-decoration: none; background-color: #6c757d; color: white; padding: 8px 12px; font-size: 13px; border-radius: 4px; font-weight: bold; }
        .config-link:hover { background-color: #5a6268; }
        h2 { text-align: center; margin-top: 0; color: #333; }
        .card { border: 1px solid #ddd; border-radius: 6px; padding: 15px; margin-bottom: 15px; background: #fafafa; }
        .card-title { font-weight: bold; margin-bottom: 10px; color: #555; font-size: 14px; text-transform: uppercase; }
        .row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
        button { padding: 10px 15px; font-size: 14px; border-radius: 4px; border: none; cursor: pointer; background-color: #007bff; color: white; transition: 0.2s; }
        button:hover { background-color: #0056b3; }
        .relay-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
        .relay-btn { height: 70px; font-weight: bold; background-color: #e0e0e0; color: #000; border-radius: 6px; }
        .relay-btn.on { background-color: #4CAF50; color: #fff; }
        #console { width: 100%; height: 180px; background-color: #1e1e1e; color: #00ff00; font-family: monospace; font-size: 12px; padding: 10px; box-sizing: border-box; overflow-y: scroll; border-radius: 4px; white-space: pre-wrap; }
        .flex-fill { flex: 1; }
        #connectionStatus { font-weight: bold; padding: 5px 10px; border-radius: 4px; display: inline-block; }
        .status-disconnected { background: #ffebee; color: #c62828; }
        .status-connected { background: #e8f5e9; color: #2e7d32; }
    </style>
</head>
<body>
<div class="container">
    <a href="/config" class="config-link">⚙️ Config</a>
    <h2>ESP32 Home Automation</h2>
    <div class="card">
        <div class="card-title">System Status</div>
        <div id="connectionStatus" class="status-disconnected">Status: Disconnected</div>
    </div>
    <div class="card">
        <div class="card-title">Relay Controls (4-Channel)</div>
        <div class="relay-grid">
            <button id="relay-1" class="relay-btn" onclick="toggleRelay(1)">Relay 1<br>OFF</button>
            <button id="relay-2" class="relay-btn" onclick="toggleRelay(2)">Relay 2<br>OFF</button>
            <button id="relay-3" class="relay-btn" onclick="toggleRelay(3)">Relay 3<br>OFF</button>
            <button id="relay-4" class="relay-btn" onclick="toggleRelay(4)">Relay 4<br>OFF</button>
        </div>
    </div>
    <div class="card">
        <div class="card-title">Fan Dimmer Control (0-9)</div>
        <div class="row">
            <button onclick="sendCommand('DIMMER 0')">MIN (0)</button>
            <button onclick="sendCommand('DIMM_DN')">Step -</button>
            <input type="range" id="dimmerSlider" class="flex-fill" min="0" max="9" value="0" onchange="onSliderChange(this.value)">
            <button onclick="sendCommand('DIMM_UP')">Step +</button>
            <button onclick="sendCommand('DIMMER 9')">MAX (9)</button>
        </div>
    </div>
    <div class="card">
        <div class="card-title">Global Commands</div>
        <div class="row">
            <button class="flex-fill" onclick="sendCommand('ALL_ON')">ALL ON</button>
            <button class="flex-fill" onclick="sendCommand('ALL_OFF')">ALL OFF</button>
            <button class="flex-fill" onclick="sendCommand('STATUS')">Get Status</button>
        </div>
    </div>
    <div class="card">
        <div class="card-title">Live Console / Feedback</div>
        <div id="console"></div>
    </div>
</div>
<script>
    let relayStates = [false, false, false, false];

    function pollState() {
        fetch('/status-json')
            .then(res => res.json())
            .then(data => {
                updateConnectionBanner(true);
                if (data.POWER1 !== undefined) {
                    updateRelayUI(1, data.POWER1 === 'ON');
                    updateRelayUI(2, data.POWER2 === 'ON');
                    updateRelayUI(3, data.POWER3 === 'ON');
                    updateRelayUI(4, data.POWER4 === 'ON');
                    if (data.Dimmer !== undefined) {
                        document.getElementById('dimmerSlider').value = data.Dimmer;
                    }
                }
            })
            .catch(err => {
                updateConnectionBanner(false);
            });
    }

    function pollLogs() {
        fetch('/logs')
            .then(res => res.text())
            .then(logText => {
                if (logText.trim().length > 0) {
                    const consoleEl = document.getElementById('console');
                    consoleEl.innerHTML += logText;
                    consoleEl.scrollTop = consoleEl.scrollHeight;
                }
            }).catch(e => {});
    }

    function updateConnectionBanner(connected) {
        const banner = document.getElementById('connectionStatus');
        banner.innerText = connected ? "Status: Connected" : "Status: Disconnected";
        banner.className = connected ? "status-connected" : "status-disconnected";
    }

    function toggleRelay(relayNum) { 
        sendCommand(`POWER${relayNum} ${relayStates[relayNum - 1] ? 'OFF' : 'ON'}`); 
    }

    function onSliderChange(val) { 
        sendCommand(`DIMMER ${val}`); 
    }

    function sendCommand(cmd) {
        fetch(`/cmd?action=${encodeURIComponent(cmd)}`)
            .then(res => res.json())
            .then(data => {
                if (data.POWER1 !== undefined) {
                    updateRelayUI(1, data.POWER1 === 'ON');
                    updateRelayUI(2, data.POWER2 === 'ON');
                    updateRelayUI(3, data.POWER3 === 'ON');
                    updateRelayUI(4, data.POWER4 === 'ON');
                    if (data.Dimmer !== undefined) {
                        document.getElementById('dimmerSlider').value = data.Dimmer;
                    }
                }
            })
            .catch(err => {});
    }

    function updateRelayUI(relayNum, isOn) {
        relayStates[relayNum - 1] = isOn;
        const btn = document.getElementById(`relay-${relayNum}`);
        if (btn) {
            btn.innerText = `Relay ${relayNum}\n${isOn ? 'ON' : 'OFF'}`;
            if (isOn) btn.classList.add('on'); else btn.classList.remove('on');
        }
    }

    setInterval(pollState, 3000);
    setInterval(pollLogs, 1000);
    window.onload = pollState;
</script>
</body>
</html>
)rawliteral";

// --- Configuration Portal HTML Template ---
const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Device Configuration</title>
    <style>
        body { font-family: Arial, sans-serif; background-color: #f4f6f8; margin: 0; padding: 20px; display: flex; justify-content: center; }
        .container { width: 100%; max-width: 500px; background: #ffffff; border-radius: 8px; padding: 20px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }
        h2 { text-align: center; margin-top: 0; color: #333; }
        .back-link { display: inline-block; margin-bottom: 15px; text-decoration: none; color: #007bff; font-weight: bold; font-size: 14px; }
        label { font-weight: bold; font-size: 13px; color: #555; display: block; margin-top: 10px; }
        input[type="text"], input[type="password"], input[type="number"] { padding: 8px; border: 1px solid #ccc; border-radius: 4px; width: 100%; box-sizing: border-box; margin-top: 5px; margin-bottom: 5px; }
        hr { border: 0; border-top: 1px solid #eee; margin: 20px 0; }
        button { width: 100%; padding: 12px; font-size: 15px; border-radius: 4px; border: none; cursor: pointer; background-color: #28a745; color: white; font-weight: bold; margin-top: 15px; }
        button:hover { background-color: #218838; }
    </style>
</head>
<body>
<div class="container">
    <a href="/" class="back-link">&larr; Back to Dashboard</a>
    <h2>Device Configuration</h2>
    <form action="/save-settings" method="POST">
        <h3>Wi-Fi Settings</h3>
        <label>Wi-Fi SSID:</label><input type="text" name="wifi_ssid" id="cfg_ssid">
        <label>Wi-Fi Password:</label><input type="password" name="wifi_pass" id="cfg_pass_wifi">
        <hr>
        <h3>MQTT Settings</h3>
        <label>MQTT Server:</label><input type="text" name="mqtt_server" id="cfg_server">
        <label>MQTT Port:</label><input type="number" name="mqtt_port" id="cfg_port">
        <label>MQTT Username:</label><input type="text" name="mqtt_user" id="cfg_user">
        <label>MQTT Password:</label><input type="password" name="mqtt_pass" id="cfg_pass">
        <label>Command Topic:</label><input type="text" name="mqtt_topic_cmd" id="cfg_tcmd">
        <label>State Topic:</label><input type="text" name="mqtt_topic_state" id="cfg_tstate">
        <button type="submit">Save & Reboot</button>
    </form>
</div>
<script>
    fetch('/get-config').then(res => res.json()).then(data => {
        document.getElementById('cfg_ssid').value = data.ssid || '';
        document.getElementById('cfg_pass_wifi').value = data.pass_wifi || '';
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

// --- Ring Buffer for Web Console Real-Time Log Streaming ---
String webLogBuffer = "";

void logMessage(const String& msg) {
    Serial.println(msg);
    webLogBuffer += msg + "\n";
    if (webLogBuffer.length() > 2048) {
        webLogBuffer = webLogBuffer.substring(webLogBuffer.length() - 1536);
    }
}

// --- Hardware Timer & Interrupt Handlers for AC Dimming ---
void IRAM_ATTR onDimmerTimer() {
  portENTER_CRITICAL_ISR(&dimmerMux);
  digitalWrite(TriacPin, HIGH);
  ets_delay_us(50);
  digitalWrite(TriacPin, LOW);
  timerStop(dimmerTimer);
  portEXIT_CRITICAL_ISR(&dimmerMux);
}

void IRAM_ATTR onZeroCross() {
  portENTER_CRITICAL_ISR(&dimmerMux);
  if (triacState && dimmerValue > 0) {
    timerRestart(dimmerTimer);
    timerAlarm(dimmerTimer, spd[dimmerValue], false, 0);
  }
  portEXIT_CRITICAL_ISR(&dimmerMux);
}

void applyDimmer() {
  portENTER_CRITICAL(&dimmerMux);
  if (!triacState || dimmerValue == 0) {
    digitalWrite(TriacPin, LOW);
  }
  portEXIT_CRITICAL(&dimmerMux);
}

void setDimmerLevel(uint8_t val) {
  if (val > DIMMER_MAX) val = DIMMER_MAX;
  dimmerValue = val;
  triacState = (dimmerValue > 0);
  applyDimmer();
  
  preferences.begin("config", false);
  preferences.putUChar("dimmer", dimmerValue);
  preferences.putUChar("triacState", triacState ? 1 : 0);
  preferences.end();
  
  logMessage(">> Dimmer set to: " + String(dimmerValue));
  broadcastState(); 
}

void triacOn() {
  triacState = true;
  if (dimmerValue == 0) dimmerValue = DIMMER_MAX;
  applyDimmer();
  preferences.begin("config", false);
  preferences.putUChar("dimmer", dimmerValue);
  preferences.putUChar("triacState", 1);
  preferences.end();
  logMessage(">> Dimmer set to: " + String(dimmerValue));
  broadcastState();
}

void triacOff() {
  triacState = false;
  dimmerValue = DIMMER_MIN;
  applyDimmer();
  preferences.begin("config", false);
  preferences.putUChar("dimmer", dimmerValue);
  preferences.putUChar("triacState", 0);
  preferences.end();
  logMessage(">> Dimmer set to: 0");
  broadcastState();
}

void triacOnOff() {
  if (triacState) triacOff();
  else triacOn();
}

void dimm_Up() {
  if (!triacState) { setDimmerLevel(1); return; }
  if (dimmerValue < DIMMER_MAX) { dimmerValue++; }
  setDimmerLevel(dimmerValue);
}

void dimm_Dn() {
  if (!triacState) return;
  if (dimmerValue > DIMMER_MIN) { dimmerValue--; }
  setDimmerLevel(dimmerValue);
}

// --- IR Remote Decoder ---
void ir_remote() {
  if (IrReceiver.decode()) {
    if (!(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
      uint32_t code = IrReceiver.decodedIRData.decodedRawData;
      logMessage(">> IR Code Received: 0x" + String(code, HEX));
      switch (code) {
        case IR_Button_1: relayToggle(1); break;
        case IR_Button_2: relayToggle(2); break;
        case IR_Button_3: relayToggle(3); break;
        case IR_Button_4: relayToggle(4); break;
        case IR_Button_5: triacOnOff(); break;
        case IR_Button_Up: dimm_Up(); break;
        case IR_Button_Dn: dimm_Dn(); break;
        case IR_All_Off: 
          for(int i=1; i<=4; i++) setRelayState(i, false);
          triacOff();
          break;
        case IR_All_On: 
          for(int i=1; i<=4; i++) setRelayState(i, true);
          setDimmerLevel(DIMMER_MAX);
          break;
        default: break;
      }
    }
    IrReceiver.resume();
  }
}

// --- Relay Control ---
void setRelayState(int relay, bool turnOn) {
    int pin = 0;
    String prefKey = "";
    switch (relay) {
        case 1: pin = RelayPin1; prefKey = "relay1"; break;
        case 2: pin = RelayPin2; prefKey = "relay2"; break;
        case 3: pin = RelayPin3; prefKey = "relay3"; break;
        case 4: pin = RelayPin4; prefKey = "relay4"; break;
        default: return;
    }
    relayStates[relay - 1] = turnOn;
    digitalWrite(pin, turnOn ? LOW : HIGH); // Low = Relay ON (active-low)

    preferences.begin("config", false);
    preferences.putUChar(prefKey.c_str(), turnOn ? 1 : 0);
    preferences.end();
    
    logMessage(">> Relay " + String(relay) + " -> " + (turnOn ? "ON" : "OFF"));
    broadcastState(); 
}

void relayToggle(int relay) {
    int pin = 0;
    switch (relay) {
        case 1: pin = RelayPin1; break;
        case 2: pin = RelayPin2; break;
        case 3: pin = RelayPin3; break;
        case 4: pin = RelayPin4; break;
        default: return;
    }
    bool currentState = (digitalRead(pin) == LOW);
    setRelayState(relay, !currentState);
}

// --- Preferences Storage Helpers ---
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

    uint8_t r1 = preferences.getUChar("relay1", 0);
    uint8_t r2 = preferences.getUChar("relay2", 0);
    uint8_t r3 = preferences.getUChar("relay3", 0);
    uint8_t r4 = preferences.getUChar("relay4", 0);
    dimmerValue = preferences.getUChar("dimmer", 0);
    uint8_t storedTriac = preferences.getUChar("triacState", 0);
    preferences.end();

    setRelayState(1, r1 == 1);
    setRelayState(2, r2 == 1);
    setRelayState(3, r3 == 1);
    setRelayState(4, r4 == 1);

    triacState = (storedTriac == 1 || dimmerValue > 0);
    applyDimmer();
}

void saveConfig(String ssid, String pass, String srv, int prt, String usr, String pwd, String tCmd, String tState) {
    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", pass);
    preferences.putString("server", srv);
    preferences.putInt("port", prt);
    preferences.putString("user", usr);
    preferences.putString("pass", pwd);
    preferences.putString("tcmd", tCmd);
    preferences.putString("tstate", tState);
    preferences.end();
}

// --- State JSON Helper (All 4 Relays Included) ---
String getStateJSON() {
    JsonDocument doc;
    doc["POWER1"] = relayStates[0] ? "ON" : "OFF";
    doc["POWER2"] = relayStates[1] ? "ON" : "OFF";
    doc["POWER3"] = relayStates[2] ? "ON" : "OFF";
    doc["POWER4"] = relayStates[3] ? "ON" : "OFF";
    doc["Dimmer"] = dimmerValue;
    String output;
    serializeJson(doc, output);
    return output;
}

// --- Wi-Fi & Connection Management ---
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
                logMessage(">> WiFi reconnection timed out. Entering AP Mode automatically...");
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

void checkButton() {
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
        delay(50);
        if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
            unsigned long pressTime = millis();
            while (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
                if (millis() - pressTime > 3000) {
                    logMessage(">> Long press on GPIO 0 detected! Entering AP Setup Mode...");
                    enterAPMode();
                    return;
                }
            }
        }
    }
}

void enterAPMode() {
    apModeActive = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    
    bool apStarted = WiFi.softAP("ESP32-Setup-AP", "12345678");
    if (apStarted) {
        logMessage(">> AP Mode Started! SSID: ESP32-Setup-AP | IP: http://192.168.4.1");
    }

    server.stop();

    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", CONFIG_PAGE); });
    server.on("/get-config", HTTP_GET, []() {
        JsonDocument doc;
        doc["ssid"] = stored_ssid; doc["pass_wifi"] = stored_password;
        doc["server"] = mqtt_server; doc["port"] = mqtt_port;
        doc["user"] = mqtt_user; doc["pass"] = mqtt_pass;
        doc["tcmd"] = mqtt_topic_cmd; doc["tstate"] = mqtt_topic_state;
        String response; serializeJson(doc, response);
        server.send(200, "application/json", response);
    });
    server.on("/save-settings", HTTP_POST, []() {
        String s_ssid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : DEFAULT_SSID;
        String s_pass = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : DEFAULT_PASSWORD;
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;

        saveConfig(s_ssid, s_pass, srv, prt, usr, pwd, tCmd, tState);
        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP32...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });
    server.begin();
}

// --- MQTT Callbacks & Reconnection ---
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    message.trim();
    
    logMessage(">> [MQTT Received]: " + message);
    if (String(topic) == mqtt_topic_cmd) {
        JsonDocument doc;
        if (!deserializeJson(doc, message) && doc.containsKey("command")) {
            handleCommand(doc["command"].as<String>());
        } else {
            handleCommand(message);
        }
    }
}

void reconnectMQTT() {
    if (!apModeActive && !mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        static unsigned long lastMqttRetry = 0;
        if (millis() - lastMqttRetry > 10000) { 
            lastMqttRetry = millis();
            String clientId = "ESP32Gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);
            if (mqttClient.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
                mqttClient.subscribe(mqtt_topic_cmd.c_str());
                logMessage(">> MQTT Connected & Subscribed!");
                broadcastState(); 
            }
        }
    }
}

// --- Unified Command Dispatcher ---
void handleCommand(const String& cmd) {
    String trimmedCmd = cmd;
    trimmedCmd.trim();

    for (int i = 1; i <= 4; i++) {
        if (trimmedCmd == "POWER" + String(i) + " ON") {
            setRelayState(i, true);
            return;
        } else if (trimmedCmd == "POWER" + String(i) + " OFF") {
            setRelayState(i, false);
            return;
        }
    }

    if (trimmedCmd.startsWith("DIMMER ")) {
        setDimmerLevel(trimmedCmd.substring(7).toInt());
        return;
    } else if (trimmedCmd == "DIMM_UP") {
        dimm_Up();
        return;
    } else if (trimmedCmd == "DIMM_DN") {
        dimm_Dn();
        return;
    } else if (trimmedCmd == "ALL_ON") {
        for (int i = 1; i <= 4; i++) relayStates[i - 1] = true;
        setDimmerLevel(DIMMER_MAX);
        return;
    } else if (trimmedCmd == "ALL_OFF") {
        for (int i = 1; i <= 4; i++) relayStates[i - 1] = false;
        setDimmerLevel(DIMMER_MIN);
        return;
    } else if (trimmedCmd == "STATUS") {
        logMessage(">> [STATUS REQUEST] Refreshing States...");
        broadcastState();
        logMessage("-> Relay 1: " + String(relayStates[0] ? "ON" : "OFF"));
        logMessage("-> Relay 2: " + String(relayStates[1] ? "ON" : "OFF"));
        logMessage("-> Relay 3: " + String(relayStates[2] ? "ON" : "OFF"));
        logMessage("-> Relay 4: " + String(relayStates[3] ? "ON" : "OFF"));
        logMessage("-> Fan Dimmer Level: " + String(dimmerValue) + " (Triac: " + String(triacState ? "ACTIVE" : "INACTIVE") + ")");
        logMessage("-> MQTT Status: " + String(mqttClient.connected() ? "CONNECTED" : "DISCONNECTED"));
        return;
    }

    broadcastState();
}

// --- State Broadcast ---
void broadcastState() {
    String out = getStateJSON();
    if (mqttClient.connected()) {
        mqttClient.publish(mqtt_topic_state.c_str(), out.c_str());
    }
    logMessage(">> STATE BROADCAST: " + out);
}

bool sendPeriodicStatus(void*) {
    broadcastState();
    return true;
}

// --- AceButton Event Handler ---
void handleEvent(AceButton* button, uint8_t eventType, uint8_t buttonState) {
  uint8_t pin = button->getPin();

  switch (eventType) {
    case AceButton::kEventClicked:
      if (pin == SwitchPin1) relayToggle(1);
      else if (pin == SwitchPin2) relayToggle(2);
      else if (pin == SwitchPin3) relayToggle(3);
      else if (pin == SwitchPin4) relayToggle(4);
      else if (pin == SwitchPin5) dimm_Dn();
      else if (pin == SwitchPin6) dimm_Up();
      break;

    case AceButton::kEventLongPressed:
      if (pin == SwitchPin5) setDimmerLevel(DIMMER_MIN);
      else if (pin == SwitchPin6) setDimmerLevel(DIMMER_MAX);
      break;
  }
}

// --- Setup ---
void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RelayPin1, OUTPUT);
    pinMode(RelayPin2, OUTPUT);
    pinMode(RelayPin3, OUTPUT);
    pinMode(RelayPin4, OUTPUT);
    pinMode(TriacPin, OUTPUT);
    digitalWrite(TriacPin, LOW);

    pinMode(ZeroCrossPin, INPUT_PULLUP);

    pinMode(SwitchPin1, INPUT_PULLUP);
    pinMode(SwitchPin2, INPUT_PULLUP);
    pinMode(SwitchPin3, INPUT_PULLUP);
    pinMode(SwitchPin4, INPUT_PULLUP);
    pinMode(SwitchPin5, INPUT_PULLUP);
    pinMode(SwitchPin6, INPUT_PULLUP);

    ButtonConfig* config1 = button1.getButtonConfig();
    config1->setEventHandler(handleEvent);
    config1->setFeature(ButtonConfig::kFeatureClick);

    ButtonConfig* config2 = button2.getButtonConfig();
    config2->setEventHandler(handleEvent);
    config2->setFeature(ButtonConfig::kFeatureClick);

    ButtonConfig* config3 = button3.getButtonConfig();
    config3->setEventHandler(handleEvent);
    config3->setFeature(ButtonConfig::kFeatureClick);

    ButtonConfig* config4 = button4.getButtonConfig();
    config4->setEventHandler(handleEvent);
    config4->setFeature(ButtonConfig::kFeatureClick);

    ButtonConfig* config5 = button5.getButtonConfig();
    config5->setEventHandler(handleEvent);
    config5->setFeature(ButtonConfig::kFeatureClick);
    config5->setFeature(ButtonConfig::kFeatureLongPress);
    config5->setLongPressDelay(1200);

    ButtonConfig* config6 = button6.getButtonConfig();
    config6->setEventHandler(handleEvent);
    config6->setFeature(ButtonConfig::kFeatureClick);
    config6->setFeature(ButtonConfig::kFeatureLongPress);
    config6->setLongPressDelay(1200);

    loadConfig();

    dimmerTimer = timerBegin(1000000);
    if (dimmerTimer) {
      timerAttachInterrupt(dimmerTimer, &onDimmerTimer);
      attachInterrupt(digitalPinToInterrupt(ZeroCrossPin), onZeroCross, CHANGE);
    }

    IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK);

    WiFi.begin(stored_ssid.c_str(), stored_password.c_str());
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        logMessage(">> WiFi Connected Successfully!");
        logMessage(">> Local IP Address: " + WiFi.localIP().toString());
    } else {
        logMessage(">> WiFi Connection Failed. Entering background retry mode...");
    }

    espClient.setInsecure();
    mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
    mqttClient.setCallback(callbackMQTT);
    mqttClient.setBufferSize(512);

    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", HTML_TEMPLATE); });
    server.on("/config", HTTP_GET, []() { server.send_P(200, "text/html", CONFIG_PAGE); });
    server.on("/status-json", HTTP_GET, []() { server.send(200, "application/json", getStateJSON()); });
    server.on("/logs", HTTP_GET, []() {
        server.send(200, "text/plain", webLogBuffer);
        webLogBuffer = ""; // clear buffer after retrieval
    });
    server.on("/cmd", HTTP_GET, []() {
        if (server.hasArg("action")) {
            handleCommand(server.arg("action"));
        }
        server.send(200, "application/json", getStateJSON());
    });
    server.on("/get-config", HTTP_GET, []() {
        JsonDocument doc;
        doc["ssid"] = stored_ssid; doc["pass_wifi"] = stored_password;
        doc["server"] = mqtt_server; doc["port"] = mqtt_port;
        doc["user"] = mqtt_user; doc["pass"] = mqtt_pass;
        doc["tcmd"] = mqtt_topic_cmd; doc["tstate"] = mqtt_topic_state;
        String response; serializeJson(doc, response);
        server.send(200, "application/json", response);
    });
    server.on("/save-settings", HTTP_POST, []() {
        String s_ssid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : DEFAULT_SSID;
        String s_pass = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : DEFAULT_PASSWORD;
        String srv = server.hasArg("mqtt_server") ? server.arg("mqtt_server") : DEFAULT_MQTT_SERVER;
        int prt = server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : DEFAULT_MQTT_PORT;
        String usr = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : DEFAULT_MQTT_USER;
        String pwd = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : DEFAULT_MQTT_PASS;
        String tCmd = server.hasArg("mqtt_topic_cmd") ? server.arg("mqtt_topic_cmd") : DEFAULT_MQTT_TOPIC_CMD;
        String tState = server.hasArg("mqtt_topic_state") ? server.arg("mqtt_topic_state") : DEFAULT_MQTT_TOPIC_STATE;

        saveConfig(s_ssid, s_pass, srv, prt, usr, pwd, tCmd, tState);
        server.send(200, "text/html", "<h3>Settings Saved! Restarting ESP32...</h3><script>setTimeout(()=>{window.location='/'}, 4000);</script>");
        delay(1000);
        ESP.restart();
    });
    server.begin();

    timer.every(10000, sendPeriodicStatus);
}

// --- Main Loop ---
void loop() {
    checkButton();

    if (apModeActive) {
        server.handleClient();
        return;
    }

    checkWiFiConnection();

    if (!mqttClient.connected()) {
        reconnectMQTT();
    }
    mqttClient.loop();

    server.handleClient();

    timer.tick();

    ir_remote();

    button1.check();
    button2.check();
    button3.check();
    button4.check();
    button5.check();
    button6.check();

    while (Serial.available()) {
        char c = Serial.read();
        lastCharTime = millis();
        if (serialBuffer.length() < 512) serialBuffer += c;
        else serialBuffer = "";
    }

    if (serialBuffer.length() > 0 && (millis() - lastCharTime >= SERIAL_TIMEOUT)) {
        serialBuffer.trim();
        handleCommand(serialBuffer);
        serialBuffer = "";
    }
}
