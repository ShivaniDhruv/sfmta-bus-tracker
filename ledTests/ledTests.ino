#include <Adafruit_Protomatter.h>

// MatrixPortal ESP32-S3
uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;

Adafruit_Protomatter matrix(
  32, 4, 1, rgbPins, 3, addrPins, clockPin, latchPin, oePin, false);

#define NUM_ROWS 16
#define NUM_COLS 13
#define TOTAL_LEDS (NUM_ROWS * NUM_COLS)

struct LedMapping {
  int row;
  int col;
  int lineRef;
  int stopNum;
};

LedMapping mappings[TOTAL_LEDS];
int mappingCount = 0;

void setup(void) {
  Serial.begin(9600);
  while (!Serial) { delay(10); }

  ProtomatterStatus status = matrix.begin();
  Serial.print("Protomatter begin() status: ");
  Serial.println((int)status);
  if (status != PROTOMATTER_OK) {
    for (;;);
  }

  matrix.fillScreen(0);
  matrix.show();

  Serial.println("=== LED Mapping Tool ===");
  Serial.println("For each lit LED, enter: lineRef stopNum");
  Serial.println("Enter 0 0 to skip an LED.");
  Serial.println();

  for (int row = 0; row < NUM_ROWS; row++) {
    for (int col = 0; col < NUM_COLS; col++) {
      // Clear previous LED
      matrix.fillScreen(0);
      // Light up current LED in white
      matrix.drawPixel(col, row, matrix.color565(255, 0, 0));
      matrix.show();

      Serial.print("LED at row=");
      Serial.print(row);
      Serial.print(" col=");
      Serial.print(col);
      Serial.println(" is lit. Enter lineRef and stopNum (0 0 to skip):");

      // Wait for serial input
      while (!Serial.available()) {
        delay(10);
      }

      String input = Serial.readStringUntil('\n');
      input.trim();

      int lineRef = 0, stopNum = 0;
      int spaceIdx = input.indexOf(' ');
      if (spaceIdx > 0) {
        lineRef = input.substring(0, spaceIdx).toInt();
        stopNum = input.substring(spaceIdx + 1).toInt();
      }

      if (lineRef != 0 || stopNum != 0) {
        mappings[mappingCount].row = row;
        mappings[mappingCount].col = col;
        mappings[mappingCount].lineRef = lineRef;
        mappings[mappingCount].stopNum = stopNum;
        mappingCount++;
      }
    }
  }

  // Turn off all LEDs
  matrix.fillScreen(0);
  matrix.show();

  // Print results
  Serial.println();
  Serial.println("=== Mapping Results ===");
  Serial.println("Format: {lineRef, stopNum} -> (row, col)");
  Serial.println();

  for (int i = 0; i < mappingCount; i++) {
    Serial.print("{lineRef=");
    Serial.print(mappings[i].lineRef);
    Serial.print(", stop=");
    Serial.print(mappings[i].stopNum);
    Serial.print("} -> (row=");
    Serial.print(mappings[i].row);
    Serial.print(", col=");
    Serial.print(mappings[i].col);
    Serial.println(")");
  }

  // Print as array format for easy copy-paste into busTracker
  Serial.println();
  Serial.println("=== Array for busTracker (allowedStops LED coords) ===");
  Serial.println("{");
  for (int i = 0; i < mappingCount; i++) {
    Serial.print("  {");
    Serial.print(mappings[i].lineRef);
    Serial.print(", ");
    Serial.print(mappings[i].stopNum);
    Serial.print(", ");
    Serial.print(mappings[i].row);
    Serial.print(", ");
    Serial.print(mappings[i].col);
    Serial.print("}");
    if (i < mappingCount - 1) Serial.print(",");
    Serial.println();
  }
  Serial.println("};");
}

void loop() {
  // Do nothing -- mapping complete
}
