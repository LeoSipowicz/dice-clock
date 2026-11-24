#ifndef LGFX_CONFIG_H
#define LGFX_CONFIG_H
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#define TFT_PIN_CS   10
#define TFT_PIN_DC    9
#define TFT_PIN_RST   8
#define TFT_PIN_BL    7
#define TFT_PIN_MOSI 11
#define TFT_PIN_SCLK 12
#define TFT_PIN_MISO 13
#define TFT_SPI_WRITE_HZ 20000000
#define TFT_SPI_READ_HZ   8000000
#define TFT_SPI_MODE 0
#define TFT_INVERT false
#define TFT_RGB_ORDER false
#define TFT_PANEL_HX8357D 1
#define TFT_PANEL_HX8357B 2
#define TFT_PANEL_ILI9488 3
#define TFT_PANEL_ILI9486 4
#define TFT_PANEL_ILI9481 5
#define TFT_PANEL_ST7796  6
#ifndef TFT_PANEL
    #define TFT_PANEL TFT_PANEL_HX8357D
#endif
#if TFT_PANEL == TFT_PANEL_HX8357D
    typedef lgfx::Panel_HX8357D PanelType;
#elif TFT_PANEL == TFT_PANEL_HX8357B
    typedef lgfx::Panel_HX8357B PanelType;
#elif TFT_PANEL == TFT_PANEL_ILI9488
    typedef lgfx::Panel_ILI9488 PanelType;
#elif TFT_PANEL == TFT_PANEL_ILI9486
    typedef lgfx::Panel_ILI9486 PanelType;
#elif TFT_PANEL == TFT_PANEL_ILI9481
    typedef lgfx::Panel_ILI9481 PanelType;
#elif TFT_PANEL == TFT_PANEL_ST7796
    typedef lgfx::Panel_ST7796 PanelType;
#else
    #error "TFT_PANEL is not a known panel"
#endif
class LGFX : public lgfx::LGFX_Device {
public:
    lgfx::Bus_SPI  _bus_instance;
    PanelType      _panel_instance;
    lgfx::Light_PWM _light_instance;
    LGFX(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = TFT_SPI_MODE;
            cfg.freq_write = TFT_SPI_WRITE_HZ;
            cfg.freq_read  = TFT_SPI_READ_HZ;
            cfg.spi_3wire  = (TFT_PIN_MISO < 0);
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = TFT_PIN_SCLK;
            cfg.pin_mosi = TFT_PIN_MOSI;
            cfg.pin_miso = TFT_PIN_MISO;
            cfg.pin_dc   = TFT_PIN_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs   = TFT_PIN_CS;
            cfg.pin_rst  = TFT_PIN_RST;
            cfg.pin_busy = -1;
            cfg.memory_width  = 320;
            cfg.memory_height = 480;
            cfg.panel_width   = 320;
            cfg.panel_height  = 480;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            cfg.offset_rotation = 0;
            cfg.invert    = TFT_INVERT;
            cfg.rgb_order = TFT_RGB_ORDER;
            cfg.readable   = (TFT_PIN_MISO >= 0);
            cfg.bus_shared = false;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl      = TFT_PIN_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }
        setPanel(&_panel_instance);
    }
};
#endif
