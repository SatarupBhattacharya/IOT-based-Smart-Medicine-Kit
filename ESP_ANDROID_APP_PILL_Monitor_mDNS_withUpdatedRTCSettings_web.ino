#include <ESP8266mDNS.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h> 

// --- NEW LIBRARY ---
#include <WiFiManager.h> 

// (Delete the const char* ssid and pass lines)

ESP8266WebServer server(80);
RTC_DS3231 rtc;

// ... [Keep all your pins and variables exactly the same] ...
#define TOUCH_PIN D1
#define LED_BREAKFAST D5
#define LED_LUNCH D6
#define LED_DINNER D7

unsigned long lastRtcPrint = 0;
unsigned long lastNtpSync = 0; // Timer for the 30-second NTP sync

int pillCount[3] = {6, 6, 6};   // breakfast, lunch, dinner
int alarmTimes[3] = {0, 0, 0};  // HHMM

bool doseActive = false;
bool firstTouchDetected = false;
bool pillTaken = false;
unsigned long touchStartTime = 0;
int currentDose = 0;

bool doseTakenFlag = false;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    
bool doseMissedFlag = false;
unsigned long doseStartTime = 0;

// Prevent retrigger in same minute
int lastTriggeredMinute = -1;

// --- NEW FUNCTION: Sync RTC with the ESP's Network Time ---
void syncRTCwithNTP() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // Only update if the year is valid (meaning NTP actually fetched the time)
  if (timeinfo.tm_year > 120) { // tm_year is years since 1900. > 120 means > 2020.
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
  }
}
void setup() {
  Serial.begin(115200);
  Wire.begin(D2, D3);
  
  if (!rtc.begin()) {
    Serial.println("\n\nCRITICAL ERROR: Couldn't find RTC!");
  }

  pinMode(TOUCH_PIN, INPUT);
  pinMode(LED_BREAKFAST, OUTPUT);
  pinMode(LED_LUNCH, OUTPUT);
  pinMode(LED_DINNER, OUTPUT);

  digitalWrite(LED_BREAKFAST, LOW);
  digitalWrite(LED_LUNCH, LOW);
  digitalWrite(LED_DINNER, LOW);

  // --- NEW: FACTORY RESET TRIGGER ---
  // Hold the touch sensor while booting/resetting to clear Wi-Fi
  delay(500); // Brief pause to give the sensor time to stabilize on boot
  if (digitalRead(TOUCH_PIN) == HIGH) {
    Serial.println("\n--- FACTORY RESET TRIGGERED ---");
    Serial.println("Wiping saved Wi-Fi credentials...");
    
    // Rapidly flash all LEDs 5 times to confirm to the user
    for(int i = 0; i < 5; i++) {
      digitalWrite(LED_BREAKFAST, HIGH); digitalWrite(LED_LUNCH, HIGH); digitalWrite(LED_DINNER, HIGH);
      delay(200);
      digitalWrite(LED_BREAKFAST, LOW); digitalWrite(LED_LUNCH, LOW); digitalWrite(LED_DINNER, LOW);
      delay(200);
    }

    WiFiManager wifiManager;
    wifiManager.resetSettings(); // Wipes the memory
    
    Serial.println("Memory wiped. Rebooting into Setup Mode...");
    delay(1000);
    ESP.restart(); // Restart the board so it opens the Captive Portal
  }
  // --- END FACTORY RESET TRIGGER ---

  // --- WIFIMANAGER SETUP ---
  Serial.println("\nStarting WiFiManager...");
  WiFiManager wifiManager;
  
  // This creates the Access Point. 
  // Parameter 1: Name of the AP, Parameter 2: Password for the AP
  bool res = wifiManager.autoConnect("MediSync_Setup", "12345678"); 

  if (!res) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart(); // Reset and try again
    delay(5000);
  }

  // If you get here, you have connected to the WiFi!
  Serial.println("WiFi connected!");
  Serial.print("ESP IP: ");
  Serial.println(WiFi.localIP());

  // --- RESUME YOUR NORMAL SETUP ---
  Serial.print("Configuring NTP for IST (+5:30)...");
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);
  while (now < 24 * 3600) {
    Serial.print(".");
    delay(500);
    now = time(nullptr);
  }
  Serial.println(" Time synchronized!");
  
  syncRTCwithNTP();

  if (MDNS.begin("medisync")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS responder started");
  } else {
    Serial.println("Error setting up mDNS");
  }

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
  
  // (Assuming the rest of your API endpoints like /settime and /clear are still right below this!)
  
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
  MDNS.update();

  checkAlarm();
  checkTouch();

  // --- NEW: Sync RTC with NTP every 30 seconds ---
  if (WiFi.status() == WL_CONNECTED && millis() - lastNtpSync > 30000) {
    lastNtpSync = millis();
    syncRTCwithNTP();
  }

  if (millis() - lastRtcPrint > 5000) {
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

  // Track the previous state of the sensor to detect distinct taps
  static int lastTouchState = LOW; 
  int currentTouchState = digitalRead(TOUCH_PIN);

  // Only trigger when the pin goes from LOW to HIGH (a new, separate touch)
  if (currentTouchState == HIGH && lastTouchState == LOW) {
    delay(50); // Debounce to prevent false readings

    // Confirm it is still being touched after debounce
    if (digitalRead(TOUCH_PIN) == HIGH) { 
      if (!firstTouchDetected) {
        firstTouchDetected = true;
        Serial.println("First touch acknowledged"); // 1st Touch Print
        touchStartTime = millis();
      } else if (!pillTaken) {
        Serial.println("Pill Taken"); // 2nd Touch Print
        pillTaken = true;
        int index = currentDose - 1;
        if (pillCount[index] > 0) pillCount[index]--;

        doseTakenFlag = true;
        // Android will read this
        endDose();
      }
    }
  }
  
  // Save the current state for the next loop iteration
  lastTouchState = currentTouchState; 

  // Missed dose after 1 minute
  if (millis() - doseStartTime > 60000 && doseActive) {
    if (!pillTaken) {
      doseMissedFlag = true;
      // Android will notify
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
// ... [Keep loop(), checkTouch(), checkAlarm(), etc. exactly the same] ...