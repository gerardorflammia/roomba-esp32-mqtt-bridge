# Roomba Open Interface Manuals

This folder contains the official iRobot documentation for the Roomba 500 Series:

- **Roomba_500_Series_Manual.pdf** — General user manual covering usage, maintenance, and troubleshooting
- **Roomba_SCI_Spec_Manual.pdf** — Serial Command Interface specification with all opcodes, sensor packet IDs, and protocol details

These documents are essential references for anyone wanting to extend the firmware with additional Roomba commands or sensor readings.

## Key Opcodes Used in This Project

| Opcode | Command | Description |
|---|---|---|
| 128 | Start | Enter Passive mode, begin OI communication |
| 131 | Safe | Enter Safe mode (motors controllable, safety sensors active) |
| 133 | Power | Power down the Roomba |
| 135 | Clean | Start a cleaning cycle |
| 137 | Drive | Control wheel motors (velocity + radius) |
| 140 | Song | Define a song (up to 16 notes) |
| 141 | Play | Play a previously defined song |
| 142 | Sensors | Request sensor data packet |
| 143 | Dock | Send Roomba to find its charging base |
| 173 | Stop | Stop all motors immediately |
