#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <RTClib.h>

const char* ssid = "********";
const char* pass = "********";
IPAddress local_IP(10,148,142,126);
IPAddress gateway(10,148,142,1);
IPAddress subnet(255,255,255,0);

ESP8266WebServer server(80);
RTC_DS3231 rtc;

// Pins
#define TOUCH_PIN D1
#define LED_BREAKFAST D5
#define LED_LUNCH D6
#define LED_DINNER D7
unsigned long lastRtcPrint = 0;

int pillCount[3] = {6, 6, 6};   // breakfast, lunch, dinner
int alarmTimes[3] = {0, 0, 0};  // HHMM

bool doseActive = false;
bool firstTouchDetected = false;
bool pillTaken = false;
unsigned long touchStartTime = 0;
int currentDose = 0; // 1,2,3
bool doseTakenFlag = false;
bool doseMissedFlag = false;
unsigned long doseStartTime = 0;


// Prevent retrigger in same minute
int lastTriggeredMinute = -1;

void setup() {
  Serial.begin(9600);
  Wire.begin(D2, D3);
  rtc.begin();

  pinMode(TOUCH_PIN, INPUT);
  pinMode(LED_BREAKFAST, OUTPUT);
  pinMode(LED_LUNCH, OUTPUT);
  pinMode(LED_DINNER, OUTPUT);

  digitalWrite(LED_BREAKFAST, LOW);
  digitalWrite(LED_LUNCH, LOW);
  digitalWrite(LED_DINNER, LOW);

  Serial.println("Starting WiFi connection...");
WiFi.mode(WIFI_STA);
WiFi.config(local_IP, gateway, subnet);
WiFi.begin(ssid, pass);

unsigned long startAttemptTime = millis();

while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000) {
  delay(500);
  Serial.print(".");
}

Serial.println();

if (WiFi.status() == WL_CONNECTED) {
  Serial.println("WiFi connected");
  Serial.print("ESP IP: ");
  Serial.println(WiFi.localIP());
} else {
  Serial.println("WiFi FAILED to connect");
}

  Serial.println("\nWiFi connected");
  Serial.print("ESP IP: ");
  Serial.println(WiFi.localIP());

  // API: dose taken status
server.on("/status", []() {
  server.send(200, "text/plain", doseTakenFlag ? "1" : "0");
});


server.on("/dose", []() {
  server.send(200, "text/plain", String(currentDose));
});

  // API: pill counts
  server.on("/pill", []() {
    String res = String(pillCount[0]) + "," + String(pillCount[1]) + "," + String(pillCount[2]);
    server.send(200, "text/plain", res);
  });

  // API: set alarm time
server.on("/settime", []() {
  if (server.hasArg("slot") && server.hasArg("h") && server.hasArg("m")) {
    int slot = server.arg("slot").toInt(); // 0=morning,1=noon,2=night
    int h = server.arg("h").toInt();
    int m = server.arg("m").toInt();

    alarmTimes[slot] = h * 100 + m;

    Serial.print("SETTIME received -> slot: ");
    Serial.print(slot);
    Serial.print(" time: ");
    Serial.print(h);
    Serial.print(":");
    Serial.println(m);

    server.send(200, "text/plain", "OK");
  } else {
    Serial.println("SETTIME called with BAD params");
    server.send(400, "text/plain", "BAD");
  }
});
server.on("/missed", []() {
  server.send(200, "text/plain", doseMissedFlag ? "1" : "0");
  doseMissedFlag = false;  // reset after read
});

  // TEMP TEST ENDPOINT (remove later if you want)
  server.on("/testdose", []() {
    doseTakenFlag = true;
    server.send(200, "text/plain", "OK");
  });
server.on("/clear", []() {
  doseTakenFlag = false;
  doseMissedFlag = false;
  currentDose = 0;
  server.send(200, "text/plain", "OK");
});



  server.begin();
}

void loop() {
  server.handleClient();
  checkAlarm();
  checkTouch();
  if (millis() - lastRtcPrint > 5000) {  // every 5 seconds
  lastRtcPrint = millis();
  DateTime now = rtc.now();
  Serial.print("RTC: ");
  Serial.print(now.hour());
  Serial.print(":");
  if (now.minute() < 10) Serial.print("0");
  Serial.print(now.minute());
  Serial.print(":");
  if (now.second() < 10) Serial.print("0");
  Serial.println(now.second());
}

}

void checkAlarm() {
  DateTime now = rtc.now();
  int currentTime = now.hour() * 100 + now.minute();

  static int lastMinute = -1;
  if (now.minute() == lastMinute) return;
  lastMinute = now.minute();

  Serial.print("CHECK ");
  Serial.print(currentTime);
  Serial.print(" | alarms: ");
  Serial.print(alarmTimes[0]); Serial.print(",");
  Serial.print(alarmTimes[1]); Serial.print(",");
  Serial.println(alarmTimes[2]);

  if (!doseActive) {
    for (int i = 0; i < 3; i++) {
      if (alarmTimes[i] != 0 && currentTime == alarmTimes[i]) {
        Serial.print("MATCH slot ");
        Serial.println(i + 1);
        activateDose(i + 1);
        break;
      }
    }
  }
}


void activateDose(int dose) {
  Serial.print("Activating dose: ");
  Serial.println(dose);

  currentDose = dose;
  doseActive = true;
  firstTouchDetected = false;
  pillTaken = false;
  doseTakenFlag = false;
  doseMissedFlag = false;
  doseStartTime = millis();

  digitalWrite(LED_BREAKFAST, LOW);
  digitalWrite(LED_LUNCH, LOW);
  digitalWrite(LED_DINNER, LOW);

  if (dose == 1) digitalWrite(LED_BREAKFAST, HIGH);
  if (dose == 2) digitalWrite(LED_LUNCH, HIGH);
  if (dose == 3) digitalWrite(LED_DINNER, HIGH);
}


void checkTouch() {
  if (!doseActive) return;

  if (digitalRead(TOUCH_PIN) == HIGH) {
    delay(50);

    if (!firstTouchDetected) {
      firstTouchDetected = true;
      touchStartTime = millis();
    } else if (!pillTaken) {
      pillTaken = true;
      int index = currentDose - 1;
      if (pillCount[index] > 0) pillCount[index]--;

      doseTakenFlag = true;   // Android will read this
      endDose();
    }
    delay(500);
  }

  // Missed dose after 1 minute
  if (millis() - doseStartTime > 60000 && doseActive) {
    if (!pillTaken) {
      doseMissedFlag = true; // Android will notify
    }
    endDose();
  }
}



void endDose() {
  digitalWrite(LED_BREAKFAST, LOW);
  digitalWrite(LED_LUNCH, LOW);
  digitalWrite(LED_DINNER, LOW);

  doseActive = false;
  firstTouchDetected = false;
  pillTaken = false;
  // do NOT touch currentDose here
}
