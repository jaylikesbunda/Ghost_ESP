#include "vendor/drivers/pcf8563.h"
#include "i2c_shared.h"
#include "io_manager/i2c_bus_lock.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "RTC_DRIVER";

// These Kconfig options only exist when HAS_RTC_CLOCK is enabled, but this driver
// is compiled for every board. On boards without an RTC, ghost_rtc_init is never
// called (its only caller is guarded by CONFIG_HAS_RTC_CLOCK), so these are pure
// build-time placeholders that are never reached. Even when it is called, the RTC
// rides an already-created shared I2C bus, so the pins are reused-not-created and
// ignored -- hence a "not connected" sentinel rather than a real pin number.
#ifndef CONFIG_RTC_I2C_SDA_PIN
#define CONFIG_RTC_I2C_SDA_PIN (-1)
#endif
#ifndef CONFIG_RTC_I2C_SCL_PIN
#define CONFIG_RTC_I2C_SCL_PIN (-1)
#endif

static i2c_port_num_t rtc_i2c_port;
static uint8_t rtc_address;
static rtc_chip_type_t rtc_chip;
static i2c_master_bus_handle_t rtc_i2c_bus;
static i2c_master_dev_handle_t rtc_i2c_dev;

static uint8_t _bcd_to_dec(uint8_t val) {
  return ((val / 16 * 10) + (val % 16));
}

static uint8_t _dec_to_bcd(uint8_t val) {
  return ((val / 10 * 16) + (val % 10));
}

// Generic RTC initialization
esp_err_t ghost_rtc_init(i2c_port_num_t i2c_port, uint8_t addr, rtc_chip_type_t chip_type) {
  rtc_i2c_port = i2c_port;
  rtc_address = addr;
  rtc_chip = chip_type;

  esp_err_t ret = i2c_shared_get_or_create_bus(rtc_i2c_port,
                                               CONFIG_RTC_I2C_SDA_PIN,
                                               CONFIG_RTC_I2C_SCL_PIN,
                                               true,
                                               &rtc_i2c_bus,
                                               NULL);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus %d not available: %s", (int)rtc_i2c_port, esp_err_to_name(ret));
    rtc_i2c_bus = NULL;
    return ret;
  }
  if (rtc_i2c_dev) {
    i2c_master_bus_rm_device(rtc_i2c_dev);
    rtc_i2c_dev = NULL;
  }
  ret = i2c_shared_add_device(rtc_i2c_bus, rtc_address, 100000, &rtc_i2c_dev);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to attach RTC device 0x%02X on I2C bus %d: %s",
             rtc_address, (int)rtc_i2c_port, esp_err_to_name(ret));
    rtc_i2c_dev = NULL;
    return ret;
  }
  
  ESP_LOGI(TAG, "RTC initialized: chip=%d, addr=0x%02X, port=%d", chip_type, addr, i2c_port);
  
  // For DS1307, ensure clock is running (clear CH bit)
  if (chip_type == RTC_CHIP_DS1307 || chip_type == RTC_CHIP_DS3231) {
    uint8_t sec_reg;
    ret = i2c_master_transmit_receive(rtc_i2c_dev,
                                      (uint8_t[]){DS1307_SEC_REG}, 1,
                                      &sec_reg, 1, 1000);
    if (ret == ESP_OK && (sec_reg & DS1307_CH_MASK)) {
      // Clear clock halt bit
      sec_reg &= ~DS1307_CH_MASK;
      uint8_t data[] = {DS1307_SEC_REG, sec_reg};
      i2c_master_transmit(rtc_i2c_dev, data, sizeof(data), 1000);
      ESP_LOGI(TAG, "DS1307 clock started");
    }
  }
  
  return ESP_OK;
}

// Legacy PCF8563 initialization for backward compatibility
esp_err_t pcf8563_init(i2c_port_num_t i2c_port, uint8_t addr) {
  return ghost_rtc_init(i2c_port, addr, RTC_CHIP_PCF8563);
}

static esp_err_t _read_register(uint8_t reg, uint8_t *data, size_t len) {
  if (!rtc_i2c_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  bool locked = i2c_bus_lock(rtc_i2c_port, 1000);
  esp_err_t ret = i2c_master_transmit_receive(rtc_i2c_dev, &reg, 1, data, len, 1000);
  if (locked) i2c_bus_unlock(rtc_i2c_port);
  return ret;
}

static esp_err_t _write_register(uint8_t reg, const uint8_t *data, size_t len) {
  if (!rtc_i2c_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  uint8_t buffer[1 + len];
  buffer[0] = reg;
  memcpy(&buffer[1], data, len);
  bool locked = i2c_bus_lock(rtc_i2c_port, 1000);
  esp_err_t ret = i2c_master_transmit(rtc_i2c_dev, buffer, sizeof(buffer), 1000);
  if (locked) i2c_bus_unlock(rtc_i2c_port);
  return ret;
}

esp_err_t rtc_set_datetime(const RTC_Date *datetime) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    // PCF8563 register order starting at 0x02: seconds, minutes, hours, days,
    // weekdays, months/century, years. Weekday is not tracked here but must
    // occupy register 0x06 so month and year land in their correct slots.
    uint8_t data[7];
    data[0] = _dec_to_bcd(datetime->second) & ~PCF8563_VOL_LOW_MASK;
    data[1] = _dec_to_bcd(datetime->minute);
    data[2] = _dec_to_bcd(datetime->hour);
    data[3] = _dec_to_bcd(datetime->day);
    data[4] = 0; // weekday (register 0x06, not tracked)
    data[5] = _dec_to_bcd(datetime->month) |
              ((datetime->year < 2000) ? PCF8563_CENTURY_MASK : 0);
    data[6] = _dec_to_bcd(datetime->year % 100);
    return _write_register(PCF8563_SEC_REG, data, 7);
  } else {
    // DS1307/DS3231. Register order at 0x00: seconds, minutes, hours, weekday,
    // day-of-month, month, year. Weekday is auto-incremented by the chip at
    // midnight and does not affect date arithmetic, so a fixed valid value is
    // fine; range is 1-7.
    uint8_t data[7];
    data[0] = _dec_to_bcd(datetime->second) & ~DS1307_CH_MASK;
    data[1] = _dec_to_bcd(datetime->minute);
    data[2] = _dec_to_bcd(datetime->hour);
    data[3] = 1; // weekday placeholder (1-7 valid; not tracked)
    data[4] = _dec_to_bcd(datetime->day);
    data[5] = _dec_to_bcd(datetime->month);
    data[6] = _dec_to_bcd(datetime->year % 100);
    esp_err_t ret = _write_register(DS1307_SEC_REG, data, 7);
    if (ret == ESP_OK && rtc_chip == RTC_CHIP_DS3231) {
      // A successful time write means the oscillator is running; clear the
      // Oscillator Stop Flag so the next boot trusts the stored time.
      uint8_t status;
      if (_read_register(DS3231_STATUS_REG, &status, 1) == ESP_OK) {
        status &= ~DS3231_OSF_MASK;
        _write_register(DS3231_STATUS_REG, &status, 1);
      }
    }
    return ret;
  }
}

esp_err_t rtc_get_datetime(RTC_Date *datetime) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data[7];
    esp_err_t ret = _read_register(PCF8563_SEC_REG, data, 7);
    if (ret != ESP_OK) {
      return ret;
    }

    // Register order: 0x02 seconds, 0x03 minutes, 0x04 hours, 0x05 days,
    // 0x06 weekdays, 0x07 months/century, 0x08 years.
    datetime->second = _bcd_to_dec(data[0] & ~PCF8563_VOL_LOW_MASK);
    datetime->minute = _bcd_to_dec(data[1] & PCF8563_MINUTES_MASK);
    datetime->hour = _bcd_to_dec(data[2] & PCF8563_HOUR_MASK);
    datetime->day = _bcd_to_dec(data[3] & PCF8563_DAY_MASK);
    // data[4] is the weekday register; not tracked.
    bool century = data[5] & PCF8563_CENTURY_MASK;
    datetime->month = _bcd_to_dec(data[5] & PCF8563_MONTH_MASK);
    datetime->year = (century ? 1900 : 2000) + _bcd_to_dec(data[6]);
  } else {
    uint8_t data[7];
    esp_err_t ret = _read_register(DS1307_SEC_REG, data, 7);
    if (ret != ESP_OK) {
      return ret;
    }

    datetime->second = _bcd_to_dec(data[0] & ~DS1307_CH_MASK);
    datetime->minute = _bcd_to_dec(data[1]);
    datetime->hour = _bcd_to_dec(data[2] & 0x3F);
    datetime->day = _bcd_to_dec(data[4]);
    datetime->month = _bcd_to_dec(data[5] & 0x1F);
    datetime->year = 2000 + _bcd_to_dec(data[6]);
  }
  return ESP_OK;
}

esp_err_t rtc_set_alarm(const RTC_Alarm *alarm) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data[4];
    data[0] = (alarm->minute != 0xFF)
                  ? _dec_to_bcd(alarm->minute) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    data[1] = (alarm->hour != 0xFF)
                  ? _dec_to_bcd(alarm->hour) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    data[2] = (alarm->day != 0xFF)
                  ? _dec_to_bcd(alarm->day) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    data[3] = (alarm->weekday != 0xFF)
                  ? _dec_to_bcd(alarm->weekday) & ~PCF8563_VOL_LOW_MASK
                  : PCF8563_VOL_LOW_MASK;
    return _write_register(PCF8563_ALRM_MIN_REG, data, 4);
  } else {
    // DS1307/DS3231 alarm functionality is more complex and not implemented
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_get_alarm(RTC_Alarm *alarm) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data[4];
    esp_err_t ret = _read_register(PCF8563_ALRM_MIN_REG, data, 4);
    if (ret != ESP_OK) {
      return ret;
    }

    alarm->minute = (data[0] != PCF8563_VOL_LOW_MASK)
                        ? _bcd_to_dec(data[0] & PCF8563_MINUTES_MASK)
                        : 0xFF;
    alarm->hour = (data[1] != PCF8563_VOL_LOW_MASK)
                      ? _bcd_to_dec(data[1] & PCF8563_HOUR_MASK)
                      : 0xFF;
    alarm->day = (data[2] != PCF8563_VOL_LOW_MASK)
                    ? _bcd_to_dec(data[2] & PCF8563_DAY_MASK)
                    : 0xFF;
    alarm->weekday = (data[3] != PCF8563_VOL_LOW_MASK)
                        ? _bcd_to_dec(data[3] & PCF8563_WEEKDAY_MASK)
                        : 0xFF;
    return ESP_OK;
  } else {
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_enable_alarm() {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data;
    esp_err_t ret = _read_register(PCF8563_STAT2_REG, &data, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    data |= (1 << 1);
    return _write_register(PCF8563_STAT2_REG, &data, 1);
  } else {
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_disable_alarm() {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data;
    esp_err_t ret = _read_register(PCF8563_STAT2_REG, &data, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    data &= ~(1 << 1);
    return _write_register(PCF8563_STAT2_REG, &data, 1);
  } else {
    ESP_LOGW(TAG, "Alarm not implemented for DS1307/DS3231");
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t rtc_check_voltage_low(bool *voltage_low) {
  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t data;
    esp_err_t ret = _read_register(PCF8563_SEC_REG, &data, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    *voltage_low = data& PCF8563_VOL_LOW_MASK;
    return ESP_OK;
  } else {
    *voltage_low = false;
    return ESP_OK;
  }
}

esp_err_t rtc_check_time_valid(bool *time_valid) {
  if (!time_valid) {
    return ESP_ERR_INVALID_ARG;
  }

  if (rtc_chip == RTC_CHIP_PCF8563) {
    uint8_t sec;
    esp_err_t ret = _read_register(PCF8563_SEC_REG, &sec, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    *time_valid = (sec & PCF8563_VOL_LOW_MASK) == 0;
    return ESP_OK;
  } else if (rtc_chip == RTC_CHIP_DS3231) {
    uint8_t status;
    esp_err_t ret = _read_register(DS3231_STATUS_REG, &status, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    *time_valid = (status & DS3231_OSF_MASK) == 0;
    return ESP_OK;
  } else {
    uint8_t sec;
    esp_err_t ret = _read_register(DS1307_SEC_REG, &sec, 1);
    if (ret != ESP_OK) {
      return ret;
    }
    *time_valid = (sec & DS1307_CH_MASK) == 0;
    return ESP_OK;
  }
}

// Legacy functions for backward compatibility
esp_err_t pcf8563_set_datetime(const RTC_Date *datetime) {
  return rtc_set_datetime(datetime);
}

esp_err_t pcf8563_get_datetime(RTC_Date *datetime) {
  return rtc_get_datetime(datetime);
}

esp_err_t pcf8563_set_alarm(const RTC_Alarm *alarm) {
  return rtc_set_alarm(alarm);
}

esp_err_t pcf8563_get_alarm(RTC_Alarm *alarm) {
  return rtc_get_alarm(alarm);
}

esp_err_t pcf8563_enable_alarm(void) {
  return rtc_enable_alarm();
}

esp_err_t pcf8563_disable_alarm(void) {
  return rtc_disable_alarm();
}

esp_err_t pcf8563_check_voltage_low(bool *voltage_low) {
  return rtc_check_voltage_low(voltage_low);
}
