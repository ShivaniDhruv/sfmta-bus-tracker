// Helper function to calculate minutes between two ISO8601 time strings
#include <TimeLib.h>
#include <Arduino.h>

// Helper to parse ISO8601 string (yyyy-mm-ddThh:mm:ssZ or with offset) to time_t (UTC)
time_t parseISO8601(const char* iso8601) {
  int year, month, day, hour, minute, second;
  // Only support Zulu time or no offset for simplicity
  if (sscanf(iso8601, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return 0; // parsing failed
  }
  tmElements_t tm;
  tm.Year = year - 1970;
  tm.Month = month;
  tm.Day = day;
  tm.Hour = hour;
  tm.Minute = minute;
  tm.Second = second;
  return makeTime(tm);
}

int getMinutesUntilArrival(const char* expectedArrival, const char* responseTimestamp) {
  time_t arrivalTime = parseISO8601(expectedArrival);
  time_t nowTime = parseISO8601(responseTimestamp);
  if (arrivalTime == 0 || nowTime == 0) return -1;
  return (arrivalTime - nowTime) / 60;
}

#include <WiFiNINA.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

// Name of the server we want to connect to
const char kHostname[] = "api.511.org";

// Number of milliseconds to wait without receiving any data before we give up
const int kNetworkTimeout = 5*60*1000; // 5 minutes
// Number of milliseconds to wait if no data is available before trying again
const int kNetworkDelay = 1000;

// ------------------------
WiFiSSLClient wifi;
HttpClient client = HttpClient(wifi, kHostname, 443);

void connectWiFi() {
  Serial.print("\nConnecting to WiFi");
  while (WiFi.begin(WIFI_SSID, WIFI_PASS) != WL_CONNECTED) {
    Serial.print(".");
    delay(1500);
  }
  Serial.println("\nWiFi connected!");
}

void fetchBusData() {
  Serial.println("\nFetching SFMTA bus data...");

  int err =0;
  String kPath = "/transit/StopMonitoring?"
                "api_key=" API_KEY
                "&agency=SF"
                "&format=json";
  // Serial.print(kPath);

  Serial.println("making GET request");
  client.beginRequest();
  err = client.get(kPath);
  client.sendHeader("Accept-Encoding", "identity");
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

  while(client.headerAvailable())
  {
   String headerName = client.readHeaderName();
   Serial.print("Header: ");
   Serial.print(headerName);
   String headerValue = client.readHeaderValue();
   Serial.print(" = ");
   Serial.println(headerValue);
  }

  int bodyLen = client.contentLength();
  Serial.print("Content length is: ");
  Serial.println(bodyLen);

  // Now we've got to the body, so we can print it out
  unsigned long timeoutStart = millis();
  unsigned long fetchStart = timeoutStart;

  // Allowed stops map
  struct AllowedStop {
    const char* lineRef;
    const char* stopRefs[25]; // max 25 stops per line
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
  const int allowedLines = sizeof(allowedStops)/sizeof(allowedStops[0]);

  // Use ArduinoJson's streaming parser to avoid storing the full response
  JsonDocument doc;

  // Fixed char arrays instead of String - avoids heap reallocation on every character.
  // preBuf is a rolling 256-byte window; large enough that neither search pattern
  // (~50 chars max) can be lost when the buffer rolls (keeps 128 bytes per roll).
  // charbuf holds one visit object at a time and is reset after each parse.
  static char charbuf[2048];
  static char preBuf[256];
  static char responseTimestampBuf[64];
  int bufLen = 0;     charbuf[0] = '\0';
  int preBufLen = 0;  preBuf[0] = '\0';
  responseTimestampBuf[0] = '\0';
  bool foundTimestamp = false;
  int visitCount = 0;
  bool inVisits = false;
  bool accumulatingVisit = false;
  int visitBracketDepth = 0;

  // For each allowed stop, store up to 3 shortest arrival times (in minutes)
  const int MAX_ARRIVALS_PER_STOP = 3;
  struct StopArrival {
    const char* stopRef;
    int arrivalMinutes[MAX_ARRIVALS_PER_STOP];
    int count;
  };
  // For each allowed line, for each stop, keep a StopArrival
  StopArrival stopArrivals[sizeof(allowedStops)/sizeof(allowedStops[0])][25];
  // Initialize
  for (int k = 0; k < allowedLines; k++) {
    for (int j = 0; j < allowedStops[k].stopCount; j++) {
      stopArrivals[k][j].stopRef = allowedStops[k].stopRefs[j];
      for (int m = 0; m < MAX_ARRIVALS_PER_STOP; m++) stopArrivals[k][j].arrivalMinutes[m] = 9999;
      stopArrivals[k][j].count = 0;
    }
  }

  bool inJsonString = false;
  bool escapeNext = false;

  Serial.println("\nScanning stops");
  unsigned long lastProgressMs = 0;

  while ((client.connected() || client.available()) && !client.endOfBodyReached() && ((millis() - timeoutStart) < kNetworkTimeout)) {
    int avail = client.available();
    if (avail <= 0) continue;
    uint8_t chunk[128];
    int toRead = min(avail, (int)sizeof(chunk));
    int bytesRead = client.read(chunk, toRead);
    unsigned long now = millis();
    if (inVisits && now - lastProgressMs >= 10000) {
      Serial.print(".");
      lastProgressMs = now;
    }
    for (int i = 0; i < bytesRead; i++) {
      char c = (char)chunk[i];

      if (!inVisits) {
        // Append to rolling pre-visit buffer; when full, discard the oldest half
        if (preBufLen < (int)sizeof(preBuf) - 1) {
          preBuf[preBufLen++] = c;
          preBuf[preBufLen] = '\0';
        } else {
          int half = sizeof(preBuf) / 2;
          memmove(preBuf, preBuf + half, preBufLen - half + 1);
          preBufLen -= half;
          preBuf[preBufLen++] = c;
          preBuf[preBufLen] = '\0';
        }

        // Search the small rolling buffer - O(window) instead of O(total received)
        if (!foundTimestamp) {
          char* tsKey = strstr(preBuf, "\"ResponseTimestamp\":\"");
          if (tsKey) {
            char* valStart = tsKey + 21; // skip past the opening quote of the value
            char* valEnd = strchr(valStart, '"');
            if (valEnd && (valEnd - valStart) < (int)sizeof(responseTimestampBuf)) {
              int len = valEnd - valStart;
              memcpy(responseTimestampBuf, valStart, len);
              responseTimestampBuf[len] = '\0';
              foundTimestamp = true;
              Serial.print("Found ResponseTimestamp: ");
              Serial.println(responseTimestampBuf);
            }
          }
        }

        if (strstr(preBuf, "\"MonitoredStopVisit\":[")) {
          inVisits = true;
          bufLen = 0;
          charbuf[0] = '\0';
        }
      } else if (!accumulatingVisit) {
        if (c == '{') {
          accumulatingVisit = true;
          visitBracketDepth = 1;
          inJsonString = false;
          escapeNext = false;
          charbuf[0] = '{';
          bufLen = 1;
          charbuf[1] = '\0';
        }
      } else {
        // Accumulate visit object into fixed buffer - no heap allocation
        if (bufLen < (int)sizeof(charbuf) - 1) {
          charbuf[bufLen++] = c;
          charbuf[bufLen] = '\0';
        }

        // Track string state so brackets inside strings are not counted.
        // Use c directly - no need to index back into the buffer.
        if (escapeNext) {
          escapeNext = false;
        } else if (c == '\\') {
          escapeNext = true;
        } else if (c == '"') {
          inJsonString = !inJsonString;
        } else if (!inJsonString) {
          if (c == '{') visitBracketDepth++;
          else if (c == '}') visitBracketDepth--;
        }

        if (visitBracketDepth == 0) {
          accumulatingVisit = false;
          DeserializationError parseErr = deserializeJson(doc, charbuf, bufLen);
          if (!parseErr) {
            const char* lineRef = doc["MonitoredVehicleJourney"]["LineRef"];
            const char* stopPointRef = doc["MonitoredVehicleJourney"]["MonitoredCall"]["StopPointRef"];
            const char* expectedArrival = doc["MonitoredVehicleJourney"]["MonitoredCall"]["ExpectedArrivalTime"];
            if (!expectedArrival) {
              expectedArrival = doc["MonitoredVehicleJourney"]["MonitoredCall"]["AimedArrivalTime"];
            }
            if (lineRef && stopPointRef && expectedArrival && responseTimestampBuf[0] != '\0') {
              for (int k = 0; k < allowedLines; k++) {
                if (strcmp(lineRef, allowedStops[k].lineRef) == 0) {
                  for (int j = 0; j < allowedStops[k].stopCount; j++) {
                    if (strcmp(stopPointRef, allowedStops[k].stopRefs[j]) == 0) {
                      int minutes = getMinutesUntilArrival(expectedArrival, responseTimestampBuf);
                      StopArrival* sa = &stopArrivals[k][j];
                      if (minutes >= 0) {
                        int pos = sa->count;
                        if (pos < MAX_ARRIVALS_PER_STOP) {
                          sa->arrivalMinutes[pos] = minutes;
                          sa->count++;
                        } else if (minutes < sa->arrivalMinutes[MAX_ARRIVALS_PER_STOP-1]) {
                          sa->arrivalMinutes[MAX_ARRIVALS_PER_STOP-1] = minutes;
                        }
                        for (int m = 0; m < sa->count-1; m++) {
                          for (int n = m+1; n < sa->count; n++) {
                            if (sa->arrivalMinutes[m] > sa->arrivalMinutes[n]) {
                              int tmp = sa->arrivalMinutes[m];
                              sa->arrivalMinutes[m] = sa->arrivalMinutes[n];
                              sa->arrivalMinutes[n] = tmp;
                            }
                          }
                        }
                        if (sa->count > MAX_ARRIVALS_PER_STOP) sa->count = MAX_ARRIVALS_PER_STOP;
                      }
                      visitCount++;
                    }
                  }
                }
              }
            }
          }
          bufLen = 0;
          charbuf[0] = '\0';
        }
      }
    }
  }

  Serial.println(" done.");

  unsigned long fetchElapsed = millis() - fetchStart;
  Serial.print("Fetch time: ");
  Serial.print(fetchElapsed / 1000);
  Serial.print(".");
  Serial.print((fetchElapsed % 1000) / 100);
  Serial.println("s");

  // Print results for debugging
  Serial.print("Filtered visits count: ");
  Serial.println(visitCount);
  Serial.print("ResponseTimestamp: ");
  Serial.println(responseTimestampBuf);
  for (int k = 0; k < allowedLines; k++) {
    Serial.print("Line "); Serial.print(allowedStops[k].lineRef); Serial.println(":");
    for (int j = 0; j < allowedStops[k].stopCount; j++) {
      StopArrival* sa = &stopArrivals[k][j];
      Serial.print(sa->stopRef); Serial.print(": ");
      for (int m = 0; m < sa->count; m++) {
        Serial.print(sa->arrivalMinutes[m]);
        if (m < sa->count-1) Serial.print(", ");
      }
      Serial.println();
    }
  }
  // Now stopArrivals contains up to 3 shortest arrival times (in minutes) for each allowed stop
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }

  connectWiFi();
}

void loop() {
  // put your main code here, to run repeatedly:
  fetchBusData();
  delay(10*60*1000);  // refresh every 10 minutes
}
