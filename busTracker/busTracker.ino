#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "Adafruit_LiquidCrystal.h"
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

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
const int allowedLines = sizeof(allowedStops) / sizeof(allowedStops[0]);

const int MAX_ARRIVALS_PER_STOP = 3;
struct StopArrival {
  const char* stopRef;
  int arrivalMinutes[3];
  int count;
};
static StopArrival stopArrivals[7][25]; // [allowedLines][max stops per line]

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
  {"24",  "14143", "Bayvw", "14142", "PacHt", 10},
  {"23",  "14200", "Bayvw", "14203", "SFZoo",  1},
  {"49",  "15614", "CtyCl", "15613", "FtMsn", 10},
  {"14",  "15614", "DlyCt", "15613", "Ferry", 10},
  {"14R", "15614", "DlyCt", "15613", "Ferry", 10},
  {"67",  "14690", "Bernl", "14568", "Missn",  5},
  {"J",   "16280", "Balbo", "16277", "Ferry", 15},
};
const int NUM_DISPLAY_PAGES = sizeof(displayPages) / sizeof(displayPages[0]);

// Button for cycling pages
const int BUTTON_PIN = 9;
int currentPage = 0;
bool currentDirection = false; // false = direction 1, true = direction 2
unsigned long lastDirectionSwap = 0;
const unsigned long DIRECTION_INTERVAL = 5000; // 5 seconds
unsigned long lastFetchTime = 0;
const unsigned long FETCH_INTERVAL = 60UL * 1000; // 1 minute

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

  // Row 1: line number + walk time
  lcd.clear();
  lcd.setCursor(0, 0);
  snprintf(row, 17, "%-3s %dm walk", p->lineRef, p->walkMinutes);
  lcd.print(row);

  // Row 2: direction label + arrival times
  const char* stopRef = direction ? p->stopRef2 : p->stopRef1;
  const char* label   = direction ? p->label2   : p->label1;
  StopArrival* sa = findArrival(p->lineRef, stopRef);

  char times[10] = "";
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

void connectWiFi() {
  Serial.print("\nConnecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1500);
  }
  Serial.println("\nWiFi connected!");
  wifi.setInsecure(); // skip SSL certificate verification
}

void fetchBusData() {
  Serial.println("\nFetching bus data from worker...");
  unsigned long fetchStart = millis();

  String kPath = "/?token=" WORKER_AUTH_TOKEN;

  client.beginRequest();
  int err = client.get(kPath);
  client.endRequest();
  if (err != 0) {
    Serial.print("HTTP GET failed, error: ");
    Serial.println(err);
    return;
  }

  int statusCode = client.responseStatusCode();
  if (statusCode != 200) {
    String response = client.responseBody();
    Serial.print("Status code: ");
    Serial.println(statusCode);
    Serial.print("Response: ");
    Serial.println(response);
    return;
  }

  String body = client.responseBody();
  Serial.print("Response size: ");
  Serial.println(body.length());

  // Initialize stopArrivals
  for (int k = 0; k < allowedLines; k++) {
    for (int j = 0; j < allowedStops[k].stopCount; j++) {
      stopArrivals[k][j].stopRef = allowedStops[k].stopRefs[j];
      for (int m = 0; m < MAX_ARRIVALS_PER_STOP; m++) stopArrivals[k][j].arrivalMinutes[m] = 9999;
      stopArrivals[k][j].count = 0;
    }
  }

  // Parse the compact JSON: {"24":{"14143":[5,12],"14142":[3]},...}
  JsonDocument doc;
  DeserializationError parseErr = deserializeJson(doc, body);
  if (parseErr) {
    Serial.print("JSON parse error: ");
    Serial.println(parseErr.c_str());
    return;
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
        sa->arrivalMinutes[count] = v.as<int>();
        count++;
      }
      sa->count = count;
      visitCount += count;
    }
  }

  unsigned long fetchElapsed = millis() - fetchStart;
  Serial.print("Fetch time: ");
  Serial.print(fetchElapsed / 1000);
  Serial.print(".");
  Serial.print((fetchElapsed % 1000) / 100);
  Serial.println("s");
  Serial.print("Arrival entries: ");
  Serial.println(visitCount);

  // Print results for debugging
  for (int k = 0; k < allowedLines; k++) {
    Serial.print("Line "); Serial.print(allowedStops[k].lineRef); Serial.println(":");
    for (int j = 0; j < allowedStops[k].stopCount; j++) {
      StopArrival* sa = &stopArrivals[k][j];
      if (sa->count == 0) continue;
      Serial.print(sa->stopRef); Serial.print(": ");
      for (int m = 0; m < sa->count; m++) {
        Serial.print(sa->arrivalMinutes[m]);
        if (m < sa->count-1) Serial.print(", ");
      }
      Serial.println();
    }
  }

  client.stop();
}

void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }

  // set up the LCD's number of rows and columns:
  if (!lcd.begin(16, 2)) {
    Serial.println("Could not init backpack. Check wiring.");
    while(1);
  }
  Serial.println("Backpack init'd.");
  lcd.setBacklight(HIGH);
  lcd.print("Connecting...");

  connectWiFi();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lcd.clear();
  lcd.print("Waiting for data");
}

void loop() {
  // Refresh data every FETCH_INTERVAL
  if (lastFetchTime == 0 || millis() - lastFetchTime >= FETCH_INTERVAL) {
    unsigned long fetchStart = millis();
    fetchBusData();
    unsigned long fetchElapsed = millis() - fetchStart;
    if (fetchElapsed > 60000) {
      lcd.clear();
      lcd.print("Fetch error:");
      lcd.setCursor(0, 1);
      lcd.print("took >1 min");
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
