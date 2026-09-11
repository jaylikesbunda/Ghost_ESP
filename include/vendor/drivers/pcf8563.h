#ifndef PCF8563_H
#define PCF8563_H

#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define PCF8563_SLAVE_ADDRESS (0x51) // 7-bit I2C Address for PCF8563
#define DS1307_SLAVE_ADDRESS (0x68)  // 7-bit I2C Address for DS1307/DS3231

// Register map for PCF8563
#define PCF8563_STAT1_REG (0x00)
#define PCF8563_STAT2_REG (0x01)
#define PCF8563_SEC_REG (0x02)
#define PCF8563_MIN_REG (0x03)
#define PCF8563_HR_REG (0x04)
#define PCF8563_DAY_REG (0x05)
#define PCF8563_WEEKDAY_REG (0x06)
#define PCF8563_MONTH_REG (0x07)
#define PCF8563_YEAR_REG (0x08)
#define PCF8563_ALRM_MIN_REG (0x09)
#define PCF8563_SQW_REG (0x0D)
#define PCF8563_TIMER1_REG (0x0E)
#define PCF8563_TIMER2_REG (0x0F)

// Register map for DS1307/DS3231
#define DS1307_SEC_REG (0x00)
#define DS1307_MIN_REG (0x01)
#define DS1307_HR_REG (0x02)
#define DS1307_DAY_REG (0x03)
#define DS1307_DATE_REG (0x04)
#define DS1307_MONTH_REG (0x05)
#define DS1307_YEAR_REG (0x06)
#define DS1307_CONTROL_REG (0x07)

// DS3231-only registers (time registers 0x00-0x06 match DS1307)
#define DS3231_STATUS_REG (0x0F)
#define DS3231_OSF_MASK (0x80) // Oscillator Stop Flag in the status register

// DS1307 bit masks
#define DS1307_CH_MASK (0x80)    // Clock Halt bit in seconds register
#define DS1307_12H_MASK (0x40)   // 12-hour mode bit
#define DS1307_PM_MASK (0x20)    // PM bit in 12-hour mode

// PCF8563 bit masks
#define PCF8563_VOL_LOW_MASK (0x80)
#define PCF8563_MINUTES_MASK (0x7F)
#define PCF8563_HOUR_MASK (0x3F)
#define PCF8563_DAY_MASK (0x3F)
#define PCF8563_WEEKDAY_MASK (0x07)
#define PCF8563_MONTH_MASK (0x1F)
#define PCF8563_CENTURY_MASK (0x80)

// Struct for date and time
typedef struct {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} RTC_Date;

// Struct for alarm settings
typedef struct {
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t weekday;
} RTC_Alarm;

// RTC chip types
typedef enum {
  RTC_CHIP_PCF8563,
  RTC_CHIP_DS1307,
  RTC_CHIP_DS3231
} rtc_chip_type_t;

// Functions
esp_err_t ghost_rtc_init(i2c_port_num_t i2c_port, uint8_t addr, rtc_chip_type_t chip_type);
esp_err_t rtc_set_datetime(const RTC_Date *datetime);
esp_err_t rtc_get_datetime(RTC_Date *datetime);
esp_err_t rtc_set_alarm(const RTC_Alarm *alarm);
esp_err_t rtc_get_alarm(RTC_Alarm *alarm);
esp_err_t rtc_enable_alarm(void);
esp_err_t rtc_disable_alarm(void);
esp_err_t rtc_check_voltage_low(bool *voltage_low);
esp_err_t rtc_check_time_valid(bool *time_valid);

// Legacy functions for backward compatibility
esp_err_t pcf8563_init(i2c_port_num_t i2c_port, uint8_t addr);
esp_err_t pcf8563_set_datetime(const RTC_Date *datetime);
esp_err_t pcf8563_get_datetime(RTC_Date *datetime);
esp_err_t pcf8563_set_alarm(const RTC_Alarm *alarm);
esp_err_t pcf8563_get_alarm(RTC_Alarm *alarm);
esp_err_t pcf8563_enable_alarm(void);
esp_err_t pcf8563_disable_alarm(void);
esp_err_t pcf8563_check_voltage_low(bool *voltage_low);

#endif // PCF8563_H
