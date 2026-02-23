#include <Arduino.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <WiFi.h>

// --- Credentials & Configuration ---
// IMPORTANT: Replace these placeholders with your own network credentials
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
const char *mqtt_server = "YOUR_MQTT_BROKER_IP";
const int mqtt_port = 1883;
// MQTT Credentials (Required for Home Assistant Mosquitto default)
const char *mqtt_user = "YOUR_MQTT_USER";
const char *mqtt_password = "YOUR_MQTT_PASSWORD";

// --- MQTT Topics ---
const char *topic_commands = "roomba/commands";
const char *topic_status = "roomba/estado";

// --- Pin Definitions ---
#define RX2_PIN 16
#define TX2_PIN 17
#define BRC_PIN 14 // Device Detect / Baud Rate Change

// --- Global Objects ---
WiFiClient espClient;
PubSubClient client(espClient);

// --- Timing Variables (Non-blocking) ---
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 5000;

// --- Safety Variables (Watchdog) ---
unsigned long lastMsgTime = 0;
const unsigned long FAILSAFE_TIMEOUT = 30000;
bool failsafeTriggered = false;

// --- Function Declarations ---
void setup_wifi();
void callback(char *topic, byte *payload, unsigned int length);
void wakeUpRoomba();
boolean reconnect();
void move(int velocity, int radius);
void playSong(int songNumber);
void loadSongs();

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Starting Roomba ESP32 Bridge ---");

  pinMode(BRC_PIN, OUTPUT);
  digitalWrite(BRC_PIN, HIGH);

  Serial2.begin(115200, SERIAL_8N1, RX2_PIN, TX2_PIN);
  Serial.println("Serial2 started for Roomba (RX:16, TX:17)");

  wakeUpRoomba();
  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  ArduinoOTA.setHostname("RoombaESP32");
  ArduinoOTA.begin();
}

void loop() {
  ArduinoOTA.handle();

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > reconnectInterval) {
      lastReconnectAttempt = now;
      if (reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    client.loop();

    // Watchdog — Uncomment for production use
    /*
    if (millis() - lastMsgTime > FAILSAFE_TIMEOUT && !failsafeTriggered) {
      Serial.println("FAILSAFE: Timeout. Stopping.");
      Serial2.write(173); // STOP
      failsafeTriggered = true;
    }
    */

    // Sensor Reading (Every 2 seconds)
    static unsigned long last_sensor_time = 0;
    if (millis() - last_sensor_time > 2000) {
      last_sensor_time = millis();

      while (Serial2.available())
        Serial2.read();

      // Request Packet Group 3 (Charging state, voltage, current, charge, capacity)
      Serial2.write(142);
      Serial2.write(3);

      unsigned long wait_start = millis();
      while (Serial2.available() < 10 && millis() - wait_start < 100) {
        delay(1);
      }

      if (Serial2.available() >= 10) {
        byte buf[10];
        Serial2.readBytes(buf, 10);

        uint8_t chargingState = buf[0];
        uint16_t voltage = (buf[1] << 8) | buf[2];
        int16_t current = (buf[3] << 8) | buf[4];
        uint16_t charge = (buf[6] << 8) | buf[7];
        uint16_t capacity = (buf[8] << 8) | buf[9];

        int battery_percent = 0;
        if (capacity > 0)
          battery_percent = (charge * 100) / capacity;
        if (battery_percent > 100)
          battery_percent = 100;

        // --- Heuristic Status Detection ---
        String statusStr = "idle";

        if (chargingState > 0 && chargingState < 4) {
          statusStr = "charging";
          if (chargingState == 3)
            statusStr = "trickle_charging";
        } else if (chargingState == 4) {
          statusStr = "docked";
        } else {
          // Only mark "cleaning" if current draw is high (motors on)
          // Threshold set to -500mA to avoid false positives
          if (current < -500)
            statusStr = "cleaning";
          else
            statusStr = "idle";
        }

        char jsonBuffer[128];
        snprintf(jsonBuffer, sizeof(jsonBuffer),
                 "{\"battery_level\": %d, \"voltage\": %d, \"status\": \"%s\", "
                 "\"charging\": %s}",
                 battery_percent, voltage, statusStr.c_str(),
                 (chargingState > 0 && chargingState < 5) ? "true" : "false");

        client.publish("roomba/estado", jsonBuffer);
      }
    }
  }
}

void wakeUpRoomba() {
  Serial.println("Waking up Roomba...");
  digitalWrite(BRC_PIN, HIGH);
  delay(100);
  digitalWrite(BRC_PIN, LOW);
  delay(500);
  digitalWrite(BRC_PIN, HIGH);
  delay(2000); // Wait for bootloader to finish

  // Robust start sequence
  Serial2.write(128); // START
  delay(100);
  Serial2.write(131); // SAFE MODE
  delay(100);

  // Load songs on boot
  loadSongs();
  Serial.println("Roomba in SAFE mode. Songs loaded.");
}

void setup_wifi() {
  delay(10);
  Serial.print("WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed.");
  }
}

boolean reconnect() {
  String clientId = "ESP32Roomba-" + String(random(0xffff), HEX);
  if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
    Serial.println("MQTT Connected");
    client.subscribe(topic_commands);
    return true;
  }
  return false;
}

void callback(char *topic, byte *payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Cmd: ");
  Serial.println(message);

  lastMsgTime = millis();
  failsafeTriggered = false;

  if (String(topic) == topic_commands) {
    // --- START / TURN_ON Commands ---
    if (message == "start" || message == "turn_on") {
      wakeUpRoomba();
      delay(1000);
      Serial2.write(135); // CLEAN
      Serial.println("Action: Start -> Clean");
    }
    // --- STOP / TURN_OFF Commands ---
    else if (message == "stop" || message == "turn_off") {
      // ROBUST STOP:
      // To stop the "Clean" cycle (Passive mode), we must switch to Safe Mode
      // (131). Simply sending Drive 0 (move(0,0)) does not stop cleaning.
      Serial2.write(128); // Start
      delay(50);
      Serial2.write(131); // Safe Mode
      delay(50);
      move(0, 0); // Handbrake
      Serial.println("Action: Stop (Robust: Safe Mode + Drive 0)");
    }
    // --- Manual Movement Commands ---
    else if (message == "forward") {
      move(200, 32768); // Straight
      Serial.println("Action: Forward");
    } else if (message == "backward") {
      move(-200, 32768); // Reverse
      Serial.println("Action: Backward");
    } else if (message == "left") {
      move(200, 1); // Spin Left (in-place)
      Serial.println("Action: Left");
    } else if (message == "right") {
      move(200, -1); // Spin Right (in-place)
      Serial.println("Action: Right");
    }
    // --- Diagonal Commands ---
    else if (message == "forward_left") {
      move(200, 300); // Gentle curve left
      Serial.println("Action: Forward-Left");
    } else if (message == "forward_right") {
      move(200, -300); // Gentle curve right
      Serial.println("Action: Forward-Right");
    } else if (message == "backward_left") {
      move(-200, 300); // Reverse curve left
      Serial.println("Action: Backward-Left");
    } else if (message == "backward_right") {
      move(-200, -300); // Reverse curve right
      Serial.println("Action: Backward-Right");
    }
    // --- Entertainment Commands ---
    else if (message == "starwars") {
      playSong(0);
      Serial.println("Action: Playing Star Wars");
    } else if (message == "mario") {
      playSong(1);
      Serial.println("Action: Playing Mario Bros");
    }
    // --- Cleaning / Dock Commands ---
    else if (message == "clean") {
      wakeUpRoomba();
      delay(500);
      Serial2.write(135);
      Serial.println("Action: Clean");
    } else if (message == "dock" || message == "return_to_base") {
      wakeUpRoomba();
      delay(500);
      Serial2.write(143); // Dock
      Serial.println("Action: Dock");
    }
    // --- POWER DOWN Command ---
    else if (message == "power_down") {
      Serial2.write(133); // Power Down
      Serial.println("Action: Power Down");
    }
  }
}

// --- Roomba Control Functions ---

void move(int velocity, int radius) {
  // AUTO-SAFE MODE:
  // If the robot is docked (Passive), it ignores Drive commands.
  // We send 131 (Safe) before moving to ensure it obeys.
  Serial2.write(131);

  // Clamp velocity -500 to 500
  if (velocity > 500)
    velocity = 500;
  if (velocity < -500)
    velocity = -500;

  // Opcode 137: Drive
  Serial2.write(137);
  Serial2.write((velocity >> 8) & 0xFF);
  Serial2.write(velocity & 0xFF);
  Serial2.write((radius >> 8) & 0xFF);
  Serial2.write(radius & 0xFF);
}

void playSong(int songNumber) {
  // Opcode 141: Play Song
  Serial2.write(141);
  Serial2.write(songNumber);
}

void loadSongs() {
  // Opcode 140: Song
  // Song 0: Star Wars (Imperial March - 16 notes)
  Serial2.write(140);
  Serial2.write(0);  // Song Number
  Serial2.write(16); // Length (Max 16)

  // Phrase 1
  Serial2.write(55);
  Serial2.write(32); // G4
  Serial2.write(55);
  Serial2.write(32); // G4
  Serial2.write(55);
  Serial2.write(32); // G4
  Serial2.write(51);
  Serial2.write(24); // Eb4
  Serial2.write(58);
  Serial2.write(8); // Bb4
  Serial2.write(55);
  Serial2.write(32); // G4
  Serial2.write(51);
  Serial2.write(24); // Eb4
  Serial2.write(58);
  Serial2.write(8); // Bb4
  Serial2.write(55);
  Serial2.write(64); // G4
  // Phrase 2 - High notes
  Serial2.write(62);
  Serial2.write(32); // D5
  Serial2.write(62);
  Serial2.write(32); // D5
  Serial2.write(62);
  Serial2.write(32); // D5
  Serial2.write(63);
  Serial2.write(24); // Eb5
  Serial2.write(58);
  Serial2.write(8); // Bb4
  Serial2.write(54);
  Serial2.write(32); // Gb4
  Serial2.write(51);
  Serial2.write(24); // Eb4

  delay(50);

  // Song 1: Super Mario Bros (Intro - 13 notes)
  Serial2.write(140);
  Serial2.write(1);  // Song Number
  Serial2.write(13); // Length

  Serial2.write(76);
  Serial2.write(18); // E5
  Serial2.write(76);
  Serial2.write(18); // E5
  Serial2.write(76);
  Serial2.write(18); // E5
  Serial2.write(72);
  Serial2.write(18); // C5
  Serial2.write(76);
  Serial2.write(18); // E5
  Serial2.write(79);
  Serial2.write(36); // G5
  Serial2.write(67);
  Serial2.write(36); // G4
  // Extended part
  Serial2.write(72);
  Serial2.write(28); // C5
  Serial2.write(67);
  Serial2.write(28); // G4
  Serial2.write(64);
  Serial2.write(28); // E4
  Serial2.write(69);
  Serial2.write(28); // A4
  Serial2.write(71);
  Serial2.write(28); // B4
  Serial2.write(70);
  Serial2.write(18); // Bb4
}
