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
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                    const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle) {
    (void)bus_handle;
    (void)dev_config;
    (void)ret_handle;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle) {
    (void)handle;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_get_bus_handle(i2c_port_num_t port_num,
                                    i2c_master_bus_handle_t *ret_handle) {
    (void)port_num;
    (void)ret_handle;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address,
                           int xfer_timeout_ms) {
    (void)bus_handle;
    (void)address;
    (void)xfer_timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t i2c_dev, uint8_t *read_buffer,
                             size_t read_size, int xfer_timeout_ms) {
    (void)i2c_dev;
    (void)read_buffer;
    (void)read_size;
    (void)xfer_timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev,
                              const uint8_t *write_buffer, size_t write_size,
                              int xfer_timeout_ms) {
    (void)i2c_dev;
    (void)write_buffer;
    (void)write_size;
    (void)xfer_timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t i2c_dev,
                                      const uint8_t *write_buffer, size_t write_size,
                                      uint8_t *read_buffer, size_t read_size,
                                      int xfer_timeout_ms) {
    (void)i2c_dev;
    (void)write_buffer;
    (void)write_size;
    (void)read_buffer;
    (void)read_size;
    (void)xfer_timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
}
