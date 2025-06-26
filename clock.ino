#include <WiFi.h>
#include <RTClib.h>
#include <AccelStepper.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <WiFiClient.h>

// Configuration
#define MOTOR_COUNT 4
const char* ssid = "CLOCK";
const char* password = "12345678";

// Pins
const int dirPins[MOTOR_COUNT] = {12, 14, 27, 18};
const int stepPins[MOTOR_COUNT] = {13, 15, 25, 5};
const int msPins[3] = {19, 21, 22}; // MS1-MS3

// Motor Params (1/16 microstepping)
const int MICROSTEPS = 16;
const int STEPS_PER_REV = 200 * MICROSTEPS;
const float DEGREES_PER_DIGIT = 32.72;
const float STEPS_PER_DIGIT = (STEPS_PER_REV * DEGREES_PER_DIGIT) / 360.0;

// Components
RTC_DS3231 rtc;
WebServer server(80);
AccelStepper motors[MOTOR_COUNT];
uint32_t lastSavedTime = 0;

TaskHandle_t motorTasks[MOTOR_COUNT];
int motorTargets[MOTOR_COUNT] = {0};
bool motorMoveFlags[MOTOR_COUNT] = {false};

// HTML Interface
const char* htmlContent = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Clock Control</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; }
    .control-panel { margin: 20px auto; width: 300px; padding: 20px; border: 1px solid #ccc; border-radius: 10px; }
    input, button { padding: 10px; margin: 5px; width: 90%%; }
    button { background: #4CAF50; color: white; border: none; cursor: pointer; }
    .status { margin-top: 20px; padding: 10px; background: #f8f8f8; border-radius: 5px; }
  </style>
</head>
<body>
  <div class="control-panel">
    <h2>ESP32 Clock</h2>
    <input type="datetime-local" id="timeInput">
    <button onclick="setTime()">Set Time</button>
    <button onclick="syncTime()">Sync Network Time</button>
    <div class="status" id="status">Current Time: Loading...</div>
  </div>
  <script>
    function setTime() {
      const timeInput = document.getElementById('timeInput').value;
      const timestamp = new Date(timeInput).getTime() / 1000;
      fetch(`/settime?epoch=${timestamp}`)
        .then(r => r.text()).then(t => updateStatus(t));
    }
    function syncTime() {
      fetch('/synctime').then(r => r.text()).then(updateStatus);
    }
    function updateStatus(t) {
      document.getElementById('status').innerText = t;
      setTimeout(loadTime, 1000);
    }
    function loadTime() {
      fetch('/gettime').then(r => r.text()).then(t => {
        document.getElementById('status').innerText = `Current Time: ${t}`;
        document.getElementById('timeInput').value = new Date(t).toISOString().slice(0,16);
      });
    }
    setInterval(loadTime, 30000);
    loadTime();
  </script>
</body>
</html>
)rawliteral";

void saveState(uint32_t epoch) {
  EEPROM.writeULong(0, epoch);
  EEPROM.commit();
}

uint32_t loadState() {
  return EEPROM.readULong(0);
}

void moveToDigit(uint8_t motor, uint8_t digit) {
  motorTargets[motor] = digit;
  motorMoveFlags[motor] = true;
}

void updateDisplay(const DateTime& dt) {
  uint8_t digits[4] = {
    (dt.hour()%12 ? dt.hour()%12 : 12) / 10,
    (dt.hour()%12 ? dt.hour()%12 : 12) % 10,
    dt.minute() / 10,
    dt.minute() % 10
  };
  for(int i = 0; i < MOTOR_COUNT; i++) {
    moveToDigit(i, digits[i]);
  }
  saveState(dt.unixtime());
}

void motorTask(void* param) {
  int motorIndex = (int)param;
  while (true) {
    if (motorMoveFlags[motorIndex]) {
      long target = motorTargets[motorIndex] * STEPS_PER_DIGIT;
      motors[motorIndex].moveTo(target);
      while (motors[motorIndex].distanceToGo() != 0) {
        motors[motorIndex].run();
        vTaskDelay(1);
      }
      motorMoveFlags[motorIndex] = false;
    }
    vTaskDelay(10);
  }
}

void handleRoot() {
  server.send(200, "text/html", htmlContent);
}

void handleSetTime() {
  if(server.hasArg("epoch")) {
    uint32_t epoch = server.arg("epoch").toInt();
    rtc.adjust(DateTime(epoch));
    updateDisplay(DateTime(epoch));
    server.send(200, "text/plain", "Time set successfully!");
  } else {
    server.send(400, "text/plain", "Missing epoch parameter");
  }
}

void handleSyncTime() {
  WiFiClient client;
  if(client.connect("worldtimeapi.org", 80)) {
    client.print("GET /api/timezone/Etc/UTC HTTP/1.1\r\nHost: worldtimeapi.org\r\nConnection: close\r\n\r\n");
    delay(500);
    String response = client.readString();
    int idx = response.indexOf("unixtime");
    if (idx != -1) {
      int colon = response.indexOf(":", idx);
      int comma = response.indexOf(",", colon);
      String timeStr = response.substring(colon + 1, comma);
      uint32_t ntpTime = timeStr.toInt();
      rtc.adjust(DateTime(ntpTime));
      updateDisplay(DateTime(ntpTime));
      server.send(200, "text/plain", "Time synced from NTP!");
    } else {
      server.send(500, "text/plain", "Invalid response from NTP server");
    }
  } else {
    server.send(500, "text/plain", "NTP sync failed");
  }
}

void handleGetTime() {
  DateTime now = rtc.now();
  server.send(200, "text/plain", 
    String(now.year()) + "-" + 
    String(now.month()) + "-" + 
    String(now.day()) + " " + 
    String(now.hour()) + ":" + 
    String(now.minute()) + ":" + 
    String(now.second()));
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);

  for(int i = 0; i < 3; i++) {
    pinMode(msPins[i], OUTPUT);
    digitalWrite(msPins[i], HIGH);
  }

  for(int i = 0; i < MOTOR_COUNT; i++) {
    motors[i] = AccelStepper(AccelStepper::DRIVER, stepPins[i], dirPins[i]);
    motors[i].setMaxSpeed(1000 * MICROSTEPS);
    motors[i].setAcceleration(500 * MICROSTEPS);
  }

  if(!rtc.begin()) Serial.println("RTC Error");

  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("IP: " + WiFi.localIP().toString());

  uint32_t storedTime = loadState();
  if(storedTime > 0) {
    uint32_t currentTime = rtc.now().unixtime();
    if(currentTime - storedTime < 86400) {
      updateDisplay(DateTime(storedTime + (currentTime - storedTime)));
    }
  }

  server.on("/", handleRoot);
  server.on("/settime", handleSetTime);
  server.on("/synctime", handleSyncTime);
  server.on("/gettime", handleGetTime);
  server.begin();

  for (int i = 0; i < MOTOR_COUNT; i++) {
    xTaskCreatePinnedToCore(
      motorTask,
      "MotorTask",
      4096,
      (void*)i,
      1,
      &motorTasks[i],
      1
    );
  }
}

void loop() {
  static uint8_t lastMinute = 255;
  DateTime now = rtc.now();

  if(now.minute() != lastMinute) {
    lastMinute = now.minute();
    updateDisplay(now);
  }

  server.handleClient();
  delay(100);
}