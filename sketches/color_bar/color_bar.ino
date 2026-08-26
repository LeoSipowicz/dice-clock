#include "waveshare_lcd_port.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== LCD Color Bar Test Starting ===");
    Serial.println("Initializing LCD...");
    
    waveshare_lcd_init();
    
    Serial.println("LCD initialized successfully!");
    Serial.println("Color bar test pattern displayed on screen.");
    Serial.println("Pattern shows: Blue - Green - Red color bars");
}

void loop() {
    delay(2000);
    Serial.println("LCD test running...");
}
