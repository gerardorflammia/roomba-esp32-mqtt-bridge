---
trigger: always_on
---

# Reglas de Hardware y Seguridad Crítica

## 1. Gestión del Pin "Device Detect" (Baud Rate Change)

- **Regla:** El Roomba entra en modo suspensión si no detecta actividad. Para despertarlo, el ESP32 debe controlar el pin "Device Detect" (Pin 5 del puerto Mini-DIN).
- **Acción:** Siempre que generes código de inicialización, incluye una función `wakeUpRoomba()` que ponga el pin conectado a Device Detect en LOW por 500ms y luego en HIGH.

## 2. Protección de Voltaje (Nivel Lógico)

- **Advertencia Constante:** Recuerda siempre al usuario que el TX del Roomba envía señales de 5V, lo cual puede dañar el pin RX del ESP32 (que es 3.3V).
- **Acción:** Sugiere un divisor de voltaje (dos resistencias) o un logic level converter en los diagramas o explicaciones de conexión.

## 3. Fail-Safe (Seguridad ante desconexión)

- **Lógica:** Si el ESP32 pierde conexión con el broker MQTT o el WiFi, el robot NO debe seguir ejecutando el último comando de movimiento (podría chocar o caer).
- **Código:** Implementa un `watchdog` de software. Si no se recibe un mensaje MQTT en 'X' segundos, el ESP32 debe enviar el comando `OpCode 173` (Stop) al Roomba automáticamente.

## 4. Actualizaciones OTA (Over-The-Air)

- **Contexto:** Al ser un robot móvil, conectar el cable USB para reprogramar es tedioso.
- **Requisito:** Incluye siempre la librería `ArduinoOTA` en el `main.cpp` y configúrala para permitir cargar código nuevo vía WiFi sin desmontar el robot.
