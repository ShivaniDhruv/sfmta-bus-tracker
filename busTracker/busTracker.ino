#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "Adafruit_LiquidCrystal.h"
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
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

// --- Global data structures for bus arrivals ---
struct AllowedStop {
  const char* lineRef;
  const char* stopRefs[25];
  int stopCount;
};
AllowedStop allowedStops[] = {
  {"24", {"15147", "14429", "14330", "14315", "13521", "14143", "15882", "15878", "14624", "14428", "14331", "14314", "13520", "14142", "15881", "15490"}, 16},
  {"23", {"17208", "16436", "14386", "14192", "14200", "15882", "15864", "15865", "16453", "16435", "14387", "14198", "14203", "15881", "15863", "15776"}, 16},
  {"49", {"16819", "18102", "18104", "15836", "15552", "15566", "15572", "15614", "15782", "17804", "15926", "16820", "18091", "18089", "15546", "15551", "15565", "15571", "15613", "15783", "15781", "15791"}, 22},
  {"14", {"16498", "15529", "15536", "15543", "15836", "15552", "15566", "15572", "15614", "15592", "15588", "15693", "15530", "15535", "15542", "17299", "15551", "15565", "15571", "15613", "15593", "17099"}, 22},
  {"14R", {"16498", "15529", "15536", "15552", "15566", "15572", "15614", "15592", "15588", "15693", "15530", "15535", "15551", "15565", "15571", "15613", "15593"}, 17},
  {"67", {"17532", "17746", "14686", "17924", "14688", "14690", "13476", "17552", "14697", "13710", "14687", "14568"}, 12},
  {"J", {"17217", "16994", "16995", "16997", "16996", "18059", "16214", "18156", "16280", "14788", "15418", "16992", "15731", "15417", "15727", "15419", "14006", "16215", "18155", "16277", "14787", "17778"}, 21}
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
const unsigned long FETCH_INTERVAL = 60UL * 1000; // 1 minute
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
  int err = client.get("/");
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
        stopArrivals[k][j].stopRef = allowedStops[k].stopRefs[j];
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
        const char* stopRef = allowedStops[k].stopRefs[j];
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
  }

  // Alternate direction every 5 seconds
  if (millis() - lastDirectionSwap >= DIRECTION_INTERVAL) {
    currentDirection = !currentDirection;
    lastDirectionSwap = millis();
    displayPage(currentPage, currentDirection);
  }

  // Check button for page cycling (active LOW with pull-up)
  static bool lastButtonState = HIGH;
  static unsigned long lastDebounceTime = 0;
  bool buttonState = digitalRead(BUTTON_PIN);
  if (buttonState == LOW && lastButtonState == HIGH && (millis() - lastDebounceTime) > 200) {
    currentPage = (currentPage + 1) % NUM_DISPLAY_PAGES;
    currentDirection = false;
    lastDirectionSwap = millis();
    displayPage(currentPage, currentDirection);
    lastDebounceTime = millis();
  }
  lastButtonState = buttonState;

  delay(5);
}
