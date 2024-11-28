#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "managers/gps_manager.h"
#include "esp_log.h"
#include "sys/time.h"
#include "driver/uart.h"
#include "core/callbacks.h"
#include "vendor/GPS/MicroNMEA.h"
#include "vendor/GPS/gps_logger.h"
#include "managers/settings_manager.h"
#include <managers/views/terminal_screen.h>


static const char *GPS_TAG = "GPS";

nmea_parser_handle_t nmea_hdl;

gps_date_t cacheddate = {0};

void gps_manager_init(GPSManager* manager) {
    
    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();

    uint8_t current_rx_pin = settings_get_gps_rx_pin(&G_Settings);

    if (current_rx_pin != 0)
    {   
        config.uart.rx_pin = current_rx_pin;
    }
        
#ifdef CONFIG_IS_GHOST_BOARD
    config.uart.rx_pin = 2;
#endif

    nmea_hdl = nmea_parser_init(&config);

    nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);

    manager->isinitilized = true;
    
    if (csv_file_open("gps_data") == ESP_OK) {
        printf("CSV file opened\n"
               "for GPS logging\n");
    } else {
        printf("Failed to open\n"
               "CSV file for GPS\n");
    }
}

void gps_manager_deinit(GPSManager* manager) {
    if (manager->isinitilized) {
        nmea_parser_remove_handler(nmea_hdl, gps_event_handler);
        nmea_parser_deinit(nmea_hdl);
        csv_file_close();
        printf("CSV file closed\n"
               "GPS logging stopped\n");

        manager->isinitilized = false;
    }
}

esp_err_t gps_manager_log_wardriving_data(wardriving_data_t* data) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!gps) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!gps->valid) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(data->ssid) <= 2) {
        return ESP_OK;
    }

    if (cacheddate.year <= 0)
    {
        if (gps->date.year > 100 ||
            gps->date.month < 1 || gps->date.month > 12 || 
            gps->date.day < 1 || gps->date.day > 31) {
            if (rand() % 20 == 0) {
                printf("Warning: GPS date\n"
                       "out of range:\n"
                       "%04d-%02d-%02d\n",
                       2000 + gps->date.year, 
                       gps->date.month, 
                       gps->date.day);
            }
            return ESP_OK;
        }
    }

    if (cacheddate.year <= 0)
    {
        cacheddate = gps->date;     // if we pass this check cache the year to avoid missing data
    }

    
    if (gps->tim.hour > 23 || gps->tim.minute > 59 || gps->tim.second > 59) {
        if (rand() % 20 == 0) {
            printf("Warning: GPS time\n"
                   "invalid:\n"
                   "%02d:%02d:%02d\n",
                   gps->tim.hour, 
                   gps->tim.minute, 
                   gps->tim.second);
        }
        return ESP_OK;
    }

    
    if (gps->latitude < -90.0 || gps->latitude > 90.0 || 
        gps->longitude < -180.0 || gps->longitude > 180.0) {
        if (rand() % 20 == 0) {
            printf("Warning: GPS coords\n"
                   "out of range:\n"
                   "Lat: %f\n"
                   "Lon: %f\n",
                   gps->latitude, 
                   gps->longitude);
        }
        return ESP_OK;
    }

    
    if (gps->speed < 0.0 || gps->speed > 340.0) {
        if (rand() % 20 == 0) {
            printf("Warning: GPS speed\n"
                   "out of range:\n"
                   "%f m/s\n", 
                   gps->speed);
        }
        return ESP_OK;
    }

    
    if (gps->dop_h < 0.0 || gps->dop_p < 0.0 || gps->dop_v < 0.0 || 
        gps->dop_h > 50.0 || gps->dop_p > 50.0 || gps->dop_v > 50.0) {
        if (rand() % 20 == 0) {
            printf("Warning: GPS DOP\n"
                   "out of range:\n"
                   "HDOP: %f\n"
                   "PDOP: %f\n"
                   "VDOP: %f\n",
                   gps->dop_h, 
                   gps->dop_p, 
                   gps->dop_v);
        }
        return ESP_OK;
    }

    esp_err_t ret = csv_write_data_to_buffer(data);
    if (ret != ESP_OK) {
        printf("Failed to write\n"
               "wardriving data\n"
               "to CSV buffer\n");
        return ret;
    }

    if (rand() % 2 == 0) {
        printf("Wrote to buffer\n"
               "Satellites: %u\n", 
               gps->sats_in_view);
        TERMINAL_VIEW_ADD_TEXT("Wrote to buffer\n"
                              "Satellites: %u\n", 
                              gps->sats_in_view);
    }

    return ret;
}