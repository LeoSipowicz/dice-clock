/*
 * Custom board configuration for the Waveshare ESP32-S3-Touch-LCD-4.3B.
 *
 * Reuses the library's official 4.3B preset but disables the touch panel,
 * which the dice clock does not need. (The GT911 init fails on this board and
 * would abort board->begin(), leaving the backlight off and the screen black.)
 */

#pragma once

// *INDENT-OFF*

#define ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM  (1)

#if ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM

#include <board/supported/waveshare/BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3_B.h>

// The preset enables GT911 touch; turn it off for this sketch.
#undef  ESP_PANEL_BOARD_USE_TOUCH
#define ESP_PANEL_BOARD_USE_TOUCH           (0)

#endif // ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////// File Version ///////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MAJOR 1
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MINOR 2
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_PATCH 0

// *INDENT-ON*
