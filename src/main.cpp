#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    Serial.println("Hello from my clock!");
}

void loop() {
    Serial.println("The clock is running...");
    delay(1000);
}