#include <stdio.h>
#include <string.h>
#include "platform.h"

//Use the timeout from the config, the default timeout is -1
#if CONFIG_VL53L8CX_I2C_TIMEOUT == false
#define VL53L8CX_I2C_TIMEOUT (-1)
#else
#define VL53L8CX_I2C_TIMEOUT CONFIG_VL53L8CX_I2C_TIMEOUT_VALUE
#endif

#ifdef CONFIG_VL53L8CX_RESET_PIN_HIGH
#define VL53L8CX_RESET_LEVEL 1
#elif CONFIG_VL53L8CX_RESET_PIN_LOW
#define VL53L8CX_RESET_LEVEL 0
#endif

// ================= WRITE =================

uint8_t VL53L8CX_WrMulti(
    VL53L8CX_Platform *p_platform,
    uint16_t RegisterAdress,
    uint8_t *p_values,
    uint32_t size)
{
    if (!p_platform || !p_platform->handle || (!p_values && size > 0)) {
        printf("WrMulti: invalid args\n");
        return 1;
    }

    i2c_master_transmit_multi_buffer_info_t i2c_buffers[2];

    uint8_t reg_buf[2] = {
        (uint8_t)(RegisterAdress >> 8),
        (uint8_t)(RegisterAdress & 0xFF)
    };

    i2c_buffers[0].write_buffer = reg_buf;
    i2c_buffers[0].buffer_size = 2;

    i2c_buffers[1].write_buffer = p_values;
    i2c_buffers[1].buffer_size = size;

    esp_err_t err = i2c_master_multi_buffer_transmit(
        p_platform->handle,
        i2c_buffers,
        2,
        VL53L8CX_I2C_TIMEOUT
    );

    if (err != ESP_OK) {
        printf("WrMulti ERR=%d REG=0x%X SIZE=%lu\n", err, RegisterAdress, size);
    }

    return (uint8_t)err;
}

uint8_t VL53L8CX_WrByte(
    VL53L8CX_Platform *p_platform,
    uint16_t RegisterAdress,
    uint8_t value)
{
    return VL53L8CX_WrMulti(p_platform, RegisterAdress, &value, 1);
}

// ================= READ =================

uint8_t VL53L8CX_RdMulti(
    VL53L8CX_Platform *p_platform,
    uint16_t RegisterAdress,
    uint8_t *p_values,
    uint32_t size)
{
    if (!p_platform || !p_platform->handle || (!p_values && size > 0)) {
        printf("RdMulti: invalid args\n");
        return 1;
    }

    uint8_t reg_buf[2] = {
        (uint8_t)(RegisterAdress >> 8),
        (uint8_t)(RegisterAdress & 0xFF)
    };

    esp_err_t err = i2c_master_transmit_receive(
        p_platform->handle,
        reg_buf,
        2,
        p_values,
        size,
        VL53L8CX_I2C_TIMEOUT
    );

    if (err != ESP_OK) {
        printf("RdMulti ERR=%d\n", err);
        return (uint8_t)err;
    }

    return 0;
}

uint8_t VL53L8CX_RdByte(
    VL53L8CX_Platform *p_platform,
    uint16_t RegisterAdress,
    uint8_t *p_value)
{
    return VL53L8CX_RdMulti(p_platform, RegisterAdress, p_value, 1);
}

// ================= RESET =================

uint8_t VL53L8CX_Reset_Sensor(VL53L8CX_Platform* p_platform)
{
    gpio_set_direction(p_platform->reset_gpio, GPIO_MODE_OUTPUT);

    gpio_set_level(p_platform->reset_gpio, VL53L8CX_RESET_LEVEL);
    VL53L8CX_WaitMs(p_platform, 100);

    gpio_set_level(p_platform->reset_gpio, !VL53L8CX_RESET_LEVEL);
    VL53L8CX_WaitMs(p_platform, 100);

    return ESP_OK;
}

// ================= UTIL =================

void VL53L8CX_SwapBuffer(uint8_t *buffer, uint16_t size)
{
    uint32_t i;
    uint8_t tmp[4];

    for (i = 0; i < size; i += 4) {
        tmp[0] = buffer[i + 3];
        tmp[1] = buffer[i + 2];
        tmp[2] = buffer[i + 1];
        tmp[3] = buffer[i];

        memcpy(&buffer[i], tmp, 4);
    }
}

uint8_t VL53L8CX_WaitMs(
    VL53L8CX_Platform *p_platform,
    uint32_t TimeMs)
{
    (void)p_platform;
    vTaskDelay(TimeMs / portTICK_PERIOD_MS);
    return ESP_OK;
}
