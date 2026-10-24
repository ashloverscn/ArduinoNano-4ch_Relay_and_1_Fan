#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>

const char* ssid = "ash_jio_2G";
const char* password = "ThundersnO0w@,,#";

ESP8266WebServer server(80);

const uint8_t relayPins[4] = {5, 4, 14, 12};
const uint8_t dimmerPin = 13;

const bool RELAY_ACTIVE_LOW = true;

bool relayStates[4] = {false, false, false, false};
uint8_t dimmerValue = 0;

String serialBuffer;

const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP8266 Control Panel</title>

<style>
* {
  box-sizing: border-box;
}

body {
  margin: 0;
  padding: 20px;
  background: #101522;
  color: #f3f5fa;
  font-family: Arial, sans-serif;
}

main {
  max-width: 680px;
  margin: auto;
}

h1 {
  font-size: 26px;
  margin-bottom: 6px;
}

.sub {
  color: #9ca9bf;
  margin-bottom: 24px;
}

.card {
  background: #1b2333;
  border: 1px solid #303b50;
  border-radius: 16px;
  padding: 20px;
  margin: 14px 0;
}

.top {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 10px;
}

.relay {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 15px 0;
  border-bottom: 1px solid #303b50;
}

.relay:last-child {
  border-bottom: 0;
}

.name {
  font-weight: bold;
  font-size: 16px;
}

.status {
  font-size: 12px;
  color: #9ca9bf;
  margin-top: 5px;
}

button {
  border: 0;
  border-radius: 10px;
  padding: 12px 16px;
  font-size: 14px;
  font-weight: bold;
  cursor: pointer;
}

button:disabled {
  opacity: 0.5;
}

.on {
  background: #24c78b;
  color: #08271c;
}

.off {
  background: #37445b;
  color: white;
}

.actions {
  display: flex;
  gap: 10px;
  margin-top: 16px;
  flex-wrap: wrap;
}

input[type=range] {
  width: 100%;
  margin: 25px 0 15px;
  accent-color: #46b4ff;
}

.level {
  font-size: 38px;
  font-weight: bold;
}

.small {
  font-size: 12px;
  color: #9ca9bf;
}

.dot {
  display: inline-block;
  width: 9px;
  height: 9px;
  border-radius: 50%;
  background: #24c78b;
  margin-right: 7px;
}

#connection {
  color: #9ca9bf;
}

.scale {
  display: flex;
  justify-content: space-between;
  color: #9ca9bf;
  font-size: 12px;
}

@media (max-width: 420px) {
  body {
    padding: 12px;
  }

  .card {
    padding: 15px;
  }

  button {
    padding: 11px 12px;
  }
}
</style>
</head>

<body>
<main>
  <h1>ESP8266 Control Panel</h1>
  <div class="sub">Four relays &middot; Nine-step dimmer</div>

  <div class="card">
    <div class="top">
      <b>Relay Controls</b>
      <span class="small">
        <span class="dot"></span>
        <span id="connection">Connecting...</span>
      </span>
    </div>

    <div id="relays"></div>

    <div class="actions">
      <button class="on" onclick="allRelays(true)">ALL ON</button>
      <button class="off" onclick="allRelays(false)">ALL OFF</button>
    </div>
  </div>

  <div class="card">
    <div class="top">
      <b>Dimmer Control</b>
      <div class="level">
        <span id="level">0</span>
        <span style="font-size:16px">/ 9</span>
      </div>
    </div>

    <input
      id="dimmer"
      type="range"
      min="0"
      max="9"
      step="1"
      value="0"
      oninput="showLevel(this.value)"
      onchange="setDimmer(this.value)"
    >

    <div class="scale">
      <span>0 (OFF)</span>
      <span>9 (ON)</span>
    </div>

    <div class="actions">
      <button class="off" onclick="setDimmer(0)">DIMMER OFF</button>
      <button class="on" onclick="setDimmer(9)">MAXIMUM</button>
    </div>
  </div>

  <div class="small">
    The control panel updates device state automatically.
  </div>
</main>

<script>
let current = {
  POWER1: "OFF",
  POWER2: "OFF",
  POWER3: "OFF",
  POWER4: "OFF",
  Dimmer: 0
};

function showLevel(value) {
  document.getElementById("level").textContent = value;
}

function render(state) {
  current = {...current, ...state};

  document.getElementById("connection").textContent = "Online";

  document.getElementById("relays").innerHTML =
    [1, 2, 3, 4].map(i => {
      const isOn =
        String(current["POWER" + i]).toUpperCase() === "ON";

      return `
        <div class="relay">
          <div>
            <div class="name">Relay ${i}</div>
            <div class="status">${isOn ? "ON" : "OFF"}</div>
          </div>
          <button
            class="${isOn ? "on" : "off"}"
            onclick="setRelay(${i}, ${!isOn})">
            ${isOn ? "TURN OFF" : "TURN ON"}
          </button>
        </div>`;
    }).join("");

  const d = Math.max(
    0,
    Math.min(9, Number(current.Dimmer) || 0)
  );

  document.getElementById("level").textContent = d;
  document.getElementById("dimmer").value = d;
}

async function request(url) {
  const response = await fetch(url, {
    cache: "no-store"
  });

  if (!response.ok) {
    throw new Error("Request failed");
  }

  return await response.json();
}

async function refresh() {
  try {
    render(await request("/api/state"));
  } catch (error) {
    document.getElementById("connection").textContent =
      "Offline";
  }
}

async function setRelay(number, on) {
  try {
    render(await request(
      "/api/relay?n=" + number +
      "&state=" + (on ? "ON" : "OFF")
    ));
  } catch (error) {
    document.getElementById("connection").textContent =
      "Error";
  }
}

async function setDimmer(value) {
  value = Math.max(0, Math.min(9, Number(value)));

  showLevel(value);
  document.getElementById("dimmer").value = value;

  try {
    render(await request("/api/dimmer?value=" + value));
  } catch (error) {
    document.getElementById("connection").textContent =
      "Error";
  }
}

async function allRelays(on) {
  for (let i = 1; i <= 4; i++) {
    await setRelay(i, on);
  }
}

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)rawliteral";

void writeRelay(uint8_t index, bool on) {
  relayStates[index] = on;

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(relayPins[index], on ? LOW : HIGH);
  } else {
    digitalWrite(relayPins[index], on ? HIGH : LOW);
  }
}

void writeDimmer(uint8_t value) {
  dimmerValue = constrain(value, 0, 9);

  if (dimmerValue == 0) {
    analogWrite(dimmerPin, 0);
  } else if (dimmerValue == 9) {
    analogWrite(dimmerPin, 1023);
  } else {
    analogWrite(dimmerPin, map(dimmerValue, 0, 9, 0, 1023));
  }
}

void printState() {
  StaticJsonDocument<192> doc;

  for (uint8_t i = 0; i < 4; i++) {
    char key[8];
    snprintf(key, sizeof(key), "POWER%u", i + 1);
    doc[key] = relayStates[i] ? "ON" : "OFF";
  }

  doc["Dimmer"] = dimmerValue;

  serializeJson(doc, Serial);
  Serial.println();
}

void handleJsonCommand(JsonDocument& doc) {
  for (uint8_t i = 0; i < 4; i++) {
    char key[8];
    snprintf(key, sizeof(key), "POWER%u", i + 1);

    if (doc[key].is<const char*>()) {
      String value = doc[key].as<String>();
      value.toUpperCase();

      if (value == "ON" || value == "OFF") {
        writeRelay(i, value == "ON");

        StaticJsonDocument<48> ack;
        ack[key] = value;

        serializeJson(ack, Serial);
        Serial.println();
      }
    }
  }

  if (doc.containsKey("Dimmer")) {
    int value = constrain(doc["Dimmer"].as<int>(), 0, 9);
    writeDimmer(value);

    StaticJsonDocument<32> ack;
    ack["Dimmer"] = dimmerValue;

    serializeJson(ack, Serial);
    Serial.println();
  }

  printState();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, serialBuffer);

        if (!error && doc.is<JsonObject>()) {
          handleJsonCommand(doc);
        }

        serialBuffer = "";
      }
    } else if (serialBuffer.length() < 250) {
      serialBuffer += c;
    } else {
      serialBuffer = "";
    }
  }
}

void sendHttpState() {
  StaticJsonDocument<192> doc;

  for (uint8_t i = 0; i < 4; i++) {
    char key[8];
    snprintf(key, sizeof(key), "POWER%u", i + 1);
    doc[key] = relayStates[i] ? "ON" : "OFF";
  }

  doc["Dimmer"] = dimmerValue;

  String response;
  serializeJson(doc, response);

  server.send(200, "application/json", response);
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);

  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(relayPins[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(relayPins[i], OUTPUT);
    writeRelay(i, false);
  }

  pinMode(dimmerPin, OUTPUT);

  analogWriteRange(1023);
  analogWriteFreq(1000);

  writeDimmer(0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", webpage);
  });

  server.on("/api/state", HTTP_GET, []() {
    sendHttpState();
  });

  server.on("/api/relay", HTTP_GET, []() {
    if (!server.hasArg("n") || !server.hasArg("state")) {
      server.send(400, "application/json", "{\"error\":\"Missing n or state\"}");
      return;
    }

    int n = server.arg("n").toInt();
    String value = server.arg("state");
    value.toUpperCase();

    if (n < 1 || n > 4 || (value != "ON" && value != "OFF")) {
      server.send(400, "application/json", "{\"error\":\"Invalid relay command\"}");
      return;
    }

    writeRelay(n - 1, value == "ON");

    // Send concise ACK packet
    StaticJsonDocument<48> ack;
    char key[8];
    snprintf(key, sizeof(key), "POWER%u", n);
    ack[key] = value;
    
    serializeJson(ack, Serial);
    Serial.println();
    Serial.flush();

    sendHttpState();
  });

  server.on("/api/dimmer", HTTP_GET, []() {
    if (!server.hasArg("value")) {
      server.send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }

    String input = server.arg("value");
    for (uint16_t i = 0; i < input.length(); i++) {
      if (!isDigit(input[i])) {
        server.send(400, "application/json", "{\"error\":\"Invalid dimmer value\"}");
        return;
      }
    }

    int value = input.toInt();
    if (value < 0 || value > 9) {
      server.send(400, "application/json", "{\"error\":\"Dimmer must be 0-9\"}");
      return;
    }

    writeDimmer(value);

    // Send concise ACK packet
    StaticJsonDocument<32> ack;
    ack["Dimmer"] = dimmerValue;
    
    serializeJson(ack, Serial);
    Serial.println();
    Serial.flush();

    sendHttpState();
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  printState();
}

void loop() {
  server.handleClient();
  handleSerial();
}