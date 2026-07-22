#include <Arduino.h>
#include <Wire.h>
#include "display.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    display.begin();

    Serial.printf("Free heap before 1st render: %u\n", ESP.getFreeHeap());
    display.showMessage("PhotonPass", "Touch to test");
    Serial.printf("Free heap after  1st render: %u\n", ESP.getFreeHeap());
    Serial.println("Setup done.");
}

void loop() {
    TouchPoint tp = display.readTouch();
    if (!tp.pressed) { delay(20); return; }

    // Wait for lift
    while (display.readTouch().pressed) delay(10);
    delay(40);

    Serial.printf("Touch at x=%u y=%u | heap=%u\n", tp.x, tp.y, ESP.getFreeHeap());

    // Test 1: push screen WITHOUT any text — just a clear white flash.
    // If this crashes, the problem is in epd_draw_grayscale_image itself.
    // If this succeeds, the problem is in write_mode / glyph decompression.
    display.fillScreen(false);   // white fb
    display.refresh();           // push — no text drawn

    Serial.printf("After refresh: heap=%u\n", ESP.getFreeHeap());
}
