#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== Hello World ===");
    Serial.println("Serial communication test successful!");
}

void loop() {
    delay(1000);
    Serial.println("Hello from ESP32-S3!");
}
