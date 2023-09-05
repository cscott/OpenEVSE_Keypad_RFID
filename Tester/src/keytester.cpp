#if defined(KEYTESTER)
#define RTCTEST 1
#include "Arduino.h"
#include <Wire.h>
#include <AsyncDelay.h>

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

// includes for OLED display
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "cap1214.h"
#include "config.h"
#define MDNS_NAME "keytester"

#ifdef RTCTEST
#include "RTClib.h"
RTC_DS3231 rtc;
char daysOfTheWeek[7][12] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
bool rtc_good = false;
#endif /* RTCTEST */
bool cap_good = false;

// Configuration of OLED display
Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);

// OLED FeatherWing buttons map to different pins depending on board:
#define BUTTON_A  0
#define BUTTON_B 16
#define BUTTON_C  2

// Other config
#define LED_GPIO 0
#define LED_OFF 1 // active low
#define LED_ON  0 // active low

AsyncDelay updateDelay = AsyncDelay(250, AsyncDelay::MILLIS);
bool blinkWasOn = false;
bool mdns_success;

bool cap1214_write(uint8_t regno, uint8_t val) {
  Wire.beginTransmission(CAP1214_I2C_ADDR);
  Wire.write(regno);
  Wire.write(val);
  uint8_t status = Wire.endTransmission();
  if (status != 0) {
    return false;
  }
  return true;
}

bool cap1214_read(uint8_t regno, uint8_t *val) {
    Wire.beginTransmission(CAP1214_I2C_ADDR);
    Wire.write(regno);
    uint8_t status = Wire.endTransmission();
    if (status != 0) {
        return false;
    }
    uint8_t nBytes = Wire.requestFrom(CAP1214_I2C_ADDR, 1);
    if (nBytes == 0) {
        return false;
    }
    *val = Wire.read();
    return true;
}

bool cap1214_init(void) {
  uint8_t vendor_id, product_id, revision, build_rev;
  delay(20); // at least 20ms (tCOMM)
  if (!cap1214_read(CAP1214_REG_VENDOR_ID, &vendor_id)) { return false; }
  if (vendor_id != 0x5D) { return false; }
  if (!cap1214_read(CAP1214_REG_PRODUCT_ID, &product_id)) { return false; }
  if (product_id != 0x5A) { return false; }
  if (!cap1214_read(CAP1214_REG_REVISION, &revision)) { return false; }
  if (revision != 0x80) { return false; }
  if (!cap1214_read(CAP1214_REG_BUILD_REVISION, &build_rev)) { return false; }
  if (build_rev != 0x10) { return false; }
  // Activate LEDs and scanning
  cap1214_write(CAP1214_REG_MAIN_STATUS, 0x00);
  cap1214_write(CAP1214_REG_CONFIGURATION_2, 0x02); // *not* VOL_UP_DOWN
  // Increase general sensitivity
  cap1214_write(CAP1214_REG_DATA_SENSITIVITY, 0x1F); // 64x sensitivity
  // (prior to boosting overall sensitivity: CA) (c8)
  cap1214_write(CAP1214_REG_PROXIMITY_CONTROL, 0xC9); // CS1 is prox, sum, max sens
  // allow independent configuration of CS1 threshold
  cap1214_write(CAP1214_REG_RECALIBRATION_CONFIGURATION, 0x13); // !BUT_LD_TH
  // 256ms, 2kHz 110 01000
  cap1214_write(CAP1214_REG_FEEDBACK_CONFIGURATION, 0xC8); // 256ms, 2kHz
  // Everything triggers feedback, but CS1 and CS14
  cap1214_write(CAP1214_REG_FEEDBACK_CHANNEL_CONFIGURATION_1, 0x7E);
  cap1214_write(CAP1214_REG_FEEDBACK_CHANNEL_CONFIGURATION_2, 0x3F);
  // Link the sensor LEDs CS2-CS3,CS5-CS7
  // - excluding CS1 because it is used for proximity
  // - excluding CS4 for which we need manual control because it is paired
  cap1214_write(CAP1214_REG_SENSOR_LED_LINKING, 0x76);
  // LED/GPIO pins are outputs!
  cap1214_write(CAP1214_REG_LED_GPIO_DIRECTION, 0xFF); // LED1-LED8 = output
  // flip polarity of all LEDs (normally on!)
  cap1214_write(CAP1214_REG_LED_POLARITY_1, 0xFF);
  cap1214_write(CAP1214_REG_LED_POLARITY_2, 0xFF);
  // recalibrate all
  //cap1214_write(CAP1214_REG_DIGITAL_RECALIBRATION, 0xFF);
  return true;
}

uint16_t cap1214_read_buttons(bool clear = true) {
  uint8_t st1 = 0, st2 = 0;
  cap1214_read(CAP1214_REG_BUTTON_STATUS_1, &st1);
  cap1214_read(CAP1214_REG_BUTTON_STATUS_2, &st2);
  if (clear) {
    // clear the latched button status by clearing INT!
    cap1214_write(CAP1214_REG_MAIN_STATUS, 0x00);
  }
  // merge
  uint16_t merged = (st1 & 0x3F); // zero out UP & DOWN
  merged |= (((uint16_t)st2) << 6); // Add in high order bits at the right place
  // manually set feedback pins for CS8-CS13
  // CS8/9/10 -> LED8/9/10
  uint8_t led1 = 0, led2 = 0;
  if (merged & (((uint16_t)1)<<7)) { // CS8 (touch 11, key 0)
    led1 |= 0x80;
  }
  if (merged & (((uint16_t)1)<<8)) { // CS9 (touch 2)
    led2 |= 0x01;
  }
  if (merged & (((uint16_t)1)<<9)) { // CS10 (touch 3)
    led2 |= 0x02;
  }
  // CS11 -> LED4 (as well as CS4)
  if (merged & (((uint16_t)1)<<3)) { // CS4 (touch 10, key backspace)
    led1 |= 0x08;
  }
  if (merged & (((uint16_t)1)<<10)) { // CS11 (touch 12, key enter)
    led1 |= 0x08;
  }
  // CS12 -> LED1
  if (merged & (((uint16_t)1)<<11)) { // CS12 (touch 9)
    led1 |= 0x01;
  }
  // CS13 -> LED11
  if (merged & (((uint16_t)1)<<12)) { // CS13 (touch 6)
    led2 |= 0x04;
  }
  // Update LED OUTPUT CONTROL if necessary
  // last_* initialized to bogus values to force an initial update
  static uint8_t last_led1 = 0xFF, last_led2 = 0xFF;
  if (led1 != last_led1) {
    cap1214_write(CAP1214_REG_LED_OUTPUT_CONTROL_1, led1);
    last_led1 = led1;
  }
  if (led2 != last_led2) {
    cap1214_write(CAP1214_REG_LED_OUTPUT_CONTROL_2, led2);
    last_led2 = led2;
  }
  return merged;
}

void cap1214_read_key(char *key, bool *prox) {
  uint16_t st = cap1214_read_buttons();
  *prox = (st & 1);
  *key = 0;
  uint16_t mask;
  const char *cp = "47\b158023\r96";
  for (mask=2; *cp; cp++, mask<<=1) {
    if (st&mask) {
      *key = *cp;
      return;
    }
  }
}

void normalText() {
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
}

void invertText() {
  display.setTextColor(SH110X_BLACK, SH110X_WHITE);
}

void maybeInvertText(bool maybe) {
  if (maybe) invertText(); else normalText();
}

void printHex(uint8_t num) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", (int)num);
    display.print(buf);
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextSize(1);
  invertText();
  display.println("Keypad tester");
  normalText();
#if 0
  display.print("IP: ");
  display.println(WiFi.localIP());
  if (mdns_success) {
      display.println("mDNS: " MDNS_NAME);
  }
#endif

  display.setTextSize(1);

#if 0
  // Just show responding I2C addresses
  uint8_t addr = 0;
  for (addr = 0; addr <= 0x7F; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      printHex(addr); display.print(" ");
    }
  }
  display.println();
#else
  display.print("CAP ");
  if (cap_good) {
    uint16_t st = cap1214_read_buttons(false);
    printHex(st >> 8);
    printHex(st & 0xFF);
    display.print(" ");
    char key[2] = { 0, 0 }; bool prox;
    cap1214_read_key(key, &prox);
    switch (*key) {
    case '\0': display.print(" "); break;
    case '\r': display.print("ENTER"); break;
    case '\b': display.print("BKSP"); break;
    default: display.print(key); break;
    }
    if (prox) { display.print(" PROX"); }
    display.println();
  } else {
    display.println("not found");
    uint8_t val = 0;
    bool r = cap1214_read(CAP1214_REG_VENDOR_ID, &val);
    display.print(r ? "true " : "false ");
    printHex(val);
    display.println();
  }
#endif
#ifdef RTCTEST
  if (rtc_good) {
    DateTime now = rtc.now();

    display.print(now.year(), DEC);
    display.print('/');
    display.print(now.month(), DEC);
    display.print('/');
    display.print(now.day(), DEC);
    display.print(" (");
    display.print(daysOfTheWeek[now.dayOfTheWeek()]);
    display.print(") ");
    display.print(now.hour(), DEC);
    display.print(':');
    display.print(now.minute(), DEC);
    display.print(':');
    display.print(now.second(), DEC);
    display.println();

    display.print(" since midnight 1/1/1970 = ");
    display.print(now.unixtime());
    display.print("s = ");
    display.print(now.unixtime() / 86400L);
    display.println("d");

    display.print("Temperature: ");
    display.print(rtc.getTemperature());
    display.println(" C");
  } else {
    display.println("RTC not found");
  }
#endif

  display.display();
}

void setup() {
  Wire.begin();
  Serial.begin(115200);

  // Wifi Setup
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // OLED setup
  display.begin(0x3C, true); // Address 0x3C default
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

  // Clear the buffer.
  display.clearDisplay();
  display.setRotation(1);

  display.setTextSize(1);
  display.setCursor(0,0);

  invertText();
  display.println("Keypad tester");
  normalText();

  display.println("");
  display.print("Connecting to SSID\n" WIFI_SSID ": ");
  display.display();

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.display();

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(WIFI_SSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  mdns_success = false;
  if (MDNS.begin(MDNS_NAME)) {
    mdns_success = true;
    Serial.println("MDNS responder started: " MDNS_NAME);
    display.println("mDNS: " MDNS_NAME);
    display.display();
  }

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname(MDNS_NAME);
  ArduinoOTA.setPassword(OTA_PASSWD);
  ArduinoOTA
  .onStart([]() {
    String type;
    digitalWrite(LED_BUILTIN, LOW);
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();

#ifdef RTCTEST
  display.print("RTC ");
  rtc_good = rtc.begin();
  display.println(rtc_good ? "good": "not found");
  if (rtc_good && rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
#endif /* RTCTEST */

  display.print("CAP ");
  cap_good = cap1214_init();
  display.println(cap_good ? "good": "bad");
  display.display();
  delay(500);
}

void loop() {
    ArduinoOTA.handle();
    MDNS.update();
    if (!updateDelay.isExpired()) {
        return;
    }
    updateDelay.restart();

    updateDisplay();
}

#endif /* KEYTESTER */
