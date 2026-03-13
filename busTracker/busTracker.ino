#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "Adafruit_LiquidCrystal.h"
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Protomatter.h>
#include "secrets.h"

// Only print to Serial when USB is connected (prevents blocking on USB CDC boards)
#define DBG_BEGIN(baud) Serial.begin(baud)
#define DBG_PRINT(...) do { if (Serial) Serial.print(__VA_ARGS__); } while(0)
#define DBG_PRINTLN(...) do { if (Serial) Serial.println(__VA_ARGS__); } while(0)

// Forward declarations
struct StopArrival;
StopArrival* findArrival(const char* lineRef, const char* stopRef);

// Number of milliseconds to wait without receiving any data before we give up
const int kNetworkTimeout = 30*1000; // 30 seconds (worker response is small)

// ------------------------
WiFiClientSecure wifi;
HttpClient client = HttpClient(wifi, WORKER_HOST, 443);

// Connect via i2c, default address #0 (A0-A2 not jumpered)
Adafruit_LiquidCrystal lcd(0);

// --- LED Matrix (Adafruit Protomatter) ---
uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;
Adafruit_Protomatter matrix(
  32, 4, 1, rgbPins, 3, addrPins, clockPin, latchPin, oePin, false);

// --- Global data structures for bus arrivals ---
struct StopDef {
  const char* ref;
  int8_t ledRow; // -1 = no LED
  int8_t ledCol;
};
struct AllowedStop {
  const char* lineRef;
  StopDef stops[25];
  int stopCount;
};
AllowedStop allowedStops[] = {
  {"24", {
    {"15147",8,6}, {"14429",7,3}, {"14330",7,5}, {"14315",7,7},
    {"13521",6,7}, {"14143",10,2}, {"15882",12,3}, {"15878",13,5},
    {"14624",8,7}, {"14428",7,2}, {"14331",7,4}, {"14314",7,6},
    {"13520",6,6}, {"14142",11,2}, {"15881",12,2}, {"15490",13,4}
  }, 16},
  {"23", {
    {"17208",3,3}, {"16436",3,1}, {"14386",5,5}, {"14192",7,1},
    {"14200",9,3}, {"15882",12,5}, {"15864",13,7}, {"15865",12,1},
    {"16453",3,2}, {"16435",3,0}, {"14387",5,4}, {"14198",7,0},
    {"14203",9,2}, {"15881",12,4}, {"15863",13,6}, {"15776",12,0}
  }, 16},
  {"49", {
    {"16819",1,12}, {"18102",15,9}, {"18104",15,12}, {"15836",14,9},
    {"15552",11,8}, {"15566",10,8}, {"15572",9,7}, {"15614",8,5},
    {"15782",4,0}, {"17804",5,6}, {"15926",4,6}, {"16820",1,11},
    {"18091",15,8}, {"18089",15,11}, {"15546",14,8}, {"15551",11,7},
    {"15565",10,7}, {"15571",9,6}, {"15613",8,4}, {"15783",4,1},
    {"15781",5,7}, {"15791",4,7}
  }, 22},
  {"14", {
    {"16498",15,2}, {"15529",15,6}, {"15536",14,1}, {"15543",14,5},
    {"15836",13,1}, {"15552",11,6}, {"15566",10,6}, {"15572",9,5},
    {"15614",8,3}, {"15592",4,2}, {"15588",3,4}, {"15693",15,3},
    {"15530",15,7}, {"15535",14,0}, {"15542",14,4}, {"17299",13,0},
    {"15551",11,5}, {"15565",10,5}, {"15571",9,4}, {"15613",8,2},
    {"15593",4,3}, {"17099",3,5}
  }, 22},
  {"14R", {
    {"16498",15,2}, {"15529",15,6}, {"15536",14,1}, {"15552",11,6},
    {"15566",10,6}, {"15572",9,5}, {"15614",8,3}, {"15592",4,2},
    {"15588",3,4}, {"15693",15,3}, {"15530",15,7}, {"15535",14,0},
    {"15551",11,5}, {"15565",10,5}, {"15571",9,4}, {"15613",8,2},
    {"15593",4,3}
  }, 17},
  {"67", {
    {"17532",8,1}, {"17746",11,1}, {"14686",10,1}, {"17924",11,3},
    {"14688",9,1}, {"14690",10,3}, {"13476",8,0}, {"17552",11,0},
    {"14697",10,0}, {"13710",11,4}, {"14687",9,0}, {"14568",10,4}
  }, 12},
  {"J", {
    {"17217",15,0}, {"16994",15,4}, {"16995",14,3}, {"16997",14,7},
    {"16996",13,3}, {"18059",6,1}, {"16214",6,3}, {"18156",6,5},
    {"16280",5,1}, {"14788",5,2}, {"15418",4,4}, {"16992",15,1},
    {"15731",15,5}, {"15417",14,2}, {"15727",14,6}, {"15419",13,2},
    {"14006",6,0}, {"16215",6,2}, {"18155",6,4}, {"16277",5,0},
    {"14787",5,3}, {"17778",4,5}
  }, 22}
};
#define NUM_ALLOWED_LINES (sizeof(allowedStops) / sizeof(allowedStops[0]))
#define MAX_STOPS_PER_LINE 25
const int allowedLines = NUM_ALLOWED_LINES;

const int MAX_ARRIVALS_PER_STOP = 3;
struct StopArrival {
  const char* stopRef;
  int arrivalMinutes[MAX_ARRIVALS_PER_STOP];
  int count;
};
static StopArrival stopArrivals[NUM_ALLOWED_LINES][MAX_STOPS_PER_LINE];

#define LED_ARRIVAL_THRESHOLD 2

// Get RGB color for a bus line
void getLineColor(const char* lineRef, uint8_t &r, uint8_t &g, uint8_t &b) {
  if      (strcmp(lineRef, "49") == 0)  { r=0;   g=255; b=0;   } // green
  else if (strcmp(lineRef, "14") == 0)  { r=255; g=0;   b=0;   } // red
  else if (strcmp(lineRef, "14R") == 0) { r=255; g=0;   b=0;   } // red (same as 14)
  else if (strcmp(lineRef, "67") == 0)  { r=0;   g=0;   b=255; } // blue
  else if (strcmp(lineRef, "J") == 0)   { r=255; g=110; b=0;   } // orange
  else if (strcmp(lineRef, "23") == 0)  { r=255; g=255; b=0;   } // yellow
  else if (strcmp(lineRef, "24") == 0)  { r=255; g=0;   b=255; } // magenta
  else                                  { r=255; g=255; b=255; }
}

// Update LED matrix: light stops with a bus arriving within threshold
void updateLEDs() {
  matrix.fillScreen(0);
  for (int k = 0; k < allowedLines; k++) {
    for (int j = 0; j < allowedStops[k].stopCount; j++) {
      StopArrival* sa = &stopArrivals[k][j];
      int8_t row = allowedStops[k].stops[j].ledRow;
      int8_t col = allowedStops[k].stops[j].ledCol;
      if (row < 0) continue;
      if (sa->count > 0 && sa->arrivalMinutes[0] <= LED_ARRIVAL_THRESHOLD) {
        uint8_t r, g, b;
        getLineColor(allowedStops[k].lineRef, r, g, b);
        matrix.drawPixel(col, row, matrix.color565(r, g, b));
      }
    }
  }
  matrix.show();
}

// --- LCD display page definitions ---
struct DisplayPage {
  const char* lineRef;
  const char* stopRef1;
  const char* label1;
  const char* stopRef2;
  const char* label2;
  int walkMinutes;
};
DisplayPage displayPages[] = {
  {"24","14142", "PacHt", "14143", "Bayvw", 10},
  {"23","14203", "SFZoo", "14200", "Bayvw", 1},
  {"49","15613", "FtMsn", "15614", "CtyCl", 10},
  {"14","15613", "Ferry", "15614", "DlyCt", 10},
  {"14R","15613", "Ferry", "15614", "DlyCt", 10},
  {"67","14568", "Missn", "14690", "Bernl",  5},
  {"J","16277", "Ferry", "16280", "Balbo", 15},
};
const int NUM_DISPLAY_PAGES = sizeof(displayPages) / sizeof(displayPages[0]);

// Button for cycling pages
const int BUTTON_PIN = 9;
int currentPage = 0;
bool currentDirection = false; // false = direction 1, true = direction 2
unsigned long lastDirectionSwap = 0;
const unsigned long DIRECTION_INTERVAL = 5000; // 5 seconds
unsigned long lastFetchTime = 0;
unsigned long lastSuccessfulFetch = 0;
const unsigned long FETCH_INTERVAL = 90UL * 1000; // 90 seconds
const unsigned long STALE_THRESHOLD = 3UL * 60 * 1000; // 3 minutes = stale


// Find arrival data for a specific line + stop
StopArrival* findArrival(const char* lineRef, const char* stopRef) {
  for (int k = 0; k < allowedLines; k++) {
    if (strcmp(lineRef, allowedStops[k].lineRef) == 0) {
      for (int j = 0; j < allowedStops[k].stopCount; j++) {
        if (strcmp(stopRef, stopArrivals[k][j].stopRef) == 0) {
          return &stopArrivals[k][j];
        }
      }
    }
  }
  return nullptr;
}

// Display a page: row 1 = "LL Xm walk", row 2 = one direction's arrivals
void displayPage(int pageIndex, bool direction) {
  DisplayPage* p = &displayPages[pageIndex];
  char row[17];

  // Row 1: line number + walk time (* = stale data)
  bool stale = (lastSuccessfulFetch == 0 && lastFetchTime > 0) ||
               (lastSuccessfulFetch > 0 && (millis() - lastSuccessfulFetch > STALE_THRESHOLD));
  lcd.clear();
  lcd.setCursor(0, 0);
  snprintf(row, 17, "%-3s %dm walk%s", p->lineRef, p->walkMinutes, stale ? " !" : "");
  lcd.print(row);

  // Row 2: direction label + arrival times
  const char* stopRef = direction ? p->stopRef2 : p->stopRef1;
  const char* label   = direction ? p->label2   : p->label1;
  StopArrival* sa = findArrival(p->lineRef, stopRef);

  char times[12] = "";
  if (sa && sa->count > 0) {
    int pos = 0;
    for (int i = 0; i < sa->count && pos < 9; i++) {
      if (i > 0) times[pos++] = ',';
      pos += snprintf(times + pos, 10 - pos, "%d", sa->arrivalMinutes[i]);
    }
    times[pos] = '\0';
  } else {
    strcpy(times, "--");
  }

  lcd.setCursor(0, 1);
  snprintf(row, 17, "%-5s: %sm", label, times);
  lcd.print(row);
}

// Connects/reconnects WiFi. Returns true if connected.
bool ensureWiFi(const char* lcdMsg, unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  DBG_PRINT("\n");
  DBG_PRINT(lcdMsg);
  lcd.clear();
  lcd.print(lcdMsg);

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  delay(500);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < timeoutMs) {
    DBG_PRINT(".");
    delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) {
    DBG_PRINTLN("\nWiFi connect failed!");
    lcd.clear();
    lcd.print("WiFi failed!");
    lcd.setCursor(0, 1);
    lcd.print("Will retry...");
    return false;
  }
  DBG_PRINTLN("\nWiFi connected!");
  wifi.setInsecure(); // skip SSL certificate verification
  return true;
}

// Returns elapsed ms on success, 0 on failure
unsigned long fetchBusData() {
  DBG_PRINTLN("\nFetching bus data from worker...");
  unsigned long fetchStart = millis();

  if (!ensureWiFi("Reconnecting...", 15000)) {
    return 0;
  }

  unsigned long result = 0; // 0 = failure

  client.setHttpResponseTimeout(kNetworkTimeout);
  client.beginRequest();
  int err = client.get("/api");
  client.sendHeader("Authorization", "Bearer " WORKER_AUTH_TOKEN);
  client.endRequest();
  if (err != 0) {
    DBG_PRINT("HTTP GET failed, error: ");
    DBG_PRINTLN(err);
    goto cleanup;
  }

  {
    int statusCode = client.responseStatusCode();
    if (statusCode != 200) {
      DBG_PRINT("Status code: ");
      DBG_PRINTLN(statusCode);
      goto cleanup;
    }
  }

  {
    DBG_PRINT("Content-Length: ");
    DBG_PRINTLN(client.contentLength());

    // Read body into buffer, then close connection to free SSL memory (~40KB)
    // before allocating the JSON document — prevents peak memory crash
    String body = client.responseBody();
    client.stop();

    DBG_PRINT("Body length: "); DBG_PRINTLN(body.length());
    DBG_PRINT("Free heap: "); DBG_PRINTLN(ESP.getFreeHeap());

    JsonDocument doc;
    DeserializationError parseErr = deserializeJson(doc, body);
    body = String(); // free body buffer now that doc owns the data
    if (parseErr) {
      DBG_PRINT("JSON parse error: ");
      DBG_PRINTLN(parseErr.c_str());
      goto cleanup; // previous good data in stopArrivals is preserved
    }

    // Parse succeeded — now safe to reinitialize stopArrivals
    for (int k = 0; k < allowedLines; k++) {
      for (int j = 0; j < allowedStops[k].stopCount; j++) {
        stopArrivals[k][j].stopRef = allowedStops[k].stops[j].ref;
        for (int m = 0; m < MAX_ARRIVALS_PER_STOP; m++) stopArrivals[k][j].arrivalMinutes[m] = 9999;
        stopArrivals[k][j].count = 0;
      }
    }

    int visitCount = 0;
    for (int k = 0; k < allowedLines; k++) {
      const char* lineRef = allowedStops[k].lineRef;
      JsonObject lineObj = doc[lineRef];
      if (lineObj.isNull()) continue;

      for (int j = 0; j < allowedStops[k].stopCount; j++) {
        const char* stopRef = allowedStops[k].stops[j].ref;
        JsonArray arr = lineObj[stopRef];
        if (arr.isNull()) continue;

        StopArrival* sa = &stopArrivals[k][j];
        int count = 0;
        for (JsonVariant v : arr) {
          if (count >= MAX_ARRIVALS_PER_STOP) break;
          int mins = v.as<int>() - 1; // subtract 1 min: API times can be up to 1 min late
          if (mins < 0) mins = 0;
          if (mins > 999) mins = 999;
          sa->arrivalMinutes[count] = mins;
          count++;
        }
        sa->count = count;
        visitCount += count;
      }
    }

    unsigned long fetchElapsed = millis() - fetchStart;
    DBG_PRINT("Fetch time: ");
    DBG_PRINT(fetchElapsed / 1000);
    DBG_PRINT(".");
    DBG_PRINT((fetchElapsed % 1000) / 100);
    DBG_PRINTLN("s");
    DBG_PRINT("Arrival entries: ");
    DBG_PRINTLN(visitCount);
    // Print results for debugging
    for (int k = 0; k < allowedLines; k++) {
      DBG_PRINT("Line "); DBG_PRINT(allowedStops[k].lineRef); DBG_PRINTLN(":");
      for (int j = 0; j < allowedStops[k].stopCount; j++) {
        StopArrival* sa = &stopArrivals[k][j];
        if (sa->count == 0) continue;
        DBG_PRINT(sa->stopRef); DBG_PRINT(": ");
        for (int m = 0; m < sa->count; m++) {
          DBG_PRINT(sa->arrivalMinutes[m]);
          if (m < sa->count-1) DBG_PRINT(", ");
        }
        DBG_PRINTLN();
      }
    }

    result = fetchElapsed;
  }

cleanup:
  client.stop();
  return result;
}

void setup() {
  DBG_BEGIN(9600);
  delay(500); // brief settle time after boot

  // set up the LCD's number of rows and columns:
  if (!lcd.begin(16, 2)) {
    DBG_PRINTLN("Could not init backpack. Check wiring.");
    delay(3000);
    ESP.restart();
  }
  DBG_PRINTLN("Backpack init'd.");
  lcd.setBacklight(HIGH);

  // Initialize LED matrix
  ProtomatterStatus matStatus = matrix.begin();
  if (matStatus != PROTOMATTER_OK) {
    DBG_PRINT("Protomatter error: "); DBG_PRINTLN((int)matStatus);
  }
  matrix.fillScreen(0);
  matrix.show();

  // Custom character: down arrow (slot 0)
  byte downArrow[8] = {
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b10101,
    0b01110,
    0b00100,
    0b00000
  };
  lcd.createChar(0, downArrow);

  // Show reset reason on LCD if abnormal (helps diagnose reboot loops)
  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_BROWNOUT || reason == ESP_RST_PANIC ||
      reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT) {
    lcd.setCursor(0, 0);
    switch (reason) {
      case ESP_RST_BROWNOUT: lcd.print("RST: BROWNOUT"); break;
      case ESP_RST_PANIC:    lcd.print("RST: PANIC");    break;
      case ESP_RST_INT_WDT:  lcd.print("RST: INT_WDT");  break;
      case ESP_RST_TASK_WDT: lcd.print("RST: TASK_WDT"); break;
      default:               lcd.print("RST: WDT");      break;
    }
    lcd.setCursor(0, 1);
    lcd.print("Heap:");
    lcd.print(ESP.getFreeHeap());
    delay(5000);
    lcd.clear();
  }

  lcd.print("Connecting...");

  ensureWiFi("Connecting WiFi", 30000);
  WiFi.setAutoReconnect(true);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lcd.clear();
  lcd.print("Waiting...");
}

void loop() {
  // Refresh data every FETCH_INTERVAL
  if (lastFetchTime == 0 || millis() - lastFetchTime >= FETCH_INTERVAL) {
    // Show update indicator — fetch blocks the loop so button/cycling won't respond
    lcd.setCursor(15, 0);
    lcd.write(byte(0)); // down arrow
    unsigned long fetchElapsed = fetchBusData();
    if (fetchElapsed > 0) {
      lastSuccessfulFetch = millis();
    }
    lastFetchTime = millis();
    lastDirectionSwap = millis();
    currentDirection = false;
    displayPage(currentPage, currentDirection);
    updateLEDs();
  }

  // Alternate direction every 5 seconds
  if (millis() - lastDirectionSwap >= DIRECTION_INTERVAL) {
    currentDirection = !currentDirection;
    lastDirectionSwap = millis();
    displayPage(currentPage, currentDirection);
  }

  // Button handling: single click = cycle page, double click = instant fetch, hold = reset
  static bool lastButtonState = HIGH;
  static unsigned long pressStartTime = 0;
  static unsigned long lastClickTime = 0;
  static int clickCount = 0;
  static bool holdHandled = false;
  const unsigned long DEBOUNCE_MS = 50;
  const unsigned long DOUBLE_CLICK_MS = 400;
  const unsigned long HOLD_MS = 1500;

  bool buttonState = digitalRead(BUTTON_PIN);

  // Button just pressed
  if (buttonState == LOW && lastButtonState == HIGH) {
    pressStartTime = millis();
    holdHandled = false;
  }

  // Button held down — check for long press
  if (buttonState == LOW && !holdHandled && (millis() - pressStartTime >= HOLD_MS)) {
    holdHandled = true;
    clickCount = 0;
    lcd.clear();
    lcd.print("Resetting...");
    delay(500);
    ESP.restart();
  }

  // Button just released (and not a hold)
  if (buttonState == HIGH && lastButtonState == LOW && !holdHandled) {
    if (millis() - pressStartTime >= DEBOUNCE_MS) {
      clickCount++;
      lastClickTime = millis();
    }
  }

  // Process clicks after double-click window expires
  if (clickCount > 0 && (millis() - lastClickTime >= DOUBLE_CLICK_MS)) {
    if (clickCount >= 2) {
      // Double click: instant fetch
      clickCount = 0;
      lcd.setCursor(15, 0);
      lcd.write(byte(0)); // down arrow
      unsigned long fetchElapsed = fetchBusData();
      if (fetchElapsed > 0) {
        lastSuccessfulFetch = millis();
      }
      lastFetchTime = millis();
      lastDirectionSwap = millis();
      currentDirection = false;
      displayPage(currentPage, currentDirection);
      updateLEDs();
    } else {
      // Single click: cycle page
      clickCount = 0;
      currentPage = (currentPage + 1) % NUM_DISPLAY_PAGES;
      currentDirection = false;
      lastDirectionSwap = millis();
      displayPage(currentPage, currentDirection);
    }
  }

  lastButtonState = buttonState;

  delay(5);
}
