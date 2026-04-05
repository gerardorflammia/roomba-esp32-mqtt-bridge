---
trigger: always_on
---

# Reglas de Búsqueda y Contexto del Proyecto

## 1. Fuentes de Información Prioritarias

Al buscar soluciones o generar código, sigue este orden de prioridad jerárquico:

1.  **Fuente Base:** Analiza y adapta el código del repositorio: `https://github.com/thehookup/MQTT-Roomba-ESP01`. Extrae la lógica de comandos OI y adáptala de ESP-01 a ESP32.
2.  **Documentación Oficial:** Busca referencias específicas sobre "Roomba 500 Open Interface Spec" para códigos de opcodes (motores, limpieza, dock, batería).
3.  **Librerías PlatformIO:** Prioriza librerías compatibles con ESP32 disponibles en el registro de PlatformIO para MQTT (ej. `Knolleary PubSubClient`) y manejo de JSON (ej. `ArduinoJson`).

## 2. Variables de Configuración Predefinidas (Hardcoded)

**IMPORTANTE:** Cada vez que generes código que requiera conexión WiFi o MQTT, DEBES utilizar las siguientes credenciales sin preguntar al usuario. No uses placeholders como "tu_ssid" o "tu_password".

- **WiFi SSID:** `MikroTik`
- **WiFi Password:** `12345678`
- **MQTT Broker IP (Home Assistant):** `192.168.254.220`
- **MQTT Port:** `1883` (Estándar, a menos que se indique otro).
- **Topic Base Sugerido:** `roomba/comandos` y `roomba/estado` (para integración con Node-RED).

## 3. Integración con Home Assistant y Node-RED

- El agente debe asumir que la lógica de control compleja reside en Node-RED.
- El ESP32 debe actuar principalmente como un puente (bridge): recibe comandos por MQTT -> los traduce a Serial para el Roomba -> lee sensores del Roomba -> publica estado por MQTT.
- Al sugerir configuraciones YAML para Home Assistant, usa la integración MQTT discovery o sensores manuales que apunten a la IP `192.168.254.220`.

## 4. Restricciones de Hardware (Tesis)

- Microcontrolador: ESP32 (No ESP8266/ESP-01).
- Robot: Roomba Serie 500.
- Comunicación: UART2 o UART1 (Hardware Serial) preferible sobre SoftwareSerial para estabilidad.
