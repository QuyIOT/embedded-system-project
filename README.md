RFID Card Management System with ESP32
Project Description
This project uses an ESP32 microcontroller to create a web-based interface in Access Point mode, allowing users to manage RFID cards through a browser. RFID cards can be registered, assigned a label, and deleted via the web interface. All data is stored in the ESP32’s flash memory, ensuring persistence after reboot.

Main Features
ESP32 runs in Access Point mode and hosts a web server.

RFID card UID management:

Register new cards.

Assign a name to each UID.

Delete existing UIDs.

Data is saved using SPIFFS to persist after power loss.

MFRC522 library is used for RFID communication.

Hardware Requirements
ESP32

RC522 RFID Module

RFID Tags or Cards

Jumper wires

Hardware Connections

| RC522 Pin | ESP32 Pin |
| --------- | --------- |
| SDA       | GPIO 5    |
| SCK       | GPIO 18   |
| MOSI      | GPIO 23   |
| MISO      | GPIO 19   |
| RST       | GPIO 22   |
| GND       | GND       |
| 3.3V      | 3.3V      |

How to Use
Flash the program to ESP32 using Arduino IDE or PlatformIO.

ESP32 creates a WiFi network with a predefined SSID and password.

Connect your device (phone/laptop) to the WiFi network.

Open a web browser and navigate to the default IP, usually 192.168.4.1.

From the web interface:

Scan an RFID card and assign a name to store its UID.

View the list of registered and unregistered cards.

Delete cards from memory if needed.

Project Structure

/project_root
├── main/
│   ├── app_main.c         # Main system initialization
│   ├── web_server.c       # Web server setup and request handling
│   ├── rfid_handler.c     # RFID module communication
│   ├── storage.c          # Flash storage handling
├── spiffs/
│   ├── index.html         # Web-based user interface
├── sdkconfig              # ESP-IDF configuration file

Libraries and Tools
ESP-IDF 4.x

MFRC522

SPIFFS

ESP Async WebServer (if using Arduino)

Notes
If you modify the HTML or add new pages, use spiffs.py to build the SPIFFS filesystem and flash it to ESP32.

In Access Point mode, ESP32 won’t have Internet access.

Authors
Name: Quy Bui Phu

Email: quydt23@gmail.com
