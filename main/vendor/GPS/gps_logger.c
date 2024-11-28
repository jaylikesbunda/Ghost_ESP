#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "sys/time.h"
#include "driver/uart.h"
#include <errno.h>
#include <sys/stat.h>
#include "vendor/GPS/gps_logger.h"
#include "managers/gps_manager.h"
#include "managers/sd_card_manager.h"
#include "vendor/GPS/MicroNMEA.h"
#include "core/callbacks.h"

static const char *CSV_TAG = "CSV";



#define CSV_BUFFER_SIZE 512

static FILE *csv_file = NULL;
static char csv_buffer[BUFFER_SIZE];
static size_t buffer_offset = 0;

esp_err_t csv_write_header(FILE* f) {
    const char* header = "BSSID,SSID,Latitude,Longitude,RSSI,Channel,Encryption,Time\n";

    if (f == NULL) {
        const char* mark_begin = "[BUF/BEGIN]";
        const char* mark_close = "[BUF/CLOSE]";
        uart_write_bytes(UART_NUM_0, mark_begin, strlen(mark_begin));
        uart_write_bytes(UART_NUM_0, header, strlen(header));
        uart_write_bytes(UART_NUM_0, mark_close, strlen(mark_close));
        uart_write_bytes(UART_NUM_0, "\n", 1);
        return ESP_OK;
    } else {
        size_t written = fwrite(header, 1, strlen(header), f);
        return (written == strlen(header)) ? ESP_OK : ESP_FAIL;
    }
}

void get_next_csv_file_name(char *file_name_buffer, const char* base_name) {
    int next_index = get_next_csv_file_index(base_name);  // Modify this to be CSV specific
    snprintf(file_name_buffer, MAX_FILE_NAME_LENGTH, "/mnt/ghostesp/gps/%s_%d.csv", base_name, next_index);
}

esp_err_t csv_file_open(const char* base_file_name) {
    char file_name[MAX_FILE_NAME_LENGTH];


    if (sd_card_exists("/mnt/ghostesp/gps"))
    {
        get_next_csv_file_name(file_name, base_file_name);
        csv_file = fopen(file_name, "w");
    }

    esp_err_t ret = csv_write_header(csv_file);
    if (ret != ESP_OK) {
        printf("Failed to write CSV header.\n");
        fclose(csv_file);
        csv_file = NULL;
        return ret;
    }

    printf("CSV file opened: \n%s\n", 
           file_name);
    return ESP_OK;
}

esp_err_t csv_write_data_to_buffer(wardriving_data_t *data) {
    char timestamp[35];
    

    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             gps->date.year, gps->date.month, gps->date.day,
             gps->tim.hour, gps->tim.minute, gps->tim.second, gps->tim.thousand);


    char data_line[CSV_BUFFER_SIZE];
    int len = snprintf(data_line, CSV_BUFFER_SIZE, "%s,%s,%lf,%lf,%d,%d,%s,%s\n",
                       data->bssid, data->ssid, data->latitude, data->longitude,
                       data->rssi, data->channel, data->encryption_type, timestamp);


    if (buffer_offset + len > BUFFER_SIZE) {
        printf("Buffer full,\n"
               "flushing to file...\n");
        esp_err_t ret = csv_flush_buffer_to_file();
        if (ret != ESP_OK) {
            return ret;
        }
    }


    memcpy(csv_buffer + buffer_offset, data_line, len);
    buffer_offset += len;

    return ESP_OK;
}

esp_err_t csv_flush_buffer_to_file() {
    if (csv_file == NULL) {
        printf("CSV file not open.\n"
               "Flushing to Serial...\n");
        const char* mark_begin = "[BUF/BEGIN]";
        const char* mark_close = "[BUF/CLOSE]";

        uart_write_bytes(UART_NUM_0, mark_begin, strlen(mark_begin));
        uart_write_bytes(UART_NUM_0, csv_buffer, buffer_offset);
        uart_write_bytes(UART_NUM_0, mark_close, strlen(mark_close));
        uart_write_bytes(UART_NUM_0, "\n", 1);

        buffer_offset = 0;
        return ESP_OK;
    }

    size_t written = fwrite(csv_buffer, 1, buffer_offset, csv_file);
    if (written != buffer_offset) {
        printf("Failed to write\n"
               "buffer to file.\n");
        return ESP_FAIL;
    }

    printf("Flushed %zu bytes\n"
           "to CSV file.\n", 
           buffer_offset);
    buffer_offset = 0;

    return ESP_OK;
}

void csv_file_close() {
    if (csv_file != NULL) {
        if (buffer_offset > 0) {
            printf("Flushing buffer\n"
                   "before closing...\n");
            csv_flush_buffer_to_file();
        }
        fclose(csv_file);
        csv_file = NULL;
        printf("CSV file closed.\n");
    }
}