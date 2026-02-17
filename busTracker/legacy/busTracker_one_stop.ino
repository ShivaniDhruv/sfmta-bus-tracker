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

#define API_KEY "5ec99f17-0411-486a-80b5-396bd502168c"

// -------- CONFIG --------
const char WIFI_SSID[] = "^_^";
const char WIFI_PASS[] = "johnharris";

// Name of the server we want to connect to
const char kHostname[] = "api.511.org";

// Number of milliseconds to wait without receiving any data before we give up
const int kNetworkTimeout = 30*1000;
// Number of milliseconds to wait if no data is available before trying again
const int kNetworkDelay = 1000;

// ------------------------
WiFiClient wifi;
HttpClient client = HttpClient(wifi, kHostname);

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
//  Serial.println();
//  Serial.println("Body returned follows:");

  // Now we've got to the body, so we can print it out
  unsigned long timeoutStart = millis();

  String response = "";
  char c;
  int count = 0;
  // Whilst we haven't timed out & haven't reached the end of the body
  while ( (client.connected() || client.available()) &&
          (!client.endOfBodyReached()) &&
          ((millis() - timeoutStart) < kNetworkTimeout) )
  {
      if (client.available())
      {
          c = client.read();
          response += c;
          count++;
          // Print out this character
          Serial.println(count);
          Serial.println(c);
          
          // We read something, reset the timeout counter
          timeoutStart = millis();

         
      }
      else
      {
          // We haven't got any data, so let's pause to allow some to
          // arrive
          delay(kNetworkDelay);
      }
  }

  Serial.println("Grabbed all the data");

  JsonDocument doc;

  // Remove BOM if present
  if (response.length() > 0 && (unsigned char)response[0] == 0xEF) {
    // Check for UTF-8 BOM: 0xEF,0xBB,0xBF
    if (response.length() > 2 && (unsigned char)response[1] == 0xBB && (unsigned char)response[2] == 0xBF) {
      response = response.substring(3);
    }
  }


  // Parse JSON object
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    client.stop();
    return;
  }

  Serial.println("Parsed JSON successfully.");

//  // Get ResponseTimestamp
//  const char* responseTimestamp = doc["ServiceDelivery"]["StopMonitoringDelivery"]["ResponseTimestamp"];
//  // Navigate to the array of visits
//  JsonArray visits = doc["ServiceDelivery"]["StopMonitoringDelivery"]["MonitoredStopVisit"].as<JsonArray>();
//  if (!visits.isNull()) {
//    Serial.println("\nExpected Arrival Times:");
//    for (JsonObject visit : visits) {
//      const char* expected = visit["MonitoredVehicleJourney"]["MonitoredCall"]["ExpectedArrivalTime"];
//      if (expected && strlen(expected) > 0 && responseTimestamp && strlen(responseTimestamp) > 0) {
//        int minutes = getMinutesUntilArrival(expected, responseTimestamp);
//        Serial.print("Arrival in ");
//        if (minutes >= 0) {
//          Serial.print(minutes);
//          Serial.println(" min");
//        } else {
//          Serial.print("unknown time (parse error)");
//        }
//        Serial.print(" (at: ");
//        Serial.print(expected);
//        Serial.println(")");
//      }
//    }
//  } else {
//    Serial.println("No arrivals found.");
//  }
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
  delay(600000);  // refresh every 60 seconds
}
