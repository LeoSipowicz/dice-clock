#ifndef __LCD_PORT_H
#define __LCD_PORT_H
#pragma once
#include <Arduino.h>
#include <esp_display_panel.hpp>
#define ESP_PANEL_USE_1024_600_LCD (0)
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_SDA_IO 8
#define I2C_MASTER_SCL_IO 9
#define TP_RST 1
#define LCD_BL 2
#define LCD_RST 3
#define SD_CS 4
#define USB_SEL 5
#define EXAMPLE_LCD_NAME                        ST7262
#if ESP_PANEL_USE_1024_600_LCD
    #define EXAMPLE_LCD_WIDTH                       (1024)
    #define EXAMPLE_LCD_HEIGHT                      (600)
#else
    #define EXAMPLE_LCD_WIDTH                       (800)
    #define EXAMPLE_LCD_HEIGHT                      (480)
#endif
#define EXAMPLE_LCD_COLOR_BITS                  (16)
#define EXAMPLE_LCD_RGB_DATA_WIDTH              (16)
#define EXAMPLE_LCD_RGB_COLOR_BITS          (16)
#define EXAMPLE_LCD_RGB_TIMING_FREQ_HZ          (16 * 1000 * 1000)
#if ESP_PANEL_USE_1024_600_LCD
    #define EXAMPLE_LCD_RGB_TIMING_HPW              (30)
    #define EXAMPLE_LCD_RGB_TIMING_HBP              (145)
    #define EXAMPLE_LCD_RGB_TIMING_HFP              (170)
    #define EXAMPLE_LCD_RGB_TIMING_VPW              (2)
    #define EXAMPLE_LCD_RGB_TIMING_VBP              (23)
    #define EXAMPLE_LCD_RGB_TIMING_VFP              (12)
#else
    #define EXAMPLE_LCD_RGB_TIMING_HPW              (4)
    #define EXAMPLE_LCD_RGB_TIMING_HBP              (8)
    #define EXAMPLE_LCD_RGB_TIMING_HFP              (8)
    #define EXAMPLE_LCD_RGB_TIMING_VPW              (4)
    #define EXAMPLE_LCD_RGB_TIMING_VBP              (8)
    #define EXAMPLE_LCD_RGB_TIMING_VFP              (8)
#endif
#define EXAMPLE_LCD_RGB_BOUNCE_BUFFER_SIZE  (EXAMPLE_LCD_WIDTH * 10)
#define EXAMPLE_LCD_RGB_IO_DISP            (-1)
#define EXAMPLE_LCD_RGB_IO_VSYNC           (3)
#define EXAMPLE_LCD_RGB_IO_HSYNC           (46)
#define EXAMPLE_LCD_RGB_IO_DE              (5)
#define EXAMPLE_LCD_RGB_IO_PCLK            (7)
#define EXAMPLE_LCD_RGB_IO_DATA0           (14)
#define EXAMPLE_LCD_RGB_IO_DATA1           (38)
#define EXAMPLE_LCD_RGB_IO_DATA2           (18)
#define EXAMPLE_LCD_RGB_IO_DATA3           (17)
#define EXAMPLE_LCD_RGB_IO_DATA4           (10)
#define EXAMPLE_LCD_RGB_IO_DATA5           (39)
#define EXAMPLE_LCD_RGB_IO_DATA6           (0)
#define EXAMPLE_LCD_RGB_IO_DATA7           (45)
#if EXAMPLE_LCD_RGB_DATA_WIDTH > 8
#define EXAMPLE_LCD_RGB_IO_DATA8           (48)
#define EXAMPLE_LCD_RGB_IO_DATA9           (47)
#define EXAMPLE_LCD_RGB_IO_DATA10          (21)
#define EXAMPLE_LCD_RGB_IO_DATA11          (1)
#define EXAMPLE_LCD_RGB_IO_DATA12          (2)
#define EXAMPLE_LCD_RGB_IO_DATA13          (42)
#define EXAMPLE_LCD_RGB_IO_DATA14          (41)
#define EXAMPLE_LCD_RGB_IO_DATA15          (40)
#endif
#define EXAMPLE_LCD_RST_IO                 (-1)
#define EXAMPLE_LCD_BL_IO            (-1)
#define EXAMPLE_LCD_BL_ON_LEVEL           (1)
#define EXAMPLE_LCD_BL_OFF_LEVEL !EXAMPLE_LCD_BL_ON_LEVEL
#define EXAMPLE_LCD_ENABLE_CREATE_WITH_CONFIG   (0)
#define EXAMPLE_LCD_ENABLE_PRINT_FPS            (1)
#define EXAMPLE_LCD_ENABLE_DRAW_FINISH_CALLBACK (1)
#define _EXAMPLE_LCD_CLASS(name, ...) LCD_##name(__VA_ARGS__)
#define EXAMPLE_LCD_CLASS(name, ...)  _EXAMPLE_LCD_CLASS(name, ##__VA_ARGS__)
#if EXAMPLE_LCD_ENABLE_PRINT_FPS
#define EXAMPLE_LCD_PRINT_FPS_PERIOD_MS         (1000)
#define EXAMPLE_LCD_PRINT_FPS_COUNT_MAX         (50)
#endif
void waveshare_lcd_init();
#endif
