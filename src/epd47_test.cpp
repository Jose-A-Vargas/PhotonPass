#include <Arduino.h>
#include "display.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    display.begin();
}

void loop() {
    char buf[128] = {};

    display.keyboard(buf, sizeof(buf), "Search:", Display::KeyboardMode::QWERTY);
    Serial.printf("[KB] got: '%s'\n", buf);
    display.showMessage("Got:", buf);
    delay(2000);

    display.keyboard(buf, sizeof(buf), "No space:", Display::KeyboardMode::QWERTY_NO_SPACE);
    Serial.printf("[KB] got: '%s'\n", buf);
    display.showMessage("Got:", buf);
    delay(2000);

    display.keyboard(buf, sizeof(buf), "PIN:", Display::KeyboardMode::NUMPAD);
    Serial.printf("[KB] got: '%s'\n", buf);
    display.showMessage("PIN:", buf);
    delay(2000);
}
