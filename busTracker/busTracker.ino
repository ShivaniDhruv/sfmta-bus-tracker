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
const int kNetworkTimeout = 30*1000;
// Number of milliseconds to wait if no data is available before trying again
const int kNetworkDelay = 1000;

// ------------------------
WiFiSSLClient wifi;
HttpClient client = HttpClient(wifi, kHostname, 443);

void connectWiFi() {
  Serial.println(WiFi.firmwareVersion());
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
                "&stopCode=15572"
                "&format=json";
  // Serial.print(kPath);

  Serial.println("making GET request");
  client.beginRequest();
  err = client.get(kPath);
  client.sendHeader("Accept-Encoding", "identity");
  client.endRequest();

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
    {"14R", {"16498", "15529", "15536", "15543", "15836", "15552", "15566", "15572", "15614", "15592", "15588", "15693", "15530", "15535", "15542", "17299", "15551", "15565", "15571", "15613", "15593", "17099"}, 22},
    {"67", {"17532", "17746", "14686", "17924", "14688", "14690", "13476", "17552", "14697", "13710", "14687", "14568"}, 12},
    {"J", {"17217", "16994", "16995", "16997", "16996", "18059", "16214", "18156", "16280", "14788", "15418", "16992", "15731", "15417", "15727", "15419", "14006", "16215", "18155", "16277", "14787", "17778"}, 21}
  };
  const int allowedLines = sizeof(allowedStops)/sizeof(allowedStops[0]);

  // Use ArduinoJson's streaming parser to avoid storing the full response
  StaticJsonDocument<2048> doc; // Adjust size as needed for a single visit
  String responseTimestamp = "";
  bool foundTimestamp = false;
  int visitCount = 0;
  // Read the response stream character by character
  String buffer = "";
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

  while ((client.connected() || client.available()) && !client.endOfBodyReached() && ((millis() - timeoutStart) < kNetworkTimeout)) {
    if (client.available()) {
      char c = client.read();
      buffer += c;

      // Look for ResponseTimestamp as soon as possible
      if (!foundTimestamp) {
        int idx = buffer.indexOf("\"ResponseTimestamp\":");
        if (idx != -1) {
          int quoteStart = buffer.indexOf('"', idx + 20);
          if (quoteStart != -1) {
            int quoteEnd = buffer.indexOf('"', quoteStart + 1);
            if (quoteEnd != -1) {
              responseTimestamp = buffer.substring(quoteStart + 1, quoteEnd);
              Serial.println("Found ResponseTimestamp: " + responseTimestamp);
              foundTimestamp = true;
            }
          }
        }
      }

      // Look for start of MonitoredStopVisit array
      if (!inVisits) {
        int arrIdx = buffer.indexOf("\"MonitoredStopVisit\":[");
        if (arrIdx != -1) {
          inVisits = true;
          int afterArr = arrIdx + String("\"MonitoredStopVisit\":[").length();
          buffer = buffer.substring(afterArr);
          buffer.trim();
        }
      }

      // Efficient bracket depth tracking
      if (inVisits) {
        char cc = buffer[buffer.length() - 1];
        if (!accumulatingVisit) {
          if (cc == '{') {
            accumulatingVisit = true;
            visitBracketDepth = 1;
            buffer = buffer.substring(buffer.length() - 1);
            // Serial.println("Buffer at after first {: " + buffer);
            // Serial.println("Current char: " + String(cc));
            // Serial.println("Visit bracket depth: " + String(visitBracketDepth));
          }
        } else {
          if (cc == '{') visitBracketDepth++;
          if (cc == '}') visitBracketDepth--;
          // Serial.println("Buffer at after increment/decrement: " + buffer);
          // Serial.println("Current char: " + String(cc));
          // Serial.println("Visit bracket depth: " + String(visitBracketDepth));
          if (visitBracketDepth == 0) {
            // Serial.println("Completed visit object: " + buffer);
            accumulatingVisit = false;
            // Deserialize and filter
            DeserializationError err = deserializeJson(doc, buffer);
            if (!err) {
              const char* lineRef = doc["MonitoredVehicleJourney"]["LineRef"];
              const char* stopPointRef = doc["MonitoredVehicleJourney"]["MonitoredCall"]["StopPointRef"];
              const char* expectedArrival = doc["MonitoredVehicleJourney"]["MonitoredCall"]["ExpectedArrivalTime"];
              // Only process if expectedArrival and responseTimestamp are available
              if (lineRef && stopPointRef && expectedArrival && responseTimestamp.length() > 0) {
                for (int k = 0; k < allowedLines; k++) {
                  if (strcmp(lineRef, allowedStops[k].lineRef) == 0) {
                    for (int j = 0; j < allowedStops[k].stopCount; j++) {
                      if (strcmp(stopPointRef, allowedStops[k].stopRefs[j]) == 0) {
                        int minutes = getMinutesUntilArrival(expectedArrival, responseTimestamp.c_str());
                        // Insert minutes into sorted array for this stop (keep 3 shortest)
                        StopArrival* sa = &stopArrivals[k][j];
                        if (minutes >= 0) {
                          // Insert in sorted order
                          int pos = sa->count;
                          if (pos < MAX_ARRIVALS_PER_STOP) {
                            sa->arrivalMinutes[pos] = minutes;
                            sa->count++;
                          } else if (minutes < sa->arrivalMinutes[MAX_ARRIVALS_PER_STOP-1]) {
                            sa->arrivalMinutes[MAX_ARRIVALS_PER_STOP-1] = minutes;
                          }
                          // Sort
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
                        // Serial.println("Matched visit: Line " + String(lineRef) + " Stop " + String(stopPointRef) + " Minutes: " + String(minutes));
                      }
                    }
                  }
                }
              }
            }
            buffer = "";
          }
        }
      }
    }
  }

  // Print results for debugging
  Serial.print("Filtered visits count: ");
  Serial.println(visitCount);
  Serial.print("ResponseTimestamp: ");
  Serial.println(responseTimestamp);
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
  delay(60000);  // refresh every 60 seconds
}
