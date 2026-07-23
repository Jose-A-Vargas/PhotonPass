#include <Arduino.h>
#include <Wire.h>
#include "display.h"

static uint32_t gCount = 0;

void setup() {
    Serial.begin(115200);
    delay(500);
    display.begin();
    display.showMessage("PhotonPass", "Touch to test");
    Serial.println("v3 — touch test with counter");
}

void loop() {
    TouchPoint tp = display.readTouch();
    if (!tp.pressed) { delay(20); return; }

    display.waitRelease();

    gCount++;
    char buf[24];
    snprintf(buf, sizeof(buf), "x%u y%u  #%lu", tp.x, tp.y, gCount);
    Serial.println(buf);

    char line2[16];
    snprintf(line2, sizeof(line2), "tap %lu", gCount);
    display.showMessage("Touch", line2);
}
