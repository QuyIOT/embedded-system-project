#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "MFRC522.h"
#include "esp_log.h"
#include "output.h"
#include "input.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#include "nvs.h"
#include "nvs_flash.h"

#define RELAY_PIN           2
#define BUZZER_PIN          15
#define LED_GREEN_PIN       33
#define LED_RED_PIN         32
#define BUTTON_PIN          34
#define IRQ          14

static const char *TAG = "RFID_APP";
static spi_device_handle_t spi;
static QueueHandle_t control_queue;

#define BUTTON_CHECK_MS     50   // Check button every 50ms
#define BUTTON_HOLD_2S      2000 // 2 seconds
#define BUTTON_HOLD_5S      5000 // 5 seconds

#define MAX_UIDS 10  // Maximum number of UIDs
#define MAX_UID_LEN 7  // Maximum UID length in bytes
#define UID_STR_LEN (2 * MAX_UID_LEN + 1)  // Hex string length (2 chars per byte + null terminator)

// Arrays to store UID strings (single dimension)
char registered_uids[MAX_UIDS * UID_STR_LEN];  // Single array for all registered UID strings
int registered_count = 0;  // Count of registered UIDs
char unregistered_uids[MAX_UIDS * UID_STR_LEN];  // Single array for all unregistered UID strings
int unregistered_count = 0;  // Count of unregistered UIDs

#define EXAMPLE_ESP_WIFI_SSID      "RFID_DOOR_LOCK"
#define EXAMPLE_ESP_WIFI_PASS      "12345678"
#define EXAMPLE_ESP_WIFI_CHANNEL   6
#define EXAMPLE_MAX_STA_CONN       4

/* Define MIN macro */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
static httpd_handle_t server = NULL; // HTTP server handle

// Message types for control queue
typedef enum {
    CARD_VALID,
    CARD_INVALID,
    SERVER_TOGGLE 
} app_event_t;


// Lưu UID vào NVS
void save_uid_to_nvs(const char *uid, int index) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), "uid_%d", index); // Tạo key cho UID
    err = nvs_set_str(nvs_handle, key, uid);    // Lưu UID vào NVS
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save UID %d to NVS: %s", index, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved UID %s to NVS at index %d", uid, index);
    }

    // Commit thay đổi
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit changes to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

// Xóa UID khỏi NVS
void remove_uid_from_nvs(const char *uid) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return;
    }

    char key[16];
    for (int i = 0; i < registered_count; i++) {
        if (strcmp(&registered_uids[i * UID_STR_LEN], uid) == 0) {
            snprintf(key, sizeof(key), "uid_%d", i);
            esp_err_t err = nvs_erase_key(nvs_handle, key);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Removed UID %s from NVS", uid);
            } else {
                ESP_LOGE(TAG, "Failed to remove UID %s from NVS: %s", uid, esp_err_to_name(err));
            }
            break;
        }
    }

    // Commit thay đổi
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit changes to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

// Tải UID từ NVS khi khởi động
void load_uids_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return;
    }

    // Tải từng UID từ NVS
    char key[16];
    for (int i = 0; i < MAX_UIDS; i++) {
        snprintf(key, sizeof(key), "uid_%d", i);
        size_t required_size = UID_STR_LEN;
        err = nvs_get_str(nvs_handle, key, &registered_uids[i * UID_STR_LEN], &required_size);
        if (err == ESP_OK) {
            registered_count++;
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            break; // Không còn UID nào để tải
        } else {
            ESP_LOGE(TAG, "Failed to load UID %d from NVS: %s", i, esp_err_to_name(err));
        }
    }

    nvs_close(nvs_handle);
}


// Initialize SPI and RFID RC522
static void init_rfid_rc522(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num = 19,
        .mosi_io_num = 23,
        .sclk_io_num = 18,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 5000000,  // 5 MHz
        .mode = 0,                  // SPI mode 0
        .spics_io_num = 5,         // CS pin
        .queue_size = 7
    };

    // Initialize SPI bus
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, 1);
    ESP_ERROR_CHECK(ret);

    // Attach RFID to SPI bus
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    // Initialize RC522
    PCD_Init(spi);
}

// Initialize GPIO pins
static void init_port_io(void) {
    // Initialize output pins
    output_io_create(RELAY_PIN);
    output_io_create(BUZZER_PIN);
    output_io_create(LED_GREEN_PIN);
    output_io_create(LED_RED_PIN);
    output_io_create(IRQ);

    // Set initial states
    output_io_set_level(RELAY_PIN, 0);    // Relay off (active low)
    output_io_set_level(BUZZER_PIN, 0);   // Buzzer off
    output_io_set_level(LED_GREEN_PIN, 0); // Green LED off
    output_io_set_level(LED_RED_PIN, 0);   // Red LED off
    output_io_set_level(IRQ, 1);   // IRQ off

    // Initialize button pin with pull-up
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
}

// Convert UID bytes to hex string
char* uid_to_string(uint8_t *uid, uint8_t uid_len) {
    static char uid_str[UID_STR_LEN];  // Static buffer for hex string
    if (uid_len > MAX_UID_LEN) {
        ESP_LOGE(TAG, "UID length exceeds maximum (%d bytes)", MAX_UID_LEN);
        return NULL;
    }

    uid_str[0] = '\0';
    for (int i = 0; i < uid_len; i++) {
        char temp[4];
        snprintf(temp, sizeof(temp), "%02X ", uid[i]);
        strcat(uid_str, temp);
    }
    uid_str[strlen(uid_str) - 1] = '\0';
    return uid_str;
    return uid_str;
}

// Check if UID exists in the specified array
bool is_uid_exists(char *uid_str, char *uid_array, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(&uid_array[i * UID_STR_LEN], uid_str) == 0) {
            return true;
        }
    }
    return false;
}

// Add UID to the specified array
bool add_uid(char *uid_str, char *uid_array, int *count) {
    if (*count >= MAX_UIDS) {
        ESP_LOGE(TAG, "UID array is full!");
        return false;
    }
    if (is_uid_exists(uid_str, uid_array, *count)) {
        ESP_LOGI(TAG, "UID %s already exists in the list!", uid_str);
        return false;
    }

    strncpy(&uid_array[*count * UID_STR_LEN], uid_str, UID_STR_LEN - 1);
    uid_array[*count * UID_STR_LEN + UID_STR_LEN - 1] = '\0';  // Ensure null termination
    (*count)++;
    ESP_LOGI(TAG, "Added UID to list: %s", uid_str);
    return true;
}

// Xoá một UID theo giá trị chuỗi
bool remove_uid_by_value(char *uid_str, char *uid_array, int *count) {
    for (int i = 0; i < *count; i++) {
        char *current_uid = &uid_array[i * UID_STR_LEN];
        if (strcmp(current_uid, uid_str) == 0) {
            // Dịch các UID còn lại về phía trước
            for (int j = i; j < *count - 1; j++) {
                strncpy(&uid_array[j * UID_STR_LEN], &uid_array[(j + 1) * UID_STR_LEN], UID_STR_LEN);
            }
            (*count)--;
            ESP_LOGI(TAG, "Removed UID from list: %s", uid_str);
            return true;
        }
    }
    ESP_LOGW(TAG, "UID not found: %s", uid_str);
    return false;
}


/* HTTP GET handler for / */
static esp_err_t get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char*)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

/* Return JSON for /register */
static esp_err_t get_register_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < registered_count; ++i) {
        httpd_resp_sendstr_chunk(req, "\"");
        httpd_resp_sendstr_chunk(req, &registered_uids[i * UID_STR_LEN]);
        httpd_resp_sendstr_chunk(req, "\"");
        if (i < registered_count - 1) {
            httpd_resp_sendstr_chunk(req, ",");
        }
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Return JSON for /unregister */
static esp_err_t get_unregister_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < unregistered_count; ++i) {
        httpd_resp_sendstr_chunk(req, "\"");
        httpd_resp_sendstr_chunk(req, &unregistered_uids[i * UID_STR_LEN]);
        httpd_resp_sendstr_chunk(req, "\"");
        if (i < unregistered_count - 1) {
            httpd_resp_sendstr_chunk(req, ",");
        }
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /registerUID */
static esp_err_t post_registerUID_handler(httpd_req_t *req) {
    char buf[UID_STR_LEN];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive UID");
        return ESP_FAIL;
    }
    buf[len] = '\0';
    ESP_LOGI(TAG, "Register UID: %s", buf);

    if (add_uid(buf, registered_uids, &registered_count)) {
        save_uid_to_nvs(buf, registered_count - 1); // Lưu UID mới vào NVS
        remove_uid_by_value(buf, unregistered_uids, &unregistered_count);
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "UID already registered or list full");
    }
    return ESP_OK;
}

/* POST /unregisterUID */
static esp_err_t post_unregisterUID_handler(httpd_req_t *req) {
    char buf[UID_STR_LEN];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive UID");
        return ESP_FAIL;
    }
    buf[len] = '\0';
    ESP_LOGI(TAG, "Unregister UID: %s", buf);

    if (remove_uid_by_value(buf, registered_uids, &registered_count)) {
        remove_uid_from_nvs(buf); // Xóa UID khỏi NVS
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "UID not found");
    }
    return ESP_OK;
}

/* Khai báo URI handlers */
static const httpd_uri_t get = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = get_handler,
    .user_ctx  = NULL
};
static const httpd_uri_t uri_get_register = {
    .uri = "/register",
    .method = HTTP_GET,
    .handler = get_register_handler
};

static const httpd_uri_t uri_get_unregister = {
    .uri = "/unregister",
    .method = HTTP_GET,
    .handler = get_unregister_handler
};

static const httpd_uri_t uri_post_registerUID = {
    .uri = "/registerUID",
    .method = HTTP_POST,
    .handler = post_registerUID_handler
};

static const httpd_uri_t uri_post_unregisterUID = {
    .uri = "/unregisterUID",
    .method = HTTP_POST,
    .handler = post_unregisterUID_handler
};

/* Start HTTP server */
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &get);
        httpd_register_uri_handler(server, &uri_get_register);
        httpd_register_uri_handler(server, &uri_get_unregister);
        httpd_register_uri_handler(server, &uri_post_registerUID);
        httpd_register_uri_handler(server, &uri_post_unregisterUID);
        return server;
    }
    return NULL;
}

/* Initialize SoftAP */
static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
}


// Task to handle RFID scanning
static void rfid_task(void *arg) {
    char *uid_str;
    bool card_present = false;
    int error_count = 0; // Đếm số lần lỗi liên tiếp

    while (1) {
        // Kiểm tra xem có thẻ mới được phát hiện không
        if (PICC_IsNewCardPresent(spi) && !card_present) {
            ESP_LOGI(TAG, "Card detected!");

            // Chọn thẻ và lấy UID
            if (PICC_Select(spi, &uid, 0) == STATUS_OK) {
                uid_str = uid_to_string(uid.uidByte, uid.size);
                ESP_LOGI(TAG, "UID: %s", uid_str);

                // Kiểm tra nếu thẻ đã có trong mảng registered_uids
                if (is_uid_exists(uid_str, registered_uids, registered_count)) {
                    ESP_LOGI(TAG, "UID found in registered_uids - Opening door");
                    app_event_t event = CARD_VALID;
                    xQueueSend(control_queue, &event, portMAX_DELAY);
                } else if (!is_uid_exists(uid_str, unregistered_uids, unregistered_count)) {
                    // Nếu thẻ chưa có trong mảng unregistered_uids, thêm vào
                    app_event_t event = CARD_INVALID;
                    xQueueSend(control_queue, &event, portMAX_DELAY);
                    if (add_uid(uid_str, unregistered_uids, &unregistered_count)) {
                        ESP_LOGI(TAG, "UID added to unregistered_uids: %s", uid_str);
                    } else {
                        ESP_LOGW(TAG, "Failed to add UID to unregistered_uids - List may be full");
                    }
                } else {
                    app_event_t event = CARD_INVALID;
                    xQueueSend(control_queue, &event, portMAX_DELAY);
                    ESP_LOGI(TAG, "UID already exists in unregistered_uids");
                }

                card_present = true; // Đánh dấu rằng thẻ đã được xử lý
                error_count = 0;     // Reset bộ đếm lỗi
            } else {
                ESP_LOGE(TAG, "Failed to select card - event error");
                error_count++;
            }
        } else if (!PICC_IsNewCardPresent(spi) && card_present) {
            // Nếu thẻ đã được gỡ bỏ
            card_present = false;
            ESP_LOGI(TAG, "Card removed");
        } else if (!PICC_IsNewCardPresent(spi)) {
            // Không phát hiện thẻ, reset bộ đếm lỗi
            error_count = 0;
        }

        // Nếu lỗi liên tiếp vượt quá ngưỡng, reset module RFID
        if (error_count >= 5) {
            ESP_LOGW(TAG, "Too many errors detected - Resetting RFID module");
            PCD_Init(spi); // Reset module RFID
            error_count = 0; // Reset bộ đếm lỗi sau khi reset
        }

        vTaskDelay(pdMS_TO_TICKS(500)); // Kiểm tra thẻ mới mỗi 500ms
    }
}
// Task to handle button input
static void button_task(void *arg) {
    int button_state;
    int hold_time_ms = 0;
    bool server_started = false;
    bool wifi_started = false; // Để kiểm soát trạng thái Wi-Fi
    bool action_performed = false; // Để kiểm soát hành động bật/tắt server

    while (1) {
        button_state = gpio_get_level(BUTTON_PIN); // Active low (0 khi nhấn)
        if (button_state == 0) {
            // Nút đang được nhấn, tăng thời gian giữ nút
            hold_time_ms += BUTTON_CHECK_MS;

            if (hold_time_ms >= BUTTON_HOLD_5S && !action_performed) {
                // Nếu giữ nút trên 5 giây và chưa thực hiện hành động
                if (!server_started) {
                    // Bật server và Wi-Fi nếu chưa chạy
                    ESP_LOGI(TAG, "Button held for more than 5s - Starting HTTP server and Wi-Fi");
                    server = start_webserver();
                    if (server != NULL) {
                        ESP_LOGI(TAG, "HTTP server started successfully");
                        if (!wifi_started) {
                            ESP_ERROR_CHECK(esp_wifi_start()); // Bật Wi-Fi
                            wifi_started = true;
                        }
                        server_started = true;
                    } else {
                        ESP_LOGE(TAG, "Failed to start HTTP server");
                    }
                } else {
                    // Tắt server và Wi-Fi nếu đang chạy
                    ESP_LOGI(TAG, "Button held for more than 5s - Stopping HTTP server and Wi-Fi");
                    httpd_stop(server);
                    server = NULL;
                    if (wifi_started) {
                        ESP_ERROR_CHECK(esp_wifi_stop()); // Tắt Wi-Fi
                        wifi_started = false;
                    }
                    server_started = false;
                    ESP_LOGI(TAG, "HTTP server and Wi-Fi stopped successfully");
                }
                action_performed = true; // Đánh dấu đã thực hiện hành động
                // Gửi trạng thái SERVER_TOGGLE để bật đèn
                app_event_t event = SERVER_TOGGLE;
                xQueueSend(control_queue, &event, portMAX_DELAY);
                action_performed = true;
            }
        } else {
            // Nút được nhả ra
            if (hold_time_ms > 0 && hold_time_ms < BUTTON_HOLD_5S) {
                // Nếu nhấn và nhả nút dưới 5 giây: mở cửa
                ESP_LOGI(TAG, "Button pressed for less than 5s - Opening door");
                app_event_t event = CARD_VALID;
                xQueueSend(control_queue, &event, portMAX_DELAY);
            }

            // Đặt lại trạng thái
            hold_time_ms = 0;
            action_performed = false; // Cho phép thực hiện hành động mới khi nhấn giữ lần sau
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_CHECK_MS)); // Kiểm tra trạng thái nút mỗi 50ms
    }
}

// Task to handle control (relay, LEDs, buzzer)
static void control_task(void *arg) {
    app_event_t event;
    while (1) {
        if (xQueueReceive(control_queue, &event, portMAX_DELAY)) {
            if (event == CARD_VALID) {
                // Valid card or button 2s: activate relay, green LED, and buzzer (1 beep)
                ESP_LOGI(TAG, "Valid action - Opening door");
                output_io_set_level(RELAY_PIN, 1);     // Relay on
                output_io_set_level(LED_GREEN_PIN, 1); // Green LED on
                output_io_set_level(BUZZER_PIN, 1);    // Buzzer on
                vTaskDelay(pdMS_TO_TICKS(100));        // Beep for 100ms
                output_io_set_level(BUZZER_PIN, 0);    // Buzzer off
                vTaskDelay(pdMS_TO_TICKS(1900));       // Keep relay/LED on for 2s total
                output_io_set_level(RELAY_PIN, 0);     // Relay off
                output_io_set_level(LED_GREEN_PIN, 0); // Green LED off
            } else if(event == SERVER_TOGGLE){
                ESP_LOGI(TAG, "Server toggled - Flashing LEDs");
                output_io_set_level(LED_RED_PIN, 1);
                output_io_set_level(LED_GREEN_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(1000)); // Bật đèn trong 1 giây
                output_io_set_level(LED_RED_PIN, 0);
                output_io_set_level(LED_GREEN_PIN, 0);
            } else {
                // Invalid card: red LED and buzzer (2 beeps)
                ESP_LOGI(TAG, "Invalid card");
                output_io_set_level(LED_RED_PIN, 1);   // Red LED on
                output_io_set_level(BUZZER_PIN, 1);    // Buzzer on
                vTaskDelay(pdMS_TO_TICKS(100));        // Beep 1 for 100ms
                output_io_set_level(BUZZER_PIN, 0);    // Buzzer off
                vTaskDelay(pdMS_TO_TICKS(100));        // Wait 100ms
                output_io_set_level(BUZZER_PIN, 1);    // Buzzer on
                vTaskDelay(pdMS_TO_TICKS(100));        // Beep 2 for 100ms
                output_io_set_level(BUZZER_PIN, 0);    // Buzzer off
                vTaskDelay(pdMS_TO_TICKS(1700));       // Keep LED on for 2s total
                output_io_set_level(LED_RED_PIN, 0);   // Red LED off
            }
        }
    }
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load registered UIDs from NVS
    load_uids_from_nvs();

    // Initialize hardware
    init_rfid_rc522();
    init_port_io();
    wifi_init_softap();

    // Create control queue
    control_queue = xQueueCreate(10, sizeof(app_event_t));
    if (control_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        return;
    }

    // Create tasks
    xTaskCreate(rfid_task, "rfid_task", 2048, NULL, 5, NULL);
    xTaskCreate(control_task, "control_task", 2048, NULL, 4, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 4, NULL);
}
