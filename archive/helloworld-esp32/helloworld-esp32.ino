#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Update.h>

// --- Wi-Fi & Credentials ---
const char* DEFAULT_SSID = "ash_jio_2G";
const char* DEFAULT_PASSWORD = "ThundersnO0w@,,#";

const char* DEFAULT_WEB_USER = "admin";
const char* DEFAULT_WEB_PASS = "admin1234S";

// --- Hardcoded Firmware Text to Display on Dashboard ---
const String FIRMWARE_MESSAGE = "THIS IS ESP32 SYS Firmware v1.0!";

WebServer server(80);

// --- Simple EEPROM Storage for Config ---
void saveDefaultConfig() {
    EEPROM.begin(512);
    // Write default SSID & Password if empty
    for (int i = 0; i < 32; i++) {
        EEPROM.write(i, i < strlen(DEFAULT_SSID) ? DEFAULT_SSID[i] : 0);
        EEPROM.write(64 + i, i < strlen(DEFAULT_PASSWORD) ? DEFAULT_PASSWORD[i] : 0);
    }
    EEPROM.commit();
    EEPROM.end();
}

// --- Main Dashboard HTML ---
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Hello World Dashboard</title>
    <style>
        body { font-family: Arial, sans-serif; background: #f4f6f8; text-align: center; padding: 50px; }
        .card { background: white; max-width: 500px; margin: 0 auto; padding: 30px; border-radius: 8px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }
        h1 { color: #007bff; }
        .msg { font-size: 18px; color: #333; background: #e8f5e9; padding: 15px; border-radius: 5px; margin: 20px 0; font-weight: bold; }
        a { display: inline-block; margin-top: 15px; text-decoration: none; background: #6c757d; color: white; padding: 10px 20px; border-radius: 4px; font-weight: bold; }
        a:hover { background: #5a6268; }
    </style>
</head>
<body>
    <div class="card">
        <h1>ESP32 Dashboard</h1>
        <p>Hardcoded Firmware Message:</p>
        <div class="msg" id="fwMsg">Loading...</div>
        <a href="/config">⚙️ Config & Firmware Update</a>
    </div>
    <script>
        fetch('/get-info')
            .then(res => res.json())
            .then(data => {
                document.getElementById('fwMsg').innerText = data.message;
            });
    </script>
</body>
</html>
)rawliteral";

// --- Config & OTA Page HTML ---
const char CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Config & OTA Update</title>
    <style>
        body { font-family: Arial, sans-serif; background: #f4f6f8; display: flex; justify-content: center; padding: 20px; }
        .container { width: 100%; max-width: 450px; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }
        h2 { text-align: center; color: #333; }
        a { color: #007bff; text-decoration: none; font-weight: bold; display: inline-block; margin-bottom: 15px; }
        label { font-weight: bold; font-size: 13px; color: #555; display: block; margin-top: 10px; }
        input[type="file"] { padding: 8px; width: 100%; box-sizing: border-box; margin-top: 5px; margin-bottom: 15px; }
        button { width: 100%; padding: 12px; font-size: 15px; border-radius: 4px; border: none; cursor: pointer; background-color: #28a745; color: white; font-weight: bold; }
        button:hover { background-color: #218838; }
        #status { text-align: center; font-weight: bold; margin-top: 15px; color: #007bff; }
    </style>
</head>
<body>
    <div class="container">
        <a href="/">&larr; Back to Dashboard</a>
        <h2>Firmware Update (OTA)</h2>
        <form method="POST" action="/update" enctype="multipart/form-data" onsubmit="document.getElementById('status').innerText = 'Flashing new firmware... Please wait!';">
            <label>Select Compiled Arduino .bin File:</label>
            <input type="file" name="update" accept=".bin" required>
            <button type="submit">Upload & Flash</button>
            <div id="status"></div>
        </form>
    </div>
</body>
</html>
)rawliteral";

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Connect to Wi-Fi
    WiFi.begin(DEFAULT_SSID, DEFAULT_PASSWORD);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Connected! IP Address: ");
    Serial.println(WiFi.localIP());

    // --- Web Server Routes ---
    server.on("/", HTTP_GET, []() {
        if (!server.authenticate(DEFAULT_WEB_USER, DEFAULT_WEB_PASS)) {
            return server.requestAuthentication();
        }
        server.send_P(200, "text/html", DASHBOARD_HTML);
    });

    server.on("/get-info", HTTP_GET, []() {
        if (!server.authenticate(DEFAULT_WEB_USER, DEFAULT_WEB_PASS)) {
            return server.requestAuthentication();
        }
        JsonDocument doc;
        doc["message"] = FIRMWARE_MESSAGE;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/config", HTTP_GET, []() {
        if (!server.authenticate(DEFAULT_WEB_USER, DEFAULT_WEB_PASS)) {
            return server.requestAuthentication();
        }
        server.send_P(200, "text/html", CONFIG_HTML);
    });

    // --- Web OTA Update Route for ESP32 ---
    server.on("/update", HTTP_POST, []() {
        if (!server.authenticate(DEFAULT_WEB_USER, DEFAULT_WEB_PASS)) {
            return server.requestAuthentication();
        }
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            server.send(200, "text/html", "<h3>Update Failed! Rebooting...</h3>");
        } else {
            server.send(200, "text/html", "<h3>Update Success! Rebooting ESP32...</h3><script>setTimeout(()=>{window.location='/'}, 5000);</script>");
        }
        delay(1000);
        ESP.restart();
    }, []() {
        if (!server.authenticate(DEFAULT_WEB_USER, DEFAULT_WEB_PASS)) return;
        HTTPUpload& upload = server.upload();
        
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update Start: %s\n", upload.filename.c_str());
            // Start with unknown size to handle generic binary uploads smoothly on ESP32
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) { // true sets the actual size to the progress accumulated
                Serial.printf("Update Success: %u bytes\nRebooting...\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
        yield();
    });

    server.begin();
    Serial.println("Web Server Started!");
}

void loop() {
    server.handleClient();
}