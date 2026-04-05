#include <Arduino.h>      // Base Arduino library for ESP32
#include <ArduinoOTA.h>    // Library for Over-The-Air (OTA) wireless updates
#include <PubSubClient.h>  // MQTT client library
#include <WiFi.h>          // WiFi connection management library

// --- Credentials and Configuration ---
// These variables define access to the local network and the home automation server.
const char *ssid = "YOUR_WIFI_SSID";             // WiFi access point name
const char *password = "YOUR_WIFI_PASSWORD";     // WiFi network security key
const char *mqtt_server = "192.168.1.100";       // IP address of the MQTT Broker (e.g., Home Assistant)
const int mqtt_port = 1883;                      // Standard MQTT communication port
// Credentials for MQTT server access
const char *mqtt_user = "YOUR_MQTT_USER";        // Username configured in Mosquitto
const char *mqtt_password = "YOUR_MQTT_PASSWORD";// Password configured in Mosquitto

// --- MQTT Topics ---
// Topics where the ESP32 listens for commands and publishes status information.
const char *topic_commands = "roomba/commands";  // Incoming commands (e.g., start, stop, forward)
const char *topic_status = "roomba/estado";      // Outgoing status payload in JSON format

// --- Pin Definitions ---
// Specific ESP32 pins used for UART communication with the Roomba.
#define RX2_PIN 16     // Receive pin (connected to Roomba's TX)
#define TX2_PIN 17     // Transmit pin (connected to Roomba's RX)
#define BRC_PIN 14     // Pin for "Baud Rate Change" / "Device Detect" (Pin 5 on the Mini-DIN)

// --- Global Objects ---
WiFiClient espClient;           // Creates a TCP network client
PubSubClient client(espClient); // Creates the MQTT client using the network client

// --- Time Control Variables (Non-blocking) ---
unsigned long lastReconnectAttempt = 0; // Stores the timestamp of the last MQTT connection attempt
const unsigned long reconnectInterval = 5000; // 5-second interval between reconnection attempts

// --- Safety Variables (Watchdog) ---
unsigned long lastMsgTime = 0; // Stores the timestamp of the last received MQTT message
const unsigned long FAILSAFE_TIMEOUT = 30000; // 30 seconds inactivity limit
bool failsafeTriggered = false; // Indicates if the safety stop was triggered

// --- Function Declarations ---
void setup_wifi();                               // Pre-declaration: Wireless network setup
void callback(char *topic, byte *payload, unsigned int length); // Pre-declaration: MQTT payload processing
void wakeUpRoomba();                             // Pre-declaration: Roomba wake-up routine
boolean reconnect();                             // Pre-declaration: MQTT broker reconnection
void move(int velocity, int radius);             // Pre-declaration: Movement control
void playSong(int songNumber);                   // Pre-declaration: Audio playback
void loadSongs();                                // Pre-declaration: Loads songs into Roomba RAM

void setup() {                                    // Initial configuration function (runs once)
  Serial.begin(115200);                           // Initialize serial port for USB debugging
  Serial.println("\n--- Starting Roomba ESP32 Bridge ---"); 

  // Configure the BRC pin as an output to wake up the robot
  pinMode(BRC_PIN, OUTPUT);                      
  digitalWrite(BRC_PIN, HIGH);                   // Set pin to high state by default

  // Initialization of the second hardware Serial port (UART2)
  // Parameters: Baud rate (115200), Mode (8 data bits, no parity, 1 stop bit), RX/TX Pins
  Serial2.begin(115200, SERIAL_8N1, RX2_PIN, TX2_PIN); // Start communication with Roomba
  Serial.println("Serial2 initialized for Roomba (RX:16, TX:17)"); 

  wakeUpRoomba();                                // Call routine to wake up the robot
  setup_wifi();                                  // Call Wi-Fi connection routine

  // Configure the MQTT server and the callback function
  client.setServer(mqtt_server, mqtt_port);      
  client.setCallback(callback);                  

  // Initialization of wireless updates (Over The Air)
  ArduinoOTA.setHostname("RoombaESP32");         // Device name on the network
  ArduinoOTA.begin();                            // Start the remote upload service
}                                                

void loop() {
  ArduinoOTA.handle();                  // Manage possible OTA update requests via WiFi

  if (!client.connected()) {            // If the MQTT client is disconnected
    unsigned long now = millis();       
    if (now - lastReconnectAttempt > reconnectInterval) { // If 5 seconds have passed
      lastReconnectAttempt = now;       // Update attempt timestamp
      if (reconnect()) {                // Try to reconnect to the broker
        lastReconnectAttempt = 0;       // Reset timestamp on success
      }
    }
  } else {
    client.loop();                      // Keep MQTT communication alive and process callbacks

    // Sensor Reading (Every 2 seconds to avoid saturating the UART bus)
    static unsigned long last_sensor_time = 0;
    if (millis() - last_sensor_time > 2000) {
      last_sensor_time = millis();

      // Clear any old leftover data in the serial buffer
      while (Serial2.available())
        Serial2.read();

      // Request sensor data (Polling)
      // OpCode 142 (Query): Request sensory data. 
      // Packet ID 3: Requests a group of 10 sensors (includes battery, voltage, charge, etc.)
      Serial2.write(142); 
      Serial2.write(3);   

      unsigned long wait_start = millis();
      // Wait for data with a 100ms timeout
      while (Serial2.available() < 10 && millis() - wait_start < 100) {
        delay(1);
      }

      if (Serial2.available() >= 10) { // If 10 bytes actally arrived (size of packet 3)
        byte buf[10];
        Serial2.readBytes(buf, 10);    // Read the 10 bytes and store them in the buffer

        // Byte decoding according to the "iRobot OI Spec" manual
        uint8_t chargingState = buf[0];                // Charging state (0: Not charging, 1: Reconditioning, 2: Full Charge, 3: Trickle Charge, 4: Waiting, 5: Fault)
        uint16_t voltage = (buf[1] << 8) | buf[2];     // Combine High and Low Byte to get battery millivolts
        int16_t current = (buf[3] << 8) | buf[4];      // Combine bytes to get current (Positive: Charging, Negative: Discharging/Driving)
        uint16_t charge = (buf[6] << 8) | buf[7];      // Get current stored charge in mAh
        uint16_t capacity = (buf[8] << 8) | buf[9];    // Maximum design capacity of the battery in mAh

        int battery_percent = 0;                       // Initialize percentage variable
        if (capacity > 0)                              // Avoid division by zero
          battery_percent = (charge * 100) / capacity; // Calculate relative percentage against maximum capacity
        if (battery_percent > 100)                     // Cap at 100%
          battery_percent = 100;

        String statusStr = "idle";                     // Initialize status string as idle

        if (chargingState > 0 && chargingState < 4) {  // If the robot is actively charging
          statusStr = "charging";                      
          if (chargingState == 3)                      // If state is trickle charging
            statusStr = "trickle_charging";            
        } else if (chargingState == 4) {               // If the robot is docked but fully charged
          statusStr = "docked";                        
        } else {
          if (current < -500)                          // If consuming more than 500mA (motors active)
            statusStr = "cleaning";                     
          else                                         // Otherwise
            statusStr = "idle";                        // Remains idle
        }

        char jsonBuffer[128];                          // Buffer to build the JSON string
        snprintf(jsonBuffer, sizeof(jsonBuffer),       // Format the JSON with the read variables
                 "{\"battery_level\": %d, \"voltage\": %d, \"status\": \"%s\", "
                 "\"charging\": %s}",                  
                 battery_percent, voltage, statusStr.c_str(), 
                 (chargingState > 0 && chargingState < 5) ? "true" : "false"); 

        client.publish("roomba/estado", jsonBuffer);   // Emit JSON to the MQTT broker
      }                                                
    }                                                  
  }                                                    
}                                                      

// wakeUpRoomba: Sequence of electrical pulses and OI commands to turn on the robot
void wakeUpRoomba() {
  Serial.println("Waking up Roomba...");         
  // The robot enters sleep mode if idle. To wake it up, the BRC (Device Detect) pin
  // must receive a LOW pulse of at least 500ms.
  digitalWrite(BRC_PIN, HIGH);                     // Ensure initial high state
  delay(100);                                      
  digitalWrite(BRC_PIN, LOW);                      // Pull to ground (0V) to request attention
  delay(500);                                      // Hold pulse for half a second
  digitalWrite(BRC_PIN, HIGH);                     // Return to resting state (3.3V)
  delay(2000);                                     // Allow robot firmware to boot

  // Open Interface (OI) protocol initialization commands
  Serial2.write(128);                              // OpCode 128: START (Initiates OI mode)
  delay(100);                                      
  Serial2.write(131);                              // OpCode 131: SAFE MODE (Safe mode active, cliff/bumper sensors working)
  delay(100);                                      

  loadSongs();                                     // Preload custom melodies into iRobot RAM
  Serial.println("Roomba is in SAFE mode and songs loaded."); 
}

void setup_wifi() {
  delay(10);                             
  Serial.print("WiFi: ");               
  Serial.println(ssid);                  
  WiFi.mode(WIFI_STA);                   // Configure ESP32 as a station (client)
  WiFi.begin(ssid, password);            // Start connection process
  int attempts = 0;                      
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { // Wait loop with timeout
    delay(500);                          
    Serial.print(".");                  
    attempts++;                          
  }
  if (WiFi.status() == WL_CONNECTED) {   // If WiFi connected successfully
    Serial.println("\nConnected. IP: "); 
    Serial.println(WiFi.localIP());      // Show local device IP address
  } else {                               
    Serial.println("\nWiFi Failed.");    
  }                                      
}                                        

boolean reconnect() {                             // Function for persistent MQTT reconnection
  String clientId = "ESP32Roomba-" + String(random(0xffff), HEX); // Create unique random ID
  if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) { 
    Serial.println("MQTT Connected");   
    client.subscribe(topic_commands);    // Subscribe to commands topic
    return true;                         
  }                                      
  return false;                          
}                                        

void callback(char *topic, byte *payload, unsigned int length) {
  String message = "";
  // Convert payload (byte array) to a readable String
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Cmd: ");
  Serial.println(message);               // Show message over USB monitor

  // Update timestamps for safety watchdog (if enabled)
  lastMsgTime = millis();
  failsafeTriggered = false; 

  if (String(topic) == topic_commands) {                
    // --- START / TURN_ON Commands ---
    if (message == "start" || message == "turn_on") {           
      wakeUpRoomba();                                           
      delay(1000);                                              
      Serial2.write(135);                                        // OpCode 135: Start 'Clean' cycle
      Serial.println("Action: Start -> Clean");                 
    }                                                           
    // --- STOP / TURN_OFF Commands ---
    else if (message == "stop" || message == "turn_off") {       
      Serial2.write(128);                                        // OpCode 128: Enter Passive Mode
      delay(50);                                                
      Serial2.write(131);                                        // OpCode 131: Enter Safe Mode
      delay(50);                                                
      move(0, 0);                                               // Stop motors
      Serial.println("Action: Stop (Robust: Safe Mode + Drive 0)"); 
    }                                                           
    // --- Manual Movement Commands ---
    else if (message == "forward") {                      
      move(200, 32768);                                   // 200mm/s in a straight line
      Serial.println("Action: Forward");                  
    } else if (message == "backward") {                   
      move(-200, 32768);                                  // Negative velocity (backwards)
      Serial.println("Action: Backward");                 
    } else if (message == "left") {                       
      move(200, 1);                                       // Spin left on its axis
      Serial.println("Action: Left");                     
    } else if (message == "right") {                      
      move(200, -1);                                      // Spin right on its axis
      Serial.println("Action: Right");                    
    }
    // --- Diagonal Commands (Circular curves) ---
    else if (message == "forward_left") {                 
      move(200, 300);                                     // Smooth forward-left curve
      Serial.println("Action: Forward-Left");             
    } else if (message == "forward_right") {                
      move(200, -300);                                    // Negative radius curve
      Serial.println("Action: Forward-Right");            
    } else if (message == "backward_left") {               
      move(-200, 300);                                    
      Serial.println("Action: Backward-Left");            
    } else if (message == "backward_right") {              
      move(-200, -300);                                   
      Serial.println("Action: Backward-Right");           
    }
    // --- Entertainment Commands ---
    else if (message == "starwars") {
      playSong(0);                       // Triggers song 0 playback
      Serial.println("Action: Playing Star Wars");
    } else if (message == "mario") {
      playSong(1);                       // Triggers song 1 playback
      Serial.println("Action: Playing Mario Bros");
    }
    // --- Cleaning / Docking Commands ---
    else if (message == "clean") {                        
      wakeUpRoomba();                                   
      delay(500);                                       
      Serial2.write(135);                                // OpCode 135: Start cleaning 
      Serial.println("Action: Clean");                  
    } else if (message == "dock" || message == "return_to_base") { 
      wakeUpRoomba();                                   
      delay(500);                                       
      Serial2.write(143);                                // OpCode 143: SEEK DOCK (Return to charger)
      Serial.println("Action: Dock");                   
    }                                                   
    // --- POWER_DOWN Command ---
    else if (message == "power_down") {                  
      Serial2.write(133); // OpCode 133: POWER DOWN (Puts robot into deep sleep)
      Serial.println("Action: Power Down");               
    }                                                     
  }                                                       
}                                                         

// --- Roomba Control Functions ---

void move(int velocity, int radius) {
  // Before moving, ensure the robot is in SAFE MODE (131)
  // so bumper and cliff sensors stay active to avoid falls/crashes.
  Serial2.write(131);

  // Velocity is capped between -500 and 500 mm/s by iRobot specs
  if (velocity > 500)
    velocity = 500;
  if (velocity < -500)
    velocity = -500;

  // OpCode 137: DRIVE (Engine movement directive)
  // Requires 4 data bytes: 2 for velocity, 2 for turning radius.
  Serial2.write(137);
  Serial2.write((velocity >> 8) & 0xFF); // High byte for velocity
  Serial2.write(velocity & 0xFF);        // Low byte for velocity
  Serial2.write((radius >> 8) & 0xFF);   // High byte for radius
  Serial2.write(radius & 0xFF);          // Low byte for radius
}                                        

void playSong(int songNumber) {          // Plays a sound pre-stored in memory
  // OpCode 141: PLAY
  Serial2.write(141);                    // Flash memory playback command
  Serial2.write(songNumber);             // Song ID (0 to 15 allowed)
}                                        

void loadSongs() {                             // Pre-loads melodies to RAM
  // OpCode 140: SONG (Defines a song structure)
  // Song 0: Star Wars (Imperial March)
  Serial2.write(140);                               // Definition command
  Serial2.write(0);                                 // Song ID: 0
  Serial2.write(16);                                // Length: 16 notes

  // Phrase 1 
  Serial2.write(55); Serial2.write(32); // Note: G4 | Duration: 32 ticks
  Serial2.write(55); Serial2.write(32); // Note: G4
  Serial2.write(55); Serial2.write(32); // Note: G4
  Serial2.write(51); Serial2.write(24); // Note: Eb4 | Duration: 24 ticks
  Serial2.write(58); Serial2.write(8);  // Note: Bb4 | Duration: 8 ticks
  Serial2.write(55); Serial2.write(32); // Note: G4
  Serial2.write(51); Serial2.write(24); // Note: Eb4
  Serial2.write(58); Serial2.write(8);  // Note: Bb4
  Serial2.write(55); Serial2.write(64); // Note: G4 (Long Note)

  // Phrase 2 - High notes
  Serial2.write(62); Serial2.write(32); // Note: D5 | Duration: 32 ticks
  Serial2.write(62); Serial2.write(32); // Note: D5
  Serial2.write(62); Serial2.write(32); // Note: D5
  Serial2.write(63); Serial2.write(24); // Note: Eb5
  Serial2.write(58); Serial2.write(8);  // Note: Bb4
  Serial2.write(54); Serial2.write(32); // Note: F#4
  Serial2.write(51); Serial2.write(24); // Note: Eb4

  delay(50);                             // Short pause to ensure internal saving

  // Song 1: Super Mario Bros (Main Theme - Intro)
  Serial2.write(140);                   
  Serial2.write(1);                     // Song ID: 1
  Serial2.write(13);                    // Length: 13 notes

  Serial2.write(76); Serial2.write(18); // Note: E5 (669 Hz) | Duration: 18 ticks (1 tick = 1/64 sec)
  Serial2.write(76); Serial2.write(18); // Note: E5
  Serial2.write(76); Serial2.write(18); // Note: E5
  Serial2.write(72); Serial2.write(18); // Note: C5
  Serial2.write(76); Serial2.write(18); // Note: E5
  Serial2.write(79); Serial2.write(36); // Note: G5 | Duration: 36 ticks 
  Serial2.write(67); Serial2.write(36); // Note: G4 

  // Final notes of the intro
  Serial2.write(72); Serial2.write(28); // Note: C5
  Serial2.write(67); Serial2.write(28); // Note: G4
  Serial2.write(64); Serial2.write(28); // Note: E4
  Serial2.write(69); Serial2.write(28); // Note: A4
  Serial2.write(71); Serial2.write(28); // Note: B4
  Serial2.write(70); Serial2.write(18); // Note: Bb4
}
