#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include <STM32FreeRTOS.h>


//  PIN DEFINITIONS

#define RST_PIN PA3
#define SS_PIN  PA4

const int in1       = PB8;
const int in2       = PB9;
const int buzzerPin = PA2;
const int ledPin    = PA1;
const int reedPin   = PB0;
const int vibPin    = PB1;
const int btnPin    = PB13;  // push button — motor start

//  OBJECTS
MFRC522        mfrc522(SS_PIN, RST_PIN);
hd44780_I2Cexp lcd;
HardwareSerial gsm(PA12, PA11);
HardwareSerial espSerial(PA10, PA9);
SoftwareSerial gpsSerial(PB4, PB5);
TinyGPSPlus    gps;


//  GLOBALS
byte authorizedUID[4] = {0x4A, 0xF5, 0x94, 0x97};

enum SystemState { ARMED, DISARMED, ENGINE_ON, ALARM };
volatile SystemState currentState = ARMED;
volatile bool   smsSent = false;
volatile double smsLat  = 0.0;
volatile double smsLon  = 0.0;
volatile bool   netOK   = false;

TaskHandle_t smsTaskHandle  = NULL;
TaskHandle_t mainTaskHandle = NULL;

//  GSM HELPERS — safe to call only from tasks
String gsmRead(uint32_t timeout) {
  String r = "";
  uint32_t t = millis();
  while (millis() - t < timeout) {
    while (gsm.available()) r += (char)gsm.read();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return r;
}

bool gsmCMD(String cmd, String expected, uint32_t timeout) {
  while (gsm.available()) gsm.read();
  gsm.print(cmd + "\r\n");
  String buf = "";
  uint32_t t = millis();
  while (millis() - t < timeout) {
    if (gsm.available()) {
      buf += (char)gsm.read();
      if (buf.indexOf(expected) >= 0) return true;
      if (buf.indexOf("ERROR")  >= 0) return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

bool isRegistered() {
  while (gsm.available()) gsm.read();
  gsm.print("AT+CREG?\r\n");
  String r = gsmRead(3000);
  return (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0);
}

bool isAlive() {
  while (gsm.available()) gsm.read();
  gsm.print("AT\r\n");
  String r = gsmRead(1000);
  return r.indexOf("OK") >= 0;
}

bool recoverNetwork() {
  if (!isAlive()) {
    gsm.begin(9600);
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!isAlive()) return false;
  }
  gsmCMD("ATE0",     "OK", 1000);
  vTaskDelay(pdMS_TO_TICKS(300));
  gsmCMD("AT+COPS=0","OK", 15000);
  vTaskDelay(pdMS_TO_TICKS(3000));
  for (uint8_t i = 0; i < 20; i++) {
    if (isRegistered()) return true;
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  return false;
}


//  SETUP-SAFE GSM HELPERS — use plain delay(), no vTaskDelay
//  Only call these from setup() before vTaskStartScheduler()

String gsmReadSetup(uint32_t timeout) {
  String r = "";
  uint32_t t = millis();
  while (millis() - t < timeout) {
    while (gsm.available()) r += (char)gsm.read();
    delay(10);  // plain delay — safe before scheduler
  }
  return r;
}

bool isRegisteredSetup() {
  while (gsm.available()) gsm.read();
  gsm.print("AT+CREG?\r\n");
  String r = gsmReadSetup(3000);
  return (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0);
}

//  CORE SMS SEND

bool sendSMS(const char* number, const char* message, String &outResp) {
  outResp = "";

  while (gsm.available()) gsm.read();
  gsm.print("AT\r\n");
  vTaskDelay(pdMS_TO_TICKS(1000));
  while (gsm.available()) gsm.read();

  gsm.print("ATE0\r\n");
  vTaskDelay(pdMS_TO_TICKS(1000));
  while (gsm.available()) gsm.read();

  bool textModeOK = false;
  for (int i = 0; i < 3; i++) {
    while (gsm.available()) gsm.read();
    gsm.print("AT+CMGF=1\r\n");
    String resp = "";
    uint32_t t2 = millis();
    while (millis() - t2 < 3000) {
      while (gsm.available()) resp += (char)gsm.read();
      if (resp.indexOf("OK")    >= 0) { textModeOK = true; break; }
      if (resp.indexOf("ERROR") >= 0) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (textModeOK) break;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  if (!textModeOK) { outResp = "CMGF failed"; return false; }

  vTaskDelay(pdMS_TO_TICKS(2000));

  while (gsm.available()) gsm.read();
  gsm.print("AT+CMGS=\"");
  gsm.print(number);
  gsm.print("\"\r\n");

  String prompt = "";
  uint32_t t = millis();
  bool gotPrompt = false;
  while (millis() - t < 10000) {
    while (gsm.available()) prompt += (char)gsm.read();
    if (prompt.indexOf(">")     >= 0) { gotPrompt = true; break; }
    if (prompt.indexOf("ERROR") >= 0) { outResp = prompt; return false; }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (!gotPrompt) {
    outResp = prompt.length() > 0 ? prompt : "NO PROMPT";
    gsm.write(27);
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (gsm.available()) gsm.read();
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(500));
  gsm.print(message);
  vTaskDelay(pdMS_TO_TICKS(500));
  gsm.write(26);

  t = millis();
  bool sent = false;
  while (millis() - t < 60000) {
    while (gsm.available()) outResp += (char)gsm.read();
    if (outResp.indexOf("+CMGS") >= 0) { sent = true; break; }
    if (outResp.indexOf("ERROR") >= 0) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  return sent;
}


//  LCD HELPER

void lcdPrint(const char* l1, const char* l2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}


//  SET SYSTEM STATE

void setSystemState(SystemState newState) {
  currentState = newState;
  lcd.clear();

  if (newState == ARMED) {
    digitalWrite(in1,       LOW);
    digitalWrite(in2,       LOW);
    digitalWrite(ledPin,    LOW);
    digitalWrite(buzzerPin, LOW);
    smsSent = false;
    lcd.setCursor(0, 0); lcd.print("System Armed");
    lcd.setCursor(0, 1); lcd.print(netOK ? "Net:OK Scan crd" : "Net:-- Scan crd");
  }
  else if (newState == DISARMED) {
    digitalWrite(in1,       LOW);
    digitalWrite(in2,       LOW);
    digitalWrite(ledPin,    LOW);
    digitalWrite(buzzerPin, LOW);
    smsSent = false;
    lcd.setCursor(0, 0); lcd.print("Access Granted");
    lcd.setCursor(0, 1); lcd.print("Press btn->Motor");
  }
  else if (newState == ENGINE_ON) {
    digitalWrite(in1,       HIGH);
    digitalWrite(in2,       LOW);
    digitalWrite(ledPin,    LOW);
    digitalWrite(buzzerPin, LOW);
    smsSent = false;
    lcd.setCursor(0, 0); lcd.print("Motor Running");
    lcd.setCursor(0, 1); lcd.print("Safe travels!");
  }
  else if (newState == ALARM) {
    digitalWrite(in1,       LOW);
    digitalWrite(in2,       LOW);
    digitalWrite(ledPin,    HIGH);
    digitalWrite(buzzerPin, HIGH);
    smsSent = false;
    smsLat = gps.location.lat();
    smsLon = gps.location.lng();
    lcd.setCursor(0, 0); lcd.print("INTRUSION");
    lcd.setCursor(0, 1); lcd.print("DETECTED!");
  }
}


//  ESP32 COMMUNICATION

void readESP32Commands() {
  if (espSerial.available()) {
    String cmd = espSerial.readStringUntil('\n');
    cmd.trim();
    if      (cmd == "CMD:ARM")    setSystemState(ARMED);
    else if (cmd == "CMD:DISARM") setSystemState(DISARMED);
  }
}

void sendDataToESP32() {
  static unsigned long lastSend = 0;
  if (millis() - lastSend < 500) return;
  lastSend = millis();

  espSerial.print("STATUS:");
  if      (currentState == ARMED)     espSerial.println("ARMED");
  else if (currentState == DISARMED)  espSerial.println("DISARMED");
  else if (currentState == ENGINE_ON) espSerial.println("ENGINE_ON");
  else if (currentState == ALARM)     espSerial.println("ALARM");

  if (gps.location.isValid()) {
    espSerial.print("GPS:");
    espSerial.print(gps.location.lat(), 6);
    espSerial.print(",");
    espSerial.println(gps.location.lng(), 6);
  }

  espSerial.print("DOOR:");
  espSerial.println(digitalRead(reedPin) == HIGH ? "OPEN" : "CLOSED");

  espSerial.print("ENGINE:");
  espSerial.println(digitalRead(in1) == HIGH ? "ON" : "OFF");

  espSerial.print("NET:");
  espSerial.println(netOK ? "OK" : "LOST");
}


//  SMS + NETWORK MONITOR TASK

void smsSendTask(void *pvParameters) {
  const uint32_t NET_CHECK_INTERVAL = 30000;
  uint32_t lastNetCheck = 0;

  // First network check done here — scheduler is running now
  lcdPrint("Checking net...", "Please wait");
  netOK = isRegistered();
  if (!netOK) {
    netOK = recoverNetwork();
    if (netOK) gsmCMD("AT+CMGF=1", "OK", 3000);
  }
  // Update armed screen with real network status
  setSystemState(currentState);
  lastNetCheck = millis();

  while (1) {

    // PERIODIC NETWORK HEALTH CHECK 
    if (millis() - lastNetCheck >= NET_CHECK_INTERVAL) {
      lastNetCheck = millis();
      bool wasOK = netOK;
      netOK = isRegistered();

      if (!netOK) {
        if (currentState != ALARM) {
          lcdPrint("Network LOST!", "Recovering...");
        }
        netOK = recoverNetwork();
        if (netOK) {
          gsmCMD("AT+CMGF=1", "OK", 3000);
          if (currentState != ALARM) {
            lcdPrint("Network back!", "");
            vTaskDelay(pdMS_TO_TICKS(2000));
          }
        } else {
          if (currentState != ALARM) {
            lcdPrint("No network!", "Retry in 30s");
            vTaskDelay(pdMS_TO_TICKS(2000));
          }
        }
        // Refresh state display with updated netOK
        if (currentState != ALARM) setSystemState(currentState);
      } else if (wasOK != netOK) {
        if (currentState != ALARM) setSystemState(currentState);
      }
    }

    // ALARM SMS SEND
    if (!smsSent && currentState == ALARM) {

      lcdPrint("INTRUSION!", "Sending SMS...");

      if (!netOK) {
        lcdPrint("INTRUSION!", "Recover net...");
        netOK = recoverNetwork();
        if (netOK) gsmCMD("AT+CMGF=1", "OK", 3000);
      }

      if (!netOK) {
        lcdPrint("INTRUSION!", "No net, retry..");
        vTaskDelay(pdMS_TO_TICKS(15000));
        lastNetCheck = 0;
        continue;
      }

      String msg = "ALERT: INTRUSION DETECTED! ";
      if (smsLat != 0.0 && smsLon != 0.0) {
        msg += "Lat: ";
        msg += String(smsLat, 6);
        msg += " Lon: ";
        msg += String(smsLon, 6);
        msg += " Maps: https://maps.google.com/?q=";
        msg += String(smsLat, 6);
        msg += ",";
        msg += String(smsLon, 6);
      } else {
        msg += "GPS not fixed at time of intrusion.";
      }

      String outResp = "";
      bool sent = sendSMS("+201128155622", msg.c_str(), outResp);

      if (sent) {
        smsSent = true;
        lcdPrint("SMS Sent!", "Owner notified");
        vTaskDelay(pdMS_TO_TICKS(3000));
        lcdPrint("INTRUSION", "DETECTED!");
      } else {
        lcdPrint("SMS Failed", "Retry in 15s...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        lcdPrint("INTRUSION", "DETECTED!");
        vTaskDelay(pdMS_TO_TICKS(12000));
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}


//  MAIN SYSTEM TASK

void mainTask(void *pvParameters) {
  bool lastBtnState = false;

  while (1) {
    while (gpsSerial.available() > 0) gps.encode(gpsSerial.read());

    readESP32Commands();
    sendDataToESP32();

    bool btnPressed = (digitalRead(btnPin) == LOW);

    // ARMED
    if (currentState == ARMED) {
      if (digitalRead(reedPin) == HIGH) {
        setSystemState(ALARM);
      }
      else if (digitalRead(vibPin) == HIGH) {
        setSystemState(ALARM);
      }
      else if (btnPressed && !lastBtnState) {
        lcdPrint("INTRUSION!", "Btn while armed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        setSystemState(ALARM);
      }
    }

    // DISARMED
    else if (currentState == DISARMED) {
      if (btnPressed && !lastBtnState) {
        setSystemState(ENGINE_ON);
        vTaskDelay(pdMS_TO_TICKS(500)); // debounce after state change
      }
    }

    // ENGINE_ON
    else if (currentState == ENGINE_ON) {
      if (btnPressed && !lastBtnState) {
        mfrc522.PCD_Init(); // reset RFID so card works after
        setSystemState(DISARMED);
        vTaskDelay(pdMS_TO_TICKS(500)); // debounce after state change
      }
    }

    // RFID
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      bool accessGranted = true;
      for (byte i = 0; i < 4; i++) {
        if (i >= mfrc522.uid.size ||
            mfrc522.uid.uidByte[i] != authorizedUID[i]) {
          accessGranted = false;
          break;
        }
      }

      if (accessGranted) {
        if (currentState == ARMED || currentState == ALARM) {
          setSystemState(DISARMED);
        }
        else if (currentState == DISARMED) {
          setSystemState(ARMED);
        }
        else if (currentState == ENGINE_ON) {
          setSystemState(ARMED);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
      } else {
        setSystemState(ALARM);
        vTaskDelay(pdMS_TO_TICKS(1000));
      }

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }

    // lastBtnState MUST be here — after all state changes this loop
    lastBtnState = btnPressed;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


//  SETUP

void setup() {
  Wire.setSDA(PB7);
  Wire.setSCL(PB6);
  Wire.begin();

  lcd.begin(16, 2);
  lcd.backlight();
  lcdPrint("Boot: LCD OK", "");
  delay(500);

  Serial.begin(9600);
  gsm.begin(9600);
  gpsSerial.begin(9600);
  espSerial.begin(9600);

  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(in1,       OUTPUT);
  pinMode(in2,       OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(ledPin,    OUTPUT);
  pinMode(reedPin,   INPUT_PULLUP);
  pinMode(vibPin,    INPUT);
  pinMode(btnPin,    INPUT_PULLUP);

  digitalWrite(in1,       LOW);
  digitalWrite(in2,       LOW);
  digitalWrite(buzzerPin, LOW);
  digitalWrite(ledPin,    LOW);

  // GSM wake-up — plain delay() only, no isRegistered() here
  lcdPrint("GSM Init...", "Please wait");
  delay(3000);
  gsm.print("AT\r\n");        delay(1000);
  gsm.print("ATE0\r\n");      delay(500);
  gsm.print("AT+CMGF=1\r\n"); delay(500);
  while (gsm.available()) gsm.read();
  lcdPrint("GSM Init...", "Done");
  delay(500);
  // NOTE: Network check is done inside smsSendTask after
  // scheduler starts — no isRegistered() call here

  // GPS wait — 60s max, plain delay()
  lcdPrint("Waiting GPS...", "Sats: 0");
  uint32_t gpsTimeout = millis();
  while (!gps.location.isValid() && millis() - gpsTimeout < 60000) {
    while (gpsSerial.available() > 0) gps.encode(gpsSerial.read());
    lcd.setCursor(0, 1);
    lcd.print("Sats: ");
    lcd.print(gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
    lcd.print("        ");
    delay(500);
  }
  lcdPrint(gps.location.isValid() ? "GPS Fixed!" : "GPS No fix",
           gps.location.isValid() ? "System starting" : "SMS without loc");
  delay(1500);

  setSystemState(ARMED);

  xTaskCreate(mainTask,    "Main", 384, NULL, 1, &mainTaskHandle);
  xTaskCreate(smsSendTask, "SMS",  512, NULL, 2, &smsTaskHandle);

  vTaskStartScheduler();
}

void loop() {}
