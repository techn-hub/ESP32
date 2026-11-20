//Board: "ESP32 Dev Module"
/**
 * @file ESP32_WiFi_Watchdog.ino
 *
 * @brief Autonomous Wi-Fi watchdog and modem recovery system.
 *
 * @details
 * This application uses an ESP32-WROOM-32 microcontroller to monitor
 * Wi-Fi connectivity and automatically power-cycle an external
 * Wi-Fi modem/router when a connection failure is detected.
 *
 * The modem power supply is controlled by a relay connected to GPIO 26.
 * The ESP32 remains independently powered during modem reset operations.
 *
 * Wi-Fi credentials can be configured through:
 * - USB Serial interface (115200 baud)
 * - Bluetooth Classic SPP
 *
 * Credentials are stored in the ESP32 non-volatile storage (NVS).
 *
 * The watchdog uses adaptive monitoring intervals:
 * - Level 1: 1 minute
 * - Level 2: 5 minutes
 * - Level 3: 60 minutes
 * - Level 4: 720 minutes
 *
 * After each successful Wi-Fi check, the monitoring interval advances
 * to the next level. Once the maximum level is reached, the system
 * continues monitoring at the same interval.
 *
 * Following a connection failure, the watchdog performs a hardware
 * modem reset and restarts the monitoring sequence from Level 1.
 *
 * @author NS
 * @version 1.0
 * @date 2026
 *
 * @section hardware Hardware Requirements
 * - ESP32-WROOM-32
 * - Relay module with active LOW input
 * - External Wi-Fi modem/router
 *
 * @section software Software Requirements
 * - Arduino IDE
 * - ESP32 Arduino Core
 * - WiFi.h
 * - BluetoothSerial.h
 * - Preferences.h
 *
 * @section limitations Operational Limitations
 * The current implementation checks Wi-Fi association only.
 * Internet connectivity and external server availability are
 * not monitored.
 *
 * Modem recovery uses blocking delays. Serial and Bluetooth
 * commands are temporarily suspended during this operation.
 */

#include <WiFi.h>
#include <BluetoothSerial.h>
#include <Preferences.h>

// =================================================
// HARDWARE CONFIGURATION
// =================================================

/**
 * @brief GPIO connected to the relay control input.
 */
#define RELAY_PIN 26

/**
 * @brief Relay activation logic.
 *
 * true  = Active LOW
 * false = Active HIGH
 */
#define RELAY_ACTIVE_LOW true

// =================================================
// TIMING CONFIGURATION
// =================================================

/**
 * @brief Modem power interruption time in milliseconds.
 */
#define RELAY_OFF_TIME 5000UL

/**
 * @brief Modem startup waiting time in milliseconds.
 */
#define MODEM_BOOT_TIME 90000UL

/**
 * @brief Maximum Wi-Fi connection attempt time in milliseconds.
 */
#define WIFI_CONNECT_TIMEOUT 20000UL

/**
 * @brief Adaptive Wi-Fi monitoring intervals.
 *
 * The interval advances after each successful Wi-Fi check.
 * The last interval remains active until a connection failure.
 */
const unsigned long checkIntervals[] = {
  1UL   * 60UL * 1000UL,       // Level 1: 1 minute
  5UL   * 60UL * 1000UL,       // Level 2: 5 minutes
  60UL  * 60UL * 1000UL,       // Level 3: 60 minutes
  720UL * 60UL * 1000UL        // Level 4: 720 minutes
};

/**
 * @brief Number of available monitoring levels.
 */
const int NUM_INTERVALS = 4;

// =================================================
// GLOBAL OBJECTS
// =================================================

/**
 * @brief Bluetooth Classic serial communication interface.
 */
BluetoothSerial SerialBT;

/**
 * @brief Non-volatile storage manager.
 */
Preferences preferences;

// =================================================
// GLOBAL VARIABLES
// =================================================

/**
 * @brief Configured Wi-Fi network name.
 */
String wifiSSID;

/**
 * @brief Configured Wi-Fi network password.
 */
String wifiPassword;

/**
 * @brief USB Serial input buffer.
 */
String serialBuffer;

/**
 * @brief Bluetooth input buffer.
 */
String bluetoothBuffer;

/**
 * @brief Current monitoring level.
 *
 * Valid range: 0 to NUM_INTERVALS - 1.
 */
int intervalLevel = 0;

/**
 * @brief Timestamp of the next scheduled Wi-Fi check.
 */
unsigned long nextCheck = 0;

/**
 * @brief Indicates whether Wi-Fi credentials are configured.
 */
bool wifiConfigured = false;

// =================================================
// RELAY CONTROL
// =================================================

/**
 * @brief Controls the modem power relay.
 *
 * @param on true to power ON the modem, false to power it OFF.
 *
 * @details
 * The output logic depends on RELAY_ACTIVE_LOW.
 */
void relaySet(bool on) {

  bool output;

  if (RELAY_ACTIVE_LOW)
    output = on ? LOW : HIGH;
  else
    output = on ? HIGH : LOW;

  digitalWrite(RELAY_PIN, output);
}

// =================================================
// COMMUNICATION
// =================================================

/**
 * @brief Sends a message to both communication interfaces.
 *
 * @param msg Message to be transmitted.
 */
void printBoth(const String &msg) {

  Serial.println(msg);
  SerialBT.println(msg);
}

// =================================================
// CREDENTIAL MANAGEMENT
// =================================================

/**
 * @brief Loads Wi-Fi credentials from non-volatile memory.
 *
 * @details
 * Credentials are stored in the NVS namespace "wifi-data".
 * The function sets wifiConfigured according to the availability
 * of a stored SSID.
 */
void loadCredentials() {

  preferences.begin("wifi-data", false);

  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("pass", "");

  preferences.end();

  wifiConfigured = wifiSSID.length() > 0;
}

/**
 * @brief Saves Wi-Fi credentials to non-volatile memory.
 *
 * @param ssid Wi-Fi network name.
 * @param password Wi-Fi network password.
 *
 * @details
 * Credentials remain available after ESP32 power cycles.
 */
void saveCredentials(String ssid, String password) {

  preferences.begin("wifi-data", false);

  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);

  preferences.end();

  wifiSSID = ssid;
  wifiPassword = password;

  wifiConfigured = true;
}

// =================================================
// WIFI CONNECTION MANAGEMENT
// =================================================

/**
 * @brief Attempts to connect the ESP32 to the configured Wi-Fi network.
 *
 * @return true if the Wi-Fi connection is established.
 * @return false if the connection attempt fails or times out.
 *
 * @details
 * The connection attempt is limited by WIFI_CONNECT_TIMEOUT.
 * On success, the assigned IP address is displayed.
 */
bool connectWiFi() {

  if (!wifiConfigured)
    return false;

  printBoth("Connecting to: " + wifiSSID);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  delay(200);

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );

  unsigned long start = millis();

  while (millis() - start < WIFI_CONNECT_TIMEOUT) {

    if (WiFi.status() == WL_CONNECTED) {

      printBoth("WIFI CONNECTED");
      printBoth("IP: " + WiFi.localIP().toString());

      return true;
    }

    delay(250);
  }

  printBoth("WIFI CONNECTION FAILED");

  return false;
}

// =================================================
// MODEM RECOVERY
// =================================================

/**
 * @brief Performs a hardware power-cycle of the Wi-Fi modem.
 *
 * @details
 * Recovery sequence:
 * 1. Disconnect the ESP32 from Wi-Fi.
 * 2. Disable the ESP32 Wi-Fi interface.
 * 3. Switch OFF modem power through the relay.
 * 4. Wait for RELAY_OFF_TIME.
 * 5. Restore modem power.
 * 6. Wait for MODEM_BOOT_TIME.
 * 7. Attempt to reconnect to the configured Wi-Fi network.
 * 8. Restart the monitoring sequence from Level 1.
 *
 * This function uses blocking delays.
 */
void resetModem() {

  printBoth("MODEM RESET STARTED");

  // Disable the ESP32 Wi-Fi interface.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Interrupt the modem power supply.
  relaySet(false);

  printBoth("Modem power OFF");

  delay(RELAY_OFF_TIME);

  // Restore modem power.
  relaySet(true);

  printBoth("Modem power ON");

  // Wait for modem initialization.
  printBoth("Waiting for modem initialization...");

  delay(MODEM_BOOT_TIME);

  // Attempt to restore Wi-Fi connectivity.
  connectWiFi();

  // Restart the monitoring sequence from Level 1.
  intervalLevel = 0;

  nextCheck = millis() + checkIntervals[0];

  printBoth("Next check in 1 minute");
  printBoth("MODEM RESET COMPLETED");
}

// =================================================
// COMMAND PROCESSING
// =================================================

/**
 * @brief Processes commands received through Serial or Bluetooth.
 *
 * @param cmd Complete command string.
 *
 * @details
 * Supported commands:
 * - WIFI:SSID;PASSWORD
 * - STATUS
 * - RESET
 * - HELP
 */
void processCommand(String cmd) {

  cmd.trim();

  // -------------------------------------------------
  // Configure Wi-Fi credentials.
  // -------------------------------------------------

  if (cmd.startsWith("WIFI:")) {

    String data = cmd.substring(5);

    int separator = data.indexOf(';');

    if (separator <= 0) {

      printBoth("ERROR: WIFI:ssid;password");
      return;
    }

    String ssid = data.substring(0, separator);
    String password = data.substring(separator + 1);

    saveCredentials(ssid, password);

    printBoth("Credentials saved");

    if (connectWiFi())
      printBoth("WiFi configuration OK");
    else
      printBoth("WiFi configuration saved, connection failed");

    // Restart monitoring from Level 1.
    intervalLevel = 0;

    nextCheck = millis() + checkIntervals[0];

    return;
  }

  // -------------------------------------------------
  // Display current watchdog status.
  // -------------------------------------------------

  if (cmd == "STATUS") {

    printBoth("------------------------");
    printBoth("ESP32 WIFI WATCHDOG");

    printBoth("SSID: " + wifiSSID);

    printBoth(
      WiFi.status() == WL_CONNECTED ?
      "WIFI: CONNECTED" :
      "WIFI: DISCONNECTED"
    );

    printBoth(
      "Level: " + String(intervalLevel + 1)
    );

    printBoth("------------------------");

    return;
  }

  // -------------------------------------------------
  // Manually restart the modem.
  // -------------------------------------------------

  if (cmd == "RESET") {

    resetModem();
    return;
  }

  // -------------------------------------------------
  // Display available commands.
  // -------------------------------------------------

  if (cmd == "HELP") {

    printBoth("Available commands:");
    printBoth("WIFI:ssid;password");
    printBoth("STATUS");
    printBoth("RESET");
    printBoth("HELP");

    return;
  }

  printBoth("Unknown command");
}

// =================================================
// SERIAL AND BLUETOOTH INPUT HANDLING
// =================================================

/**
 * @brief Reads and processes commands from a communication interface.
 *
 * @param port Reference to the input Stream.
 * @param buffer Dedicated input buffer for the interface.
 *
 * @details
 * Commands are terminated by carriage return or newline.
 * Separate buffers are used for USB Serial and Bluetooth.
 */
void readPort(Stream &port, String &buffer) {

  while (port.available()) {

    char c = port.read();

    if (c == '\n' || c == '\r') {

      if (buffer.length() > 0) {

        processCommand(buffer);
        buffer = "";
      }
    }
    else if (buffer.length() < 200) {

      buffer += c;
    }
  }
}

// =================================================
// INITIALIZATION
// =================================================

/**
 * @brief Initializes the ESP32 watchdog system.
 *
 * @details
 * Initialization sequence:
 * 1. Starts USB serial communication.
 * 2. Initializes the relay output in the OFF state.
 * 3. Powers ON the modem.
 * 4. Starts Bluetooth Classic SPP.
 * 5. Loads stored Wi-Fi credentials.
 * 6. Attempts the initial Wi-Fi connection.
 *
 * If the initial connection fails, the modem recovery
 * sequence is executed automatically.
 */
void setup() {

  // Start USB serial communication.
  Serial.begin(115200);

  // Preload the relay output latch with the OFF level.
  digitalWrite(
    RELAY_PIN,
    RELAY_ACTIVE_LOW ? HIGH : LOW
  );

  // Configure relay GPIO as output.
  pinMode(RELAY_PIN, OUTPUT);

  // Ensure that the modem is powered ON.
  relaySet(true);

  // Start Bluetooth Classic serial communication.
  SerialBT.begin("ESP32-WIFI-WATCHDOG");

  printBoth("ESP32 WIFI WATCHDOG START");

  // Load previously stored network credentials.
  loadCredentials();

  if (wifiConfigured) {

    printBoth("Stored credentials found");

    if (connectWiFi()) {

      intervalLevel = 0;

      nextCheck = millis() + checkIntervals[0];

    }
    else {

      // Attempt automatic modem recovery.
      resetModem();
    }
  }
  else {

    printBoth("WiFi not configured");
    printBoth("Send WIFI:ssid;password");
  }
}

// =================================================
// MAIN EXECUTION LOOP
// =================================================

/**
 * @brief Main watchdog execution loop.
 *
 * @details
 * The loop continuously:
 * - Processes USB serial commands.
 * - Processes Bluetooth commands.
 * - Checks the scheduled monitoring timestamp.
 * - Verifies Wi-Fi connection status.
 * - Advances the monitoring interval after successful checks.
 * - Initiates modem recovery after a connection failure.
 *
 * The millis() function is used for periodic scheduling,
 * avoiding blocking delays during normal monitoring operation.
 */
void loop() {

  // Process USB serial commands.
  readPort(Serial, serialBuffer);

  // Process Bluetooth commands.
  readPort(SerialBT, bluetoothBuffer);

  // Wait until Wi-Fi credentials are configured.
  if (!wifiConfigured)
    return;

  // Check whether the next monitoring event is due.
  if ((long)(millis() - nextCheck) >= 0) {

    printBoth("Checking WiFi...");

    if (WiFi.status() == WL_CONNECTED) {

      // Wi-Fi connection is available.
      printBoth("WiFi OK");

      // Advance to the next monitoring interval.
      if (intervalLevel < NUM_INTERVALS - 1)
        intervalLevel++;

      // Schedule the next monitoring event.
      nextCheck = millis() +
                  checkIntervals[intervalLevel];

      printBoth(
        "Next check: " +
        String(checkIntervals[intervalLevel] / 60000UL) +
        " minutes"
      );
    }
    else {

      // Wi-Fi connection has been lost.
      printBoth("WiFi ERROR");

      // Restart the modem and monitoring sequence.
      resetModem();
    }
  }
}
