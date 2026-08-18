#include <Arduino.h>

#define LED_PIN 4

void setup() {
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);

    Serial.println("External LED test starting");
}

void loop() {
    Serial.println("ON");
    digitalWrite(LED_PIN, HIGH);
    delay(1000);

    Serial.println("OFF");
    digitalWrite(LED_PIN, LOW);
    delay(1000);
}