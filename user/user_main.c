/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2015 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP8266 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_common.h"
#include "esp_wifi.h"
#include "uart.h"
#include "apps/sntp.h"

#include "iot_export.h"
#include "aliyun_port.h"
#include "aliyun_config.h"
#include "ota.h"
#include "mqtt.h"

int got_ip_flag = 0;
ota_info_t *p_ota_info = NULL;

/******************************************************************************
 * FunctionName : user_rf_cal_sector_set
 * Description  : SDK just reversed 4 sectors, used for rf init data and paramters.
 *                We add this function to force users to set rf cal sector, since
 *                we don't know which sector is free in user's application.
 *                sector map for last several sectors : ABCCC
 *                A : rf cal
 *                B : rf init data
 *                C : sdk parameters
 * Parameters   : none
 * Returns      : rf cal sector
*******************************************************************************/
uint32 user_rf_cal_sector_set(void)
{
    flash_size_map size_map = system_get_flash_size_map();
    uint32 rf_cal_sec = 0;

    switch (size_map) {
    case FLASH_SIZE_4M_MAP_256_256:
        rf_cal_sec = 128 - 5;
        break;

    case FLASH_SIZE_8M_MAP_512_512:
        rf_cal_sec = 256 - 5;
        break;

    case FLASH_SIZE_16M_MAP_512_512:
    case FLASH_SIZE_16M_MAP_1024_1024:
        rf_cal_sec = 512 - 5;
        break;

    case FLASH_SIZE_32M_MAP_512_512:
    case FLASH_SIZE_32M_MAP_1024_1024:
        rf_cal_sec = 1024 - 5;
        break;

    case FLASH_SIZE_64M_MAP_1024_1024:
        rf_cal_sec = 2048 - 5;
        break;

    case FLASH_SIZE_128M_MAP_1024_1024:
        rf_cal_sec = 4096 - 5;
        break;

    default:
        rf_cal_sec = 0;
        break;
    }

    return rf_cal_sec;
}

void sntpfn()
{
#if START_SNTP
    printf("Initializing SNTP\n");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);

// for set more sntp server, plz modify macro SNTP_MAX_SERVERS in sntp_opts.h file
    sntp_setservername(0, "202.112.29.82");        // set sntp server after got ip address, you had better to adjust the sntp server to your area
//    sntp_setservername(1, "time-a.nist.gov");
//    sntp_setservername(2, "ntp.sjtu.edu.cn");
//    sntp_setservername(3, "0.nettime.pool.ntp.org");
//    sntp_setservername(4, "time-b.nist.gov");
//    sntp_setservername(5, "time-a.timefreq.bldrdoc.gov");
//    sntp_setservername(6, "time-b.timefreq.bldrdoc.gov");
//    sntp_setservername(7, "time-c.timefreq.bldrdoc.gov");
//    sntp_setservername(8, "utcnist.colorado.edu");
//    sntp_setservername(9, "time.nist.gov");

    sntp_init();

    while (1) {
        u32_t ts = 0;
        ts = sntp_get_current_timestamp();
        printf("current time : %s\n", sntp_get_real_time(ts));

        if (ts == 0) {
            printf("did not get a valid time from sntp server\n");
        } else {
            break;
        }

        vTaskDelay(TASK_CYCLE / portTICK_RATE_MS);
    }

#endif
}

// WiFi callback function
void event_handler(System_Event_t *event)
{
    switch (event->event_id) {
    case EVENT_STAMODE_GOT_IP:
        printf("WiFi connected\n");
        sntpfn();
        got_ip_flag = 1;
        break;

    case EVENT_STAMODE_DISCONNECTED:
        printf("WiFi disconnected, try to connect...\n");
        got_ip_flag = 0;
        wifi_station_connect();
        break;

    default:
        break;
    }
}

void initialize_wifi(void)
{
    wifi_set_opmode(STATION_MODE);

    // set AP parameter
    struct station_config config;
    bzero(&config, sizeof(struct station_config));
    sprintf(config.ssid, WIFI_SSID);
    sprintf(config.password, WIFI_PASSWORD);
    wifi_station_set_config(&config);

    wifi_station_set_auto_connect(true);
    wifi_station_set_reconnect_policy(true);
    wifi_set_event_handler_cb(event_handler);
}

void heap_check_task(void *para)
{
    while (1) {
        vTaskDelay(TASK_CYCLE / portTICK_RATE_MS);
        printf("[heap check task] free heap size:%d\n", system_get_free_heap_size());
    }
}

void main_process(void *para)
{
    extern unsigned int max_content_len;    // maxium fragment length in bytes, more info see as RFC 6066: part 4
    max_content_len = 4 * 1024;

    hal_micros_set_default_time();  // startup millisecond timer, get millisecond timestamp by hal_millis() interface
    printf("SDK version:%s \n", system_get_sdk_version());
    printf("\n******************************************  \n  SDK compile time:%s %s\n******************************************\n\n", __DATE__, __TIME__);


#if HEAP_CHECK_TASK
    xTaskCreate(heap_check_task, "heap_check_task", 128, NULL, 5, NULL);
#endif

    initialize_wifi();

    if (DEFAULT_TASK_MODE == REMOTE_OTA_TASK) {
        p_ota_info = (ota_info_t *)zalloc(sizeof(ota_info_t));

        if (system_param_load(PARAM_SAVE_SEC, 0, p_ota_info, sizeof(ota_info_t)) != true) {
            print_error("para load");
        }

        printf("ota flag:%u\n", p_ota_info->ota_flag);

        if (p_ota_info->ota_flag == 1) {
            printf("bin size:%u\n", p_ota_info->bin_size);
            printf("latest version:%s\n", p_ota_info->latest_version);
            printf("hostname:%s\n", p_ota_info->hostname);
            printf("ota path:%s\n", p_ota_info->ota_path);
            p_ota_info->ota_flag = 0;

            if (system_param_save_with_protect(PARAM_SAVE_SEC, p_ota_info, sizeof(ota_info_t)) != true) {
                print_error("para save error");
            }

            if (xTaskCreate(remote_ota_upgrade_task, "remote_ota_upgrade_task", 2048, NULL, 5, NULL) != pdPASS) {   // start remote ota upgrade
                print_error("remote upgrade task");
            }

            printf("remote ota upgrade task start...\n");
            vTaskDelete(NULL);
        }
    }

    switch (DEFAULT_TASK_MODE) {
    case MQTT_TASK:
        if (xTaskCreate(mqtt_task, "mqtt_task", 1500, NULL, 5, NULL) != pdPASS) {
            print_error("mqtt task");
        }
        break;

    case LOCAL_OTA_TASK:
        if (xTaskCreate(local_ota_task, "local_ota_task", 1500, NULL, 5, NULL) != pdPASS) {
            print_error("lota task");
        }
        break;

    case REMOTE_OTA_TASK:
        if (xTaskCreate(remote_ota_task, "remote_ota_task", 1500, NULL, 5, NULL) != pdPASS) {
            print_error("rota task");
        }
        break;

    default:
        printf("task mode error happened!\n");
        break;
    }

    vTaskDelete(NULL);
}

void user_init(void)
{
    // default baudrate: 74880, change it if necessary
//    UART_SetBaudrate(0, 115200);
//    UART_SetBaudrate(1, 115200);

    if (xTaskCreate(main_process, "main_process", 1024, NULL, 5, NULL) != pdPASS) {
        print_error("main process");
    }
}


