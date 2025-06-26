// ESP32 12-Hour Clock using Stepper Motors, A4988, RTC_DS3231, and Web Interface

#include <WiFi.h>
#include <WebServer.h>
#include <AccelStepper.h>
#include <RTClib.h>

// WiFi Config
const char* ssid = "CLOCK";
const char* password = "12345678";

// RTC
RTC_DS3231 rtc;

// Web Server
WebServer server(80);

// Stepper Config
#define MOTOR_COUNT 4
const int dirPins[MOTOR_COUNT] = {12, 14, 27, 18};
const int stepPins[MOTOR_COUNT] = {13, 15, 25, 5};
const int msPins[3] = {19, 21, 22};

#define MICROSTEPS 16
#define STEPS_PER_REV (200 * MICROSTEPS)
#define DEG_PER_DIGIT 32.72
const float STEPS_PER_DIGIT = (STEPS_PER_REV * DEG_PER_DIGIT) / 360.0;

AccelStepper steppers[MOTOR_COUNT];
TaskHandle_t motorTasks[MOTOR_COUNT];
volatile float targetSteps[MOTOR_COUNT] = {0};
volatile bool moveFlag[MOTOR_COUNT] = {false};

// Utility: Convert time digits to motor targets
void setMotorDigit(uint8_t index, uint8_t digit) {
  if (digit > 10) digit = 10;
  targetSteps[index] = digit * STEPS_PER_DIGIT;
  moveFlag[index] = true;
}

// Update motors for current RTC time
void updateDisplay(const DateTime& dt) {
  uint8_t hour = dt.hour() % 12;
  if (hour == 0) hour = 12;
  setMotorDigit(0, hour / 10);       // H1
  setMotorDigit(1, hour % 10);       // H2
  setMotorDigit(2, dt.minute() / 10); // M1
  setMotorDigit(3, dt.minute() % 10); // M2
}

// Stepper task for each motor
void motorTask(void* param) {
  int idx = (int)param;
  while (true) {
    if (moveFlag[idx]) {
      steppers[idx].moveTo(targetSteps[idx]);
      while (steppers[idx].distanceToGo() != 0) {
        steppers[idx].run();
        vTaskDelay(1);
      }
      moveFlag[idx] = false;
    }
    vTaskDelay(10);
  }
}

// Web Interface HTML
const char* htmlPage = R"rawliteral(
<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><title>ESP32 Clock</title>
<style>body{text-align:center;font-family:sans-serif}.panel{margin:auto;width:300px;padding:20px;border:1px solid #ccc;border-radius:10px}</style></head>
<body><div class='panel'><h2>ESP32 Clock</h2>
<input type='datetime-local' id='timeInput'><br>
<button onclick='setTime()'>Set Time</button>
<h3>Manual Control</h3>
<div>
  <input type='number' id='d0' min='0' max='10'><button onclick='manual(0)'>Set H1</button><br>
  <input type='number' id='d1' min='0' max='10'><button onclick='manual(1)'>Set H2</button><br>
  <input type='number' id='d2' min='0' max='10'><button onclick='manual(2)'>Set M1</button><br>
  <input type='number' id='d3' min='0' max='10'><button onclick='manual(3)'>Set M2</button><br>
</div><p id='status'></p></div>
<script>
function setTime(){
  let t = new Date(document.getElementById('timeInput').value).getTime()/1000;
  fetch('/settime?epoch='+t).then(r=>r.text()).then(t=>document.getElementById('status').innerText=t);
}
function manual(i){
  let val = document.getElementById('d'+i).value;
  fetch('/setdigit?motor='+i+'&digit='+val).then(r=>r.text()).then(t=>document.getElementById('status').innerText=t);
}
</script></body></html>
)rawliteral";

// Web Handlers
void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleSetTime() {
  if (server.hasArg("epoch")) {
    uint32_t ts = server.arg("epoch").toInt();
    rtc.adjust(DateTime(ts));
    updateDisplay(DateTime(ts));
    server.send(200, "text/plain", "Time updated");
  } else {
    server.send(400, "text/plain", "Missing epoch");
  }
}

void handleManualSet() {
  if (server.hasArg("motor") && server.hasArg("digit")) {
    int motor = server.arg("motor").toInt();
    int digit = server.arg("digit").toInt();
    if (motor >= 0 && motor < MOTOR_COUNT && digit >= 0 && digit <= 10) {
      setMotorDigit(motor, digit);
      server.send(200, "text/plain", "Digit set");
    } else {
      server.send(400, "text/plain", "Invalid range");
    }
  } else {
    server.send(400, "text/plain", "Missing args");
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println(WiFi.localIP());

  if (!rtc.begin()) Serial.println("RTC failed");

  // Set microstepping
  for (int i = 0; i < 3; i++) {
    pinMode(msPins[i], OUTPUT);
    digitalWrite(msPins[i], HIGH);
  }

  for (int i = 0; i < MOTOR_COUNT; i++) {
    steppers[i] = AccelStepper(AccelStepper::DRIVER, stepPins[i], dirPins[i]);
    steppers[i].setMaxSpeed(1000);
    steppers[i].setAcceleration(500);
    steppers[i].setCurrentPosition(0);
    xTaskCreatePinnedToCore(motorTask, "MotorTask", 4096, (void*)i, 1, &motorTasks[i], 1);
  }

  // Go to RTC time on boot
  updateDisplay(rtc.now());

  server.on("/", handleRoot);
  server.on("/settime", handleSetTime);
  server.on("/setdigit", handleManualSet);
  server.begin();
}

void loop() {
  server.handleClient();
  static uint8_t lastMinute = 255;
  DateTime now = rtc.now();
  if (now.minute() != lastMinute) {
    lastMinute = now.minute();
    updateDisplay(now);
  }
  delay(100);
}