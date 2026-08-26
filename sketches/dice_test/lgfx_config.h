#ifndef LGFX_CONFIG_H
#define LGFX_CONFIG_H

// LovyanGFX config for the Waveshare ESP32-S3 4.3" RGB LCD.
// Panel: ST7262, 800x480, 16-bit parallel RGB.
// Pins and timings match the values from waveshare_lcd_port.h.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// RGB panel classes are not pulled in by LovyanGFX.hpp. Include them explicitly.
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

class LGFX : public lgfx::LGFX_Device {
public:
    lgfx::Bus_RGB   _bus_instance;
    lgfx::Panel_RGB _panel_instance;

    LGFX(void) {
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width  = 800;
            cfg.memory_height = 480;
            cfg.panel_width   = 800;
            cfg.panel_height  = 480;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _panel_instance.config_detail();
            cfg.use_psram = 1; // framebuffer lives in PSRAM
            _panel_instance.config_detail(cfg);
        }

        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;

            // Blue: DATA0..DATA4
            cfg.pin_d0  = GPIO_NUM_14;
            cfg.pin_d1  = GPIO_NUM_38;
            cfg.pin_d2  = GPIO_NUM_18;
            cfg.pin_d3  = GPIO_NUM_17;
            cfg.pin_d4  = GPIO_NUM_10;
            // Green: DATA5..DATA10
            cfg.pin_d5  = GPIO_NUM_39;
            cfg.pin_d6  = GPIO_NUM_0;
            cfg.pin_d7  = GPIO_NUM_45;
            cfg.pin_d8  = GPIO_NUM_48;
            cfg.pin_d9  = GPIO_NUM_47;
            cfg.pin_d10 = GPIO_NUM_21;
            // Red: DATA11..DATA15
            cfg.pin_d11 = GPIO_NUM_1;
            cfg.pin_d12 = GPIO_NUM_2;
            cfg.pin_d13 = GPIO_NUM_42;
            cfg.pin_d14 = GPIO_NUM_41;
            cfg.pin_d15 = GPIO_NUM_40;

            cfg.pin_henable = GPIO_NUM_5;  // DE
            cfg.pin_vsync   = GPIO_NUM_3;
            cfg.pin_hsync   = GPIO_NUM_46;
            cfg.pin_pclk    = GPIO_NUM_7;
            cfg.freq_write  = 16000000;    // 16 MHz

            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 8;
            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 8;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 8;
            cfg.pclk_active_neg   = 1;
            cfg.de_idle_high      = 0;
            cfg.pclk_idle_high    = 0;

            _bus_instance.config(cfg);
        }

        _panel_instance.setBus(&_bus_instance);
        setPanel(&_panel_instance);
    }
};

#endif // LGFX_CONFIG_H
