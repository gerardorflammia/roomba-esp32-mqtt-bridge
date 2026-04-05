#include <Arduino.h> // Librería base de Arduino para ESP32
#include <ArduinoOTA.h> // Librería para actualizaciones inalámbricas (Over-The-Air)
#include <PubSubClient.h> // Librería para el cliente MQTT
#include <WiFi.h>         // Librería para la gestión de la conexión WiFi

// --- Credenciales y Configuración ---
// Estas variables definen el acceso a la red local y al servidor domótico.
const char *ssid = "MikroTik";     // Nombre del punto de acceso WiFi
const char *password = "12345678"; // Clave de seguridad de la red WiFi
const char *mqtt_server =
    "192.168.10.5";         // IP del Broker (donde corre Home Assistant)
const int mqtt_port = 1883; // Puerto de comunicación MQTT (estándar)
// Credenciales para el acceso al servidor MQTT
const char *mqtt_user = "roomba";     // Usuario configurado en Mosquitto
const char *mqtt_password = "roomba"; // Contraseña configurada en Mosquitto

// --- Topics MQTT ---
// Los temas donde el ESP32 escucha órdenes y publica información.
const char *topic_commands =
    "roomba/commands"; // Comandos entrantes (ej: start, stop)
const char *topic_status = "roomba/estado"; // Estado saliente en formato JSON

// --- Definición de Pines ---
// Se utilizan pines específicos del ESP32 para la comunicación con el robot.
#define RX2_PIN 16 // Pin de recepción (conectado al TX del Roomba)
#define TX2_PIN 17 // Pin de transmisión (conectado al RX del Roomba)
#define BRC_PIN                                                                \
  14 // Pin para "Baud Rate Change" / "Device Detect" (Pin 5 del Mini-DIN)

// --- Objetos Globales ---
WiFiClient espClient;           // Crea un cliente de red TCP
PubSubClient client(espClient); // Crea el cliente MQTT usando el cliente de red

// --- Variables para control de tiempo (Non-blocking) ---
unsigned long lastReconnectAttempt =
    0; // Almacena el último intento de conexión MQTT
const unsigned long reconnectInterval =
    5000; // Intervalo de 5 segundos entre reconexiones

// --- Variables de Seguridad (Watchdog) ---
unsigned long lastMsgTime =
    0; // Almacena la hora del último mensaje MQTT recibido
const unsigned long FAILSAFE_TIMEOUT =
    30000;                      // Tiempo límite de 30 segundos de inactividad
bool failsafeTriggered = false; // Indica si se activó la parada de seguridad

// --- Declaración de Funciones ---
void setup_wifi(); // Pre-declaración: Configuración de red inalámbrica
void callback(char *topic, byte *payload,
              unsigned int length); // Pre-declaración: Procesamiento MQTT
void wakeUpRoomba();                // Pre-declaración: Despertado del robot
boolean reconnect(); // Pre-declaración: Reconexión al broker MQTT
void move(int velocity, int radius); // Pre-declaración: Control de movimiento
void playSong(int songNumber);       // Pre-declaración: Reproducción de audio
void loadSongs(); // Pre-declaración: Carga de canciones en RAM

void setup() {          // Función de configuración inicial (corre una vez)
  Serial.begin(115200); // Inicia puerto serie para depuración USB
  Serial.println(
      "\n--- Iniciando Roomba ESP32 Bridge ---"); // Mensaje de bienvenida en
                                                  // consola

  // Configuración del pin BRC como salida para poder despertar al robot
  pinMode(BRC_PIN, OUTPUT);    // Establece el pin 14 como salida digital
  digitalWrite(BRC_PIN, HIGH); // Pone el pin en estado alto por defecto

  // Inicialización del segundo puerto Serial de hardware (UART2)
  // Parámetros: Baudios (115200), Modo (8 bits, sin paridad, 1 bit parada),
  // Pines RX/TX
  Serial2.begin(115200, SERIAL_8N1, RX2_PIN,
                TX2_PIN); // Inicia comunicación con Roomba
  Serial.println(
      "Serial2 iniciado para Roomba (RX:16, TX:17)"); // Confirma pines en log

  wakeUpRoomba(); // Llama a la rutina para encender el robot
  setup_wifi();   // Llama a la rutina de conexión inalámbrica

  // Configuración del servidor MQTT y la función de respuesta (callback)
  client.setServer(mqtt_server,
                   mqtt_port);  // Define dirección y puerto del broker
  client.setCallback(callback); // Asigna la función que procesa mensajes

  // Inicialización de actualizaciones inalámbricas (Over The Air)
  ArduinoOTA.setHostname("RoombaESP32"); // Nombre del dispositivo en la red
  ArduinoOTA.begin();                    // Inicia el servicio de carga remota
} // Fin de la función setup

void loop() {
  ArduinoOTA.handle(); // Gestiona posibles peticiones de actualización vía WiFi

  if (!client.connected()) {      // Si el cliente MQTT se ha desconectado
    unsigned long now = millis(); // Obtiene el tiempo actual en milisegundos
    if (now - lastReconnectAttempt >
        reconnectInterval) {      // Si han pasado 5 segundos
      lastReconnectAttempt = now; // Actualiza la marca de tiempo del intento
      if (reconnect()) {          // Intenta reconectar al broker
        lastReconnectAttempt = 0; // Resetea si tiene éxito
      }
    }
  } else {
    client.loop(); // Mantiene viva la comunicación MQTT y procesa callbacks

    // Watchdog (Sistema de seguridad ante pérdida de control)
    /* DESACTIVADO TEMPORALMENTE PARA PRUEBAS (Evitaba el movimiento libre en
    debug) if (millis() - lastMsgTime > FAILSAFE_TIMEOUT && !failsafeTriggered)
    { // Si hay silencio largo Serial.println("FAILSAFE: Timeout. Stopping.");
    // Informa el error por USB Serial2.write(173); // OpCode 173: Detiene el
    sistema OI completamente por seguridad failsafeTriggered = true; // Marca
    que el seguro se ha activado
    }
    */

    // Lectura de Sensores (Cada 2 segundos para no saturar el bus)
    static unsigned long last_sensor_time = 0;
    if (millis() - last_sensor_time > 2000) {
      last_sensor_time = millis();

      // Limpia restos de datos viejos en el buffer serial
      while (Serial2.available())
        Serial2.read();

      // Solicitud de datos de sensores (Polling)
      // OpCode 142 (Query): Pide datos.
      // Packet ID 3: Solicita un grupo de 10 sensores (incluye batería,
      // voltaje, carga, etc.)
      Serial2.write(142);
      Serial2.write(3);

      unsigned long wait_start = millis();
      while (Serial2.available() < 10 && millis() - wait_start < 100) {
        delay(1);
      }

      if (Serial2.available() >= 10) { // Si hay 10 bytes (tamaño del paquete 3)
        byte buf[10];
        Serial2.readBytes(buf,
                          10); // Lee los 10 bytes y los guarda en el buffer

        // Decodificación de los bytes según el manual "OI Spec"
        uint8_t chargingState =
            buf[0]; // Estado de carga (0: No cargando, 1: Recuperación, 2:
                    // Carga, 3: Goteo, 4: Espera, 5: Error)
        uint16_t voltage =
            (buf[1] << 8) | buf[2]; // Combina Byte Alto y Bajo para obtener
                                    // milivoltios de batería
        int16_t current =
            (buf[3] << 8) | buf[4]; // Combina bytes para obtener corriente
                                    // (Positivo: Carga, Negativo: Consumo)
        uint16_t charge =
            (buf[6] << 8) | buf[7]; // Obtiene la carga actual almacenada en mAh
        uint16_t capacity =
            (buf[8] << 8) |
            buf[9]; // Capacidad máxima de diseño de la batería en mAh

        int battery_percent = 0; // Inicializa variable de porcentaje
        if (capacity > 0)        // Evita división por cero
          battery_percent =
              (charge * 100) /
              capacity;            // Calcula el porcentaje relativo al máximo
        if (battery_percent > 100) // Corrección si supera el 100%
          battery_percent = 100;

        String statusStr = "idle"; // Inicializa cadena de estado en reposo

        if (chargingState > 0 &&
            chargingState < 4) {  // Si el robot está en proceso de carga activa
          statusStr = "charging"; // Establece estado como cargando
          if (chargingState == 3) // Si el estado es carga de mantenimiento
            statusStr = "trickle_charging"; // Define como carga de goteo
        } else if (chargingState ==
                   4) {         // Si el robot está conectado pero cargado
          statusStr = "docked"; // Establece estado como en base
        } else {
          if (current < -500) // Si consume más de 500mA (motores activados)
            statusStr = "cleaning"; // Define estado como limpiando
          else                      // De lo contrario
            statusStr = "idle";     // Permanece en espera
        }

        char jsonBuffer[128]; // Buffer para construir la cadena JSON
        snprintf(
            jsonBuffer,
            sizeof(jsonBuffer), // Formatea el JSON con las variables leídas
            "{\"battery_level\": %d, \"voltage\": %d, \"status\": \"%s\", "
            "\"charging\": %s}", // Estructura del objeto JSON
            battery_percent, voltage,
            statusStr.c_str(), // Inserta valores numéricos y texto
            (chargingState > 0 && chargingState < 5)
                ? "true"
                : "false"); // Determina booleano de carga

        client.publish("roomba/estado",
                       jsonBuffer); // Envía el JSON al broker MQTT
      } // Fin del bloque de recepción exitosa de 10 bytes
    } // Fin del temporizador de lectura de sensores
  } // Fin de la condición de conexión MQTT activa
} // Fin del bucle infinito loop()

// wakeUpRoomba: Secuencia de pulsos eléctricos y comandos OI para encender el
// robot
void wakeUpRoomba() {
  Serial.println("Despertando Roomba..."); // Imprime en el log de depuración
  // El robot entra en reposo si no se usa. Para despertarlo, el pin BRC (Device
  // Detect) debe recibir un pulso LOW de al menos 500ms.
  digitalWrite(BRC_PIN, HIGH); // Asegura estado alto inicial
  delay(100);                  // Breve espera de estabilización
  digitalWrite(BRC_PIN, LOW);  // Hala a tierra (0V) para pedir atención
  delay(500);                  // Mantiene el pulso durante medio segundo
  digitalWrite(BRC_PIN, HIGH); // Regresa a estado de reposo (3.3V)
  delay(2000);                 // Espera el arranque del firmware del robot

  // Comandos de inicialización del protocolo Open Interface (OI)
  Serial2.write(128); // OpCode 128: START (Inicia modo OI)
  delay(100);         // Espera de procesamiento
  Serial2.write(131); // OpCode 131: SAFE MODE (Modo seguro activo)
  delay(100);         // Espera de procesamiento

  loadSongs(); // Carga las melodías en la RAM del iRobot
  Serial.println("Roomba en SAFE mode y canciones cargadas."); // Info en log
}

void setup_wifi() {
  delay(10);                  // Pequeña pausa de estabilidad
  Serial.print("WiFi: ");     // Imprime etiqueta en monitor serial
  Serial.println(ssid);       // Imprime el nombre de la red
  WiFi.mode(WIFI_STA);        // Configura el ESP32 como estación (cliente)
  WiFi.begin(ssid, password); // Inicia el proceso de conexión
  int attempts = 0;           // Contador de intentos
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { // Bucle de espera
    delay(500);        // Espera medio segundo
    Serial.print("."); // Imprime un punto por cada intento
    attempts++;        // Incrementa contador
  }
  if (WiFi.status() ==
      WL_CONNECTED) { // Si el estado de WiFi es conectado exitosamente
    Serial.println("\nConectado. IP: "); // Imprime mensaje de confirmación
    Serial.println(
        WiFi.localIP()); // Muestra la dirección IP local del dispositivo
  } else {               // En caso de que no se lograra la conexión
    Serial.println("\nFallo WiFi."); // Informa el error por el terminal serial
  } // Fin de la validación de conexión
} // Fin de la configuración de red WiFi

boolean reconnect() { // Función para reconexión persistente a MQTT
  String clientId = "ESP32Roomba-" +
                    String(random(0xffff), HEX); // Crea un ID aleatorio único
  if (client.connect(
          clientId.c_str(), mqtt_user,
          mqtt_password)) { // Ejecuta intento de conexión con credenciales
    Serial.println(
        "MQTT Conectado"); // Registra éxito en el broker por puerto serial
    client.subscribe(
        topic_commands); // Se asocia al tema de escucha para recibir órdenes
    return true;         // Retorna verdadero indicando conexión establecida
  } // Fin del bloque de éxito
  return false; // Retorna falso indicando que la conexión falló
} // Fin de la función de reconexión

void callback(char *topic, byte *payload, unsigned int length) {
  String message = "";
  // Convierte el payload (array de bytes) en un String legible
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Cmd: ");
  Serial.println(message); // Muestra el mensaje recibido en el monitor USB

  // Actualiza el tiempo del último mensaje para el sistema de seguridad
  lastMsgTime = millis();
  failsafeTriggered = false; // Resetea el flag de seguridad

  if (String(topic) ==
      topic_commands) { // Compara si el tópico es el de comandos
    // --- Comandos START / TURN_ON ---
    if (message == "start" ||
        message == "turn_on") {                 // Opción: Iniciar limpieza
      wakeUpRoomba();                           // Despierta el robot
      delay(1000);                              // Pausa de 1s
      Serial2.write(135);                       // OpCode 135: Inicia Clean
      Serial.println("Action: Start -> Clean"); // Log por USB
    } // Fin comando start
    // --- Comandos STOP / TURN_OFF ---
    else if (message == "stop" ||
             message == "turn_off") { // Opción: Detener robot
      Serial2.write(128);             // OpCode 128: Pone modo Pasivo
      delay(50);                      // Pausa breve
      Serial2.write(131);             // OpCode 131: Pone modo Seguro
      delay(50);                      // Pausa breve
      move(0, 0);                     // Frena motores
      Serial.println(
          "Action: Stop (Robust: Safe Mode + Drive 0)"); // Log por USB
    } // Fin comando stop
    // --- Comandos de Movimiento Manual ---
    else if (message == "forward") {      // Si el comando es hacia adelante
      move(200, 32768);                   // 200mm/s en línea recta
      Serial.println("Action: Forward");  // Log de consola
    } else if (message == "backward") {   // Si el comando es hacia atrás
      move(-200, 32768);                  // Velocidad negativa (atrás)
      Serial.println("Action: Backward"); // Log de consola
    } else if (message == "left") {       // Si el comando es girar izquierda
      move(200, 1);                       // Giro sobre eje izquierdo
      Serial.println("Action: Left");     // Log de consola
    } else if (message == "right") {      // Si el comando es girar derecha
      move(200, -1);                      // Giro sobre eje derecho
      Serial.println("Action: Right");    // Log de consola
    }
    // --- Diagonales (Giro circular) ---
    else if (message == "forward_left") {       // Curva adelante izquierda
      move(200, 300);                           // Velocidad y radio suave
      Serial.println("Action: Forward-Left");   // Log de consola
    } else if (message == "forward_right") {    // Curva adelante derecha
      move(200, -300);                          // Velocidad y radio negativo
      Serial.println("Action: Forward-Right");  // Log de consola
    } else if (message == "backward_left") {    // Curva atrás izquierda
      move(-200, 300);                          // Marcha atrás y radio
      Serial.println("Action: Backward-Left");  // Log de consola
    } else if (message == "backward_right") {   // Curva atrás derecha
      move(-200, -300);                         // Marcha atrás radio negativo
      Serial.println("Action: Backward-Right"); // Log de consola
    }
    // --- Comandos de Entretenimiento ---
    else if (message == "starwars") {
      playSong(0); // Llama a la función para tocar la canción 0
      Serial.println("Action: Playing Star Wars");
    } else if (message == "mario") {
      playSong(1); // Llama a la función para tocar la canción 1
      Serial.println("Action: Playing Mario Bros");
    }
    // --- Comandos de Limpieza / Base ---
    else if (message == "clean") {     // Comando para modo limpieza auto
      wakeUpRoomba();                  // Asegura que el robot esté despierto
      delay(500);                      // Pausa de seguridad
      Serial2.write(135);              // OpCode 135: Orden de limpieza
      Serial.println("Action: Clean"); // Log por USB
    } else if (message == "dock" ||
               message == "return_to_base") { // Comando volver a base
      wakeUpRoomba();                 // Asegura que el robot esté despierto
      delay(500);                     // Pausa de seguridad
      Serial2.write(143);             // OpCode 143: SEEK DOCK
      Serial.println("Action: Dock"); // Log por USB
    } // Fin bloque comandos base
    // --- Comando POWER_DOWN ---
    else if (message == "power_down") { // Si el mensaje es apagar el sistema
      Serial2.write(133); // OpCode 133: POWER DOWN (Pone al robot en
                          // hibernación profunda)
      Serial.println(
          "Action: Power Down"); // Informa la desconexión por consola
    } // Fin del comando power_down
  } // Fin de la validación de tópico de comandos
} // Fin de la función callback de MQTT

// --- Funciones de Control Roomba ---

void move(int velocity, int radius) {
  // Antes de mover, aseguramos que el robot esté en SAFE MODE (131)
  // para que los sensores de choque y abismo estén activos.
  Serial2.write(131);

  // Limitamos la velocidad entre -500 y 500 mm/s según especificación iRobot
  if (velocity > 500)
    velocity = 500;
  if (velocity < -500)
    velocity = -500;

  // OpCode 137: DRIVE (Directiva para mover motores)
  // Requiere 4 bytes de datos: 2 para velocidad y 2 para radio de giro.
  Serial2.write(137);
  Serial2.write((velocity >> 8) & 0xFF); // Byte alto de velocidad
  Serial2.write(velocity & 0xFF);        // Byte bajo de velocidad
  Serial2.write((radius >> 8) &
                0xFF); // Envía los 8 bits más significativos del radio
  Serial2.write(radius &
                0xFF); // Envía los 8 bits menos significativos del radio
} // Fin de la función de movimiento Drive

void playSong(int songNumber) { // Función para ejecutar sonidos almacenados
  // OpCode 141: PLAY (Reproduce una canción que ya esté cargada en memoria)
  Serial2.write(141); // Comando para ordenar reproducción flash
  Serial2.write(
      songNumber); // Valor de la canción (0 a 15 permitidos en iRobot)
} // Fin de la función playSong

void loadSongs() { // Función para pre-cargar melodías
  // OpCode 140: SONG (Define una canción en la memoria)
  // Canción 0: Star Wars (Marcha Imperial)
  Serial2.write(140); // Comando de definición
  Serial2.write(0);   // ID de canción: 0
  Serial2.write(16);  // Longitud: 16 notas

  // Phrase 1 - Compas inicial
  Serial2.write(55);
  Serial2.write(32); // Nota: Sol4 | Duración: 32 ticks
  Serial2.write(55);
  Serial2.write(32); // Nota: Sol4 | Duración: 32 ticks
  Serial2.write(55);
  Serial2.write(32); // Nota: Sol4 | Duración: 32 ticks
  Serial2.write(51);
  Serial2.write(24); // Nota: Mi bemol 4 | Duración: 24 ticks
  Serial2.write(58);
  Serial2.write(8); // Nota: Si bemol 4 | Duración: 8 ticks
  Serial2.write(55);
  Serial2.write(32); // Nota: Sol4 | Duración: 32 ticks
  Serial2.write(51);
  Serial2.write(24); // Nota: Mi bemol 4 | Duración: 24 ticks
  Serial2.write(58);
  Serial2.write(8); // Nota: Si bemol 4 | Duración: 8 ticks
  Serial2.write(55);
  Serial2.write(64); // Nota: Sol4 | Duración: 64 ticks (Nota larga)

  // Phrase 2 - Notas agudas
  Serial2.write(62);
  Serial2.write(32); // Nota: Re5 | Duración: 32 ticks
  Serial2.write(62);
  Serial2.write(32); // Nota: Re5 | Duración: 32 ticks
  Serial2.write(62);
  Serial2.write(32); // Nota: Re5 | Duración: 32 ticks
  Serial2.write(63);
  Serial2.write(24); // Nota: Mi bemol 5 | Duración: 24 ticks
  Serial2.write(58);
  Serial2.write(8); // Nota: Si bemol 4 | Duración: 8 ticks
  Serial2.write(54);
  Serial2.write(32); // Nota: Fa sostenido 4 | Duración: 32 ticks
  Serial2.write(51);
  Serial2.write(24); // Nota: Mi bemol 4 | Duración: 24 ticks

  delay(50); // Pausa para que el robot guarde la canción interiormente

  // Canción 1: Super Mario Bros (Tema principal corto)
  Serial2.write(140); // Comando para definir nueva canción
  Serial2.write(1);   // Asigna el número 1 a esta melodía
  Serial2.write(13);  // Definimos que tendrá 13 pares de datos

  Serial2.write(76);
  Serial2.write(
      18); // Nota: Mi5 (669 Hz) | Duración: 18 ticks (1 tick = 1/64 seg)
  Serial2.write(76);
  Serial2.write(18); // Nota: Mi5 | Duración: 18 ticks
  Serial2.write(76);
  Serial2.write(18); // Nota: Mi5 | Duración: 18 ticks
  Serial2.write(72);
  Serial2.write(18); // Nota: Do5 | Duración: 18 ticks
  Serial2.write(76);
  Serial2.write(18); // Nota: Mi5 | Duración: 18 ticks
  Serial2.write(79);
  Serial2.write(36); // Nota: Sol5 | Duración: 36 ticks (Negra)
  Serial2.write(67);
  Serial2.write(36); // Nota: Sol4 | Duración: 36 ticks (Negra)

  // Notas finales de la intro de Mario
  Serial2.write(72);
  Serial2.write(28); // Nota: Do5 | Duración: 28 ticks
  Serial2.write(67);
  Serial2.write(28); // Nota: Sol4 | Duración: 28 ticks
  Serial2.write(64);
  Serial2.write(28); // Nota: Mi4 | Duración: 28 ticks
  Serial2.write(69);
  Serial2.write(28); // Nota: La4 | Duración: 28 ticks
  Serial2.write(71);
  Serial2.write(28); // Nota: Si4 | Duración: 28 ticks
  Serial2.write(70);
  Serial2.write(18); // Nota: Si bemol 4 | Duración: 18 ticks
}
