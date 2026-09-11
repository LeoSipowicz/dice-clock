// No-op stubs for the NEW ESP-IDF I2C driver. ESP32_Display_Panel uses the
// legacy driver, which aborts at startup if the new one is linked; these
// satisfy LovyanGFX's references so it never gets pulled in. Never called.
#include <esp_err.h>
#include <driver/i2c_master.h>

extern "C" {

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config,
                             i2c_master_bus_handle_t *ret_bus_handle) {
    (void)bus_config;
    (void)ret_bus_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle) {
    (void)bus_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

}
