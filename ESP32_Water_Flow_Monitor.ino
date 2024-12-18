#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <U8g2lib.h>
#include <HTTPClient.h>         // For HTTP POST requests
#include <ArduinoJson.h>        // For JSON handling
#include <time.h>

// Timezone for UTC+7
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

// Wi-Fi Credentials
const char* ssid = "your AP";
const char* password = "your AP password";

// Laravel API URL

const char* apiUrl = "Your API";
// Device ID
const char* deviceId = "ESP32-WATER-FLOW-1";


// EEPROM Addresses
#define EEPROM_USERNAME_ADDR 0
#define EEPROM_PASSWORD_ADDR 32
#define EEPROM_UNIT_LIMIT_ADDR 64
#define EEPROM_TOTAL_LITERS_ADDR 128
#define EEPROM_MONTHLY_LITERS_ADDR 160

// Struct for Credentials
struct Credentials {
  char username[32];
  char password[32];
};

// Global Variables
Credentials credentials;
float totalLiters = 0.0;
float monthlyLiters = 0.0;
int defaultUnitLimit = 50;
volatile uint32_t pulseCount = 0;
float flowRate = 0.0;
bool relayState = false;
bool isAuthenticated = false;

// Pins
const int flowSensorPin = 4;
const int relayPin = 5;

// Web Server and OLED Display
WebServer server(80);
U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// Calibration factor for flow sensor
const float calibrationFactor = 7.5;

// Function Prototypes
void IRAM_ATTR pulseCounter();
void handleLoginPage();
void handleLogin();
void handleDashboard();
void handleUpdateLimit();
void handleUpdatePassword();
void handleLogout();
void updateWaterFlow();
void updateOLED();
void initializeDefaultCredentials();
void resetCredentialsToDefault();
void saveCredentials(String username, String password);
void setupTime();
float EEPROMReadFloat(int addr);
void EEPROMWriteFloat(int addr, float value);
void handleToggleRelay();
void sendDataToServer(float flowRate, float totalLiters);

void setup() {
  Serial.begin(115200);

  // Initialize OLED
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_8x13_tr);
  
  // Step 1: Display Booting...
  u8g2.drawStr(0, 40, "Booting...");
  u8g2.sendBuffer();
  delay(500); // Small delay for visibility

  // Initialize EEPROM
  u8g2.clearBuffer();
  u8g2.drawStr(0, 40, "Loading EEPROM...");
  u8g2.sendBuffer();
  EEPROM.begin(512);

  defaultUnitLimit = EEPROMReadFloat(EEPROM_UNIT_LIMIT_ADDR);
  if (defaultUnitLimit <= 0 || defaultUnitLimit > 10000) {
    defaultUnitLimit = 50;
    EEPROMWriteFloat(EEPROM_UNIT_LIMIT_ADDR, defaultUnitLimit);
  }

  totalLiters = EEPROMReadFloat(EEPROM_TOTAL_LITERS_ADDR);
  monthlyLiters = EEPROMReadFloat(EEPROM_MONTHLY_LITERS_ADDR);

  // Step 2: Display Connecting to Wi-Fi
  u8g2.clearBuffer();
  u8g2.drawStr(0, 40, "Connecting Wi-Fi...");
  u8g2.sendBuffer();

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi connected. IP address: ");
  Serial.println(WiFi.localIP());

  // Step 3: Wi-Fi Connected
  u8g2.clearBuffer();
  u8g2.drawStr(0, 40, "Wi-Fi Connected!");
  u8g2.drawStr(0, 60, WiFi.localIP().toString().c_str()); // Display IP Address
  u8g2.sendBuffer();
  delay(1000);

  // Step 4: Synchronize Time
  u8g2.clearBuffer();
  u8g2.drawStr(0, 40, "Setting up Time...");
  u8g2.sendBuffer();

  setupTime();

  u8g2.clearBuffer();
  u8g2.drawStr(0, 40, "Time Synchronized!");
  u8g2.sendBuffer();
  delay(1000);

  // Step 5: Initialize default credentials
  initializeDefaultCredentials();

  // Step 6: Ready
  u8g2.clearBuffer();
  u8g2.drawStr(0, 40, "System Ready!");
  u8g2.sendBuffer();
  delay(1000);

  // Setup Flow Sensor and Relay
  pinMode(flowSensorPin, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

  // Start Web Server
  server.on("/", handleLoginPage);
  server.on("/login", handleLogin);
  server.on("/dashboard", handleDashboard);
  server.on("/update_limit", handleUpdateLimit);
  server.on("/update_password", handleUpdatePassword);
  server.on("/logout", handleLogout);
  server.on("/toggle_relay", handleToggleRelay);

  server.begin();

  // Final Ready Message
  u8g2.clearBuffer();
  u8g2.drawStr(0, 40, "Web Server Ready!");
  u8g2.sendBuffer();
  Serial.println("Web server started.");
}


void loop() {
  server.handleClient();
  updateWaterFlow();
  updateOLED();

  static unsigned long previousTime = 0;
  unsigned long currentTime = millis();

  if (currentTime - previousTime >= 5000) { // Every 5 seconds
    previousTime = currentTime;

    // Calculate flow rate (example calculation)
    flowRate = (pulseCount / 7.5); // Adjust calibration factor as needed
    pulseCount = 0;

    totalLiters += flowRate / 60.0;

    sendDataToServer(flowRate, totalLiters);
  }

}

void sendDataToServer(float flowRate, float totalLiters) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    // JSON payload
    StaticJsonDocument<200> jsonDoc;
    jsonDoc["device_id"] = deviceId;
    jsonDoc["flow_rate"] = flowRate;
    jsonDoc["total_liters"] = totalLiters;

    // Get current time
    struct tm timeInfo;
    getLocalTime(&timeInfo);
    char timeStr[30];
    sprintf(timeStr, "%04d-%02d-%02d %02d:%02d:%02d",
            timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
            timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    jsonDoc["timestamp"] = timeStr;

    String requestBody;
    serializeJson(jsonDoc, requestBody);

    // Send HTTP POST
    http.begin(apiUrl);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(requestBody);
    if (httpResponseCode > 0) {
      Serial.println("Data sent successfully.");
    } else {
      Serial.print("Error sending data. HTTP Response code: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  }
}

// Pulse Counter ISR
void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

// Synchronize NTP Time
void setupTime() {
  Serial.println("Initializing NTP time synchronization...");
  configTime(gmtOffset_sec, daylightOffset_sec, "1.th.pool.ntp.org", "time.nist.gov", "pool.ntp.org");

  struct tm timeInfo;
  int retryCount = 0;
  const int maxRetries = 5; // Set a maximum number of retries

  while (!getLocalTime(&timeInfo) && retryCount < maxRetries) {
    Serial.println("Waiting for NTP time...");
    delay(1000); // Reduce the delay to 1 second
    retryCount++;
  }

  if (retryCount >= maxRetries) {
    Serial.println("Failed to synchronize time with NTP server.");
  } else {
    Serial.println("Time synchronized successfully!");
    Serial.printf("Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeInfo.tm_year + 1900,
                  timeInfo.tm_mon + 1,
                  timeInfo.tm_mday,
                  timeInfo.tm_hour,
                  timeInfo.tm_min,
                  timeInfo.tm_sec);
  }
}


// Initialize default credentials
void initializeDefaultCredentials() {
  EEPROM.get(EEPROM_USERNAME_ADDR, credentials);

  // Check if EEPROM contains uninitialized data
  bool uninitialized = true;
  for (int i = 0; i < sizeof(credentials.username); i++) {
    if (credentials.username[i] != 0xFF) {
      uninitialized = false;
      break;
    }
  }

  if (uninitialized) {
    Serial.println("EEPROM is uninitialized. Setting default credentials.");
    resetCredentialsToDefault();
  } else {
    Serial.println("Credentials found in EEPROM.");
    Serial.print("Username: ");
    Serial.println(credentials.username);
    Serial.print("Password: ");
    Serial.println(credentials.password);
  }
}

// Reset credentials to default
void resetCredentialsToDefault() {
  strncpy(credentials.username, "admin", sizeof(credentials.username));
  strncpy(credentials.password, "1234", sizeof(credentials.password));
  EEPROM.put(EEPROM_USERNAME_ADDR, credentials);
  EEPROM.commit();
  Serial.println("Credentials reset to default.");
}

// Save updated credentials
void saveCredentials(String username, String password) {
  strncpy(credentials.username, username.c_str(), sizeof(credentials.username));
  strncpy(credentials.password, password.c_str(), sizeof(credentials.password));
  EEPROM.put(EEPROM_USERNAME_ADDR, credentials);
  EEPROM.commit();
  Serial.println("Credentials updated.");
}

// Update water flow data
void updateWaterFlow() {
  static unsigned long previousTime = 0;
  unsigned long currentTime = millis();
  if (currentTime - previousTime >= 1000) { // Update every second
    previousTime = currentTime;

    // Calculate flow rate and usage
    flowRate = (pulseCount / calibrationFactor) / (1.0 / 60.0); // L/min
    totalLiters += (flowRate / 60.0);
    monthlyLiters += (flowRate / 60.0);

    // Reset pulse count
    pulseCount = 0;

    // Save water usage to EEPROM every 10 seconds
    static unsigned long saveTime = 0;
    if (currentTime - saveTime >= 10000) {
      saveTime = currentTime;
      EEPROMWriteFloat(EEPROM_TOTAL_LITERS_ADDR, totalLiters);
      EEPROMWriteFloat(EEPROM_MONTHLY_LITERS_ADDR, monthlyLiters);
      Serial.println("Water usage saved to EEPROM.");
    }
  }
}

// Update OLED display
void updateOLED() {
  u8g2.clearBuffer();

 // Buffer to store the combined string
char ipAddress[50]; // Adjust size based on expected string length
sprintf(ipAddress, "IP:%s", WiFi.localIP().toString().c_str());

// Set font and display the IP address on OLED
u8g2.setFont(u8g2_font_8x13_tr);
u8g2.drawStr(0, 10, ipAddress);

  u8g2.setFont(u8g2_font_6x10_tr);
  // Get current time
  struct tm timeInfo;
  if (getLocalTime(&timeInfo)) {
    char dateTime[30];
    sprintf(dateTime, "%02d-%02d-%d %02d:%02d:%02d",
            timeInfo.tm_mday, timeInfo.tm_mon + 1, timeInfo.tm_year + 1900,
            timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    u8g2.drawStr(0, 25, dateTime); // Show date and time
  } else {
    u8g2.drawStr(0, 25, "Time Sync Error");
  }

  // Display water flow and usage
  char buffer[20];
  sprintf(buffer, "Flow: %.2f L/min", flowRate);
  u8g2.drawStr(0, 45, buffer);

  sprintf(buffer, "Total: %.2f L", totalLiters);
  u8g2.drawStr(0, 60, buffer);

  sprintf(buffer, "Month: %.2f L", monthlyLiters);
  u8g2.drawStr(0, 75, buffer);

  sprintf(buffer, "Relay: %s", relayState ? "ON" : "OFF");
  u8g2.drawStr(0, 90, buffer);

  u8g2.sendBuffer();
}

// EEPROM read/write helpers
float EEPROMReadFloat(int addr) {
  float value;
  EEPROM.get(addr, value);
  return value;
}

void EEPROMWriteFloat(int addr, float value) {
  EEPROM.put(addr, value);
  EEPROM.commit();
}

// Web server handlers
// Function to display the login page with enhanced styling
void handleLoginPage() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Login</title>
      <style>
        body {
          font-family: Arial, sans-serif;
          background-color: #f0f0f5;
          margin: 0;
          display: flex;
          justify-content: center;
          align-items: center;
          height: 100vh;
        }
        .login-container {
          width: 300px;
          padding: 20px;
          background-color: #fff;
          box-shadow: 0 0 10px rgba(0, 0, 0, 0.2);
          border-radius: 8px;
          text-align: center;
        }
        h2 {
          color: #333;
        }
        input {
          width: 100%;
          padding: 10px;
          margin: 10px 0;
          border: 1px solid #ccc;
          border-radius: 5px;
        }
        button {
          background-color: #007BFF;
          color: white;
          border: none;
          padding: 10px 15px;
          cursor: pointer;
          border-radius: 5px;
          width: 100%;
        }
        button:hover {
          background-color: #0056b3;
        }
      </style>
    </head>
    <body>
      <div class="login-container">
        <h2>Login</h2>
        <form action="/login" method="POST">
          <input type="text" name="username" placeholder="Username" required>
          <input type="password" name="password" placeholder="Password" required>
          <button type="submit">Login</button>
        </form>
      </div>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleLogin() {
  String username = server.arg("username");
  String password = server.arg("password");

  if (username == String(credentials.username) && password == String(credentials.password)) {
    isAuthenticated = true;
    server.sendHeader("Location", "/dashboard");
    server.send(302);
  } else {
    server.send(401, "text/html", "<h3>Invalid Credentials</h3><a href='/'>Try Again</a>");
  }
}

// Function to display the dashboard page with enhanced styling and data
void handleDashboard() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }

  // Generate HTML
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Dashboard</title>
      <style>
        body {
          font-family: Arial, sans-serif;
          background-color: #f0f0f5;
          margin: 0;
          display: flex;
          justify-content: center;
          align-items: center;
          height: 100vh;
        }
        .dashboard-container {
          width: 400px;
          padding: 20px;
          background-color: #fff;
          box-shadow: 0 0 10px rgba(0, 0, 0, 0.2);
          border-radius: 8px;
          text-align: center;
        }
        h2 {
          color: #007BFF;
        }
        p {
          font-size: 18px;
          color: #333;
        }
        .form-container {
          margin: 15px 0;
        }
        input, button {
          padding: 10px;
          margin: 5px;
          border-radius: 5px;
          border: 1px solid #ccc;
          font-size: 16px;
          width: 90%;
        }
        button {
          background-color: #28a745;
          color: white;
          cursor: pointer;
          border: none;
        }
        button:hover {
          background-color: #218838;
        }
        .button-danger {
          background-color: #dc3545;
        }
        .button-danger:hover {
          background-color: #c82333;
        }
        .button-primary {
          background-color: #3498db;
        }
        .button-primary:hover {
          background-color: #3498db;
        }
        .button-warning {
          background-color: #f4d03f;
        }
         .button-warning:hover {
          background-color: #f4d03f;
        }

      </style>
    </head>
    <body>
      <div class="dashboard-container">
        <h2>Water Flow Dashboard</h2>

        <!-- Real-time Date and Time Display -->
        <p><strong>Current Date & Time:</strong></p>
        <p id="realtime-clock">Loading...</p>

        <!-- Flow Rate and Usage Display -->
        <div class="form-container">
          <p><strong>Flow Rate:</strong> )rawliteral" + String(flowRate, 2) + R"rawliteral( L/min</p>
          <p><strong>Total Water Usage:</strong> )rawliteral" + String(totalLiters, 2) + R"rawliteral( L</p>
          <p><strong>Monthly Usage:</strong> )rawliteral" + String(monthlyLiters, 2) + R"rawliteral( L</p>
        </div>

        <!-- Update Monthly Unit Limit Form -->
        <div class="form-container">
          <form action="/update_limit" method="POST">
            <h3>Update Monthly Unit Limit</h3>
            Current Limit: )rawliteral" + String(defaultUnitLimit) + R"rawliteral(L<br>
            <input type="number" name="unit_limit" placeholder="Enter new limit">
            <button type="submit">Update</button>
          </form>
        </div>

        <!-- Relay Control Form -->
        <div class="form-container">
          <h3>Relay Control</h3>
          <form action="/toggle_relay" method="POST">
  <button type="submit" class="button-primary )rawliteral"
    + (relayState ? "btn-off" : "") + R"rawliteral(">
    )rawliteral" + (relayState ? "Turn ON" : "Turn OFF") + R"rawliteral(
  </button>
</form>

          </form>
        </div>

        <!-- Logout Button -->
        <div class="form-container">
          <form action="/logout" method="POST">
            <button type="submit" class="button-danger">Logout</button>
          </form>
        </div>
      </div>

      <!-- JavaScript for Real-Time Clock -->
      <script>
        function updateClock() {
          const now = new Date();
          const options = {
            year: 'numeric', month: '2-digit', day: '2-digit',
            hour: '2-digit', minute: '2-digit', second: '2-digit'
          };
          document.getElementById("realtime-clock").innerText = now.toLocaleString('en-US', options);
        }
        setInterval(updateClock, 1000); // Update clock every second
        updateClock(); // Initial call
      </script>
    </body>
    </html>
  )rawliteral";

  server.send(200, "text/html", html);
}


void handleUpdateLimit() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }

  int newLimit = server.arg("unit_limit").toInt();
  if (newLimit > 0) {
    defaultUnitLimit = newLimit;
    EEPROMWriteFloat(EEPROM_UNIT_LIMIT_ADDR, defaultUnitLimit); // Save updated limit to EEPROM
    Serial.print("Updated Unit Limit: ");
    Serial.println(defaultUnitLimit);
  }

  server.sendHeader("Location", "/dashboard");
  server.send(302);
}

void handleToggleRelay() {
  // Toggle the relay state
  relayState = !relayState;

  // Update the relay output pin
  digitalWrite(relayPin, relayState ? HIGH : LOW);

  Serial.print("Relay is now: ");
  Serial.println(relayState ? "ON" : "OFF");

  // Redirect back to the dashboard
  server.sendHeader("Location", "/dashboard");
  server.send(302);
}


void handleUpdatePassword() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }

  String newUsername = server.arg("new_username");
  String newPassword = server.arg("new_password");

  if (!newUsername.isEmpty() && !newPassword.isEmpty()) {
    saveCredentials(newUsername, newPassword);
    Serial.println("Updated credentials.");
  }

  server.sendHeader("Location", "/dashboard");
  server.send(302);
}


void handleLogout() {
  isAuthenticated = false;
  server.sendHeader("Location", "/");
  server.send(302);
}
