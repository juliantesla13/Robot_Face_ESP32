#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2c.h"
#include "driver/touch_pad.h"
#include "u8g2.h"

// Librerías para WiFi y OTA
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

static const char *TAG = "VECTOR_BOT";

// ==========================================
// 1. CONFIGURACIÓN DE PINES, PANTALLA Y RED
// ==========================================
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000

#define WIFI_SSID "FAMILIA GOMEZ-2.4G"
#define WIFI_PASS "TU_CONTRASEÑA"
#define FIRMWARE_UPGRADE_URL "https://tu-servidor.com/firmware.bin"

// --- TOUCH ---
#define TOUCH_PAD_BLINK TOUCH_PAD_NUM0 // GPIO4 (T0). No choca con SDA=21 / SCL=22
#define TOUCH_THRESHOLD_PCT 70         // "tocado" al caer al 70% del valor base
#define TOUCH_POLL_MS 20

// --- PANTALLA ---
#define YELLOW_ZONE_LIMIT 16 // filas 0-15 = zona amarilla del OLED

// ==========================================
// 2. CALLBACKS NATIVOS DE I2C PARA ESP-IDF
// ==========================================
static i2c_cmd_handle_t i2c_handle;

uint8_t u8g2_gpio_and_delay_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg)
    {
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    }
    return 1;
}

uint8_t u8g2_byte_i2c_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg)
    {
    case U8X8_MSG_BYTE_SEND:
    {
        uint8_t *data = (uint8_t *)arg_ptr;
        while (arg_int > 0)
        {
            i2c_master_write_byte(i2c_handle, *data, true);
            data++;
            arg_int--;
        }
        break;
    }
    case U8X8_MSG_BYTE_INIT:
    {
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = I2C_MASTER_SDA_IO,
            .scl_io_num = I2C_MASTER_SCL_IO,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = I2C_MASTER_FREQ_HZ,
        };
        i2c_param_config(I2C_MASTER_NUM, &conf);
        i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
        break;
    }
    case U8X8_MSG_BYTE_START_TRANSFER:
    {
        uint8_t addr = u8x8_GetI2CAddress(u8x8);
        i2c_handle = i2c_cmd_link_create();
        i2c_master_start(i2c_handle);
        i2c_master_write_byte(i2c_handle, addr | I2C_MASTER_WRITE, true);
        break;
    }
    case U8X8_MSG_BYTE_END_TRANSFER:
    {
        i2c_master_stop(i2c_handle);
        i2c_master_cmd_begin(I2C_MASTER_NUM, i2c_handle, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(i2c_handle);
        break;
    }
    }
    return 1;
}

// ==========================================
// 3. MODELOS DE DATOS PARA LA CARA
// ==========================================
typedef enum
{
    EXPR_NEUTRAL,
    EXPR_HAPPY,
    EXPR_SAD,
    EXPR_ANGRY,
    EXPR_SLEEPY,
    EXPR_BLINK,
    EXPR_CONFUSED,
    EXPR_SUSPICIOUS,
    EXPR_WINK_LEFT,
    EXPR_WINK_RIGHT
} robot_expression_t;

typedef struct
{
    int16_t x;
    int16_t y;
    uint8_t width;
    uint8_t height;
    uint8_t radius;
} eye_model_t;

typedef struct
{
    eye_model_t left_eye;
    eye_model_t right_eye;
    robot_expression_t current_expression;
} robot_face_t;

// --- AJUSTES ESTRICTOS PARA EVITAR ZONA AMARILLA (Y: 0 a 15) ---
// La base de los ojos se baja significativamente a Y=30 y la altura se reduce a 20.
// Ningún cálculo debe resultar en un Y < 16.
#define EYE_BASE_W 30
#define EYE_BASE_H 34
#define EYE_RADIUS 8
#define LEFT_EYE_X_POS 18
#define RIGHT_EYE_X_POS 80
#define EYE_BASE_Y 22
// Desplazamiento en Y para que un ojo cerrado (alto 4) quede centrado en el ojo abierto
#define EYE_CLOSED_Y_OFFSET (EYE_BASE_H / 2 - 2)

void update_face_128x64(robot_face_t *face, robot_expression_t new_expr)
{
    face->current_expression = new_expr;

    // Estado Base: Y=22, Altura=34. Rango de píxeles: 22 a 56 (Totalmente en zona azul).
    face->left_eye = (eye_model_t){LEFT_EYE_X_POS, EYE_BASE_Y, EYE_BASE_W, EYE_BASE_H, EYE_RADIUS};
    face->right_eye = (eye_model_t){RIGHT_EYE_X_POS, EYE_BASE_Y, EYE_BASE_W, EYE_BASE_H, EYE_RADIUS};

    switch (new_expr)
    {
    case EXPR_NEUTRAL:
        break;
    case EXPR_BLINK:
        face->left_eye.y += EYE_CLOSED_Y_OFFSET;
        face->left_eye.height = 4;
        face->left_eye.radius = 2;
        face->right_eye.y += EYE_CLOSED_Y_OFFSET;
        face->right_eye.height = 4;
        face->right_eye.radius = 2;
        break;
    case EXPR_HAPPY:
        // "Sonrisa": Los ojos se hacen un arco inferior
        face->left_eye.height /= 2;
        face->left_eye.y += 10;
        face->right_eye.height /= 2;
        face->right_eye.y += 10;
        break;
    case EXPR_SAD:
        // Caídos y más pequeños
        face->left_eye.height = 14;
        face->left_eye.y += 6;
        face->left_eye.width -= 4;
        face->left_eye.x += 2;
        face->right_eye.height = 14;
        face->right_eye.y += 6;
        face->right_eye.width -= 4;
        face->right_eye.x += 2;
        break;
    case EXPR_ANGRY:
        // Estrechados e inclinados hacia abajo (simulado)
        face->left_eye.height = 10;
        face->left_eye.y += 5;
        face->left_eye.width += 4;
        face->left_eye.x -= 2;
        face->right_eye.height = 10;
        face->right_eye.y += 5;
        face->right_eye.width += 4;
        face->right_eye.x -= 2;
        break;
    case EXPR_SLEEPY:
        // Medio cerrados
        face->left_eye.height = 8;
        face->left_eye.y += 10;
        face->right_eye.height = 8;
        face->right_eye.y += 10;
        break;
    case EXPR_CONFUSED:
        // Un ojo normal, el otro entrecerrado
        face->right_eye.height = 10;
        face->right_eye.y += 5;
        break;
    case EXPR_SUSPICIOUS:
        // Muy estrechos
        face->left_eye.height = 6;
        face->left_eye.y += 7;
        face->right_eye.height = 6;
        face->right_eye.y += 7;
        break;
    case EXPR_WINK_LEFT:
        // Parpadeo de un solo ojo
        face->left_eye.height = 4;
        face->left_eye.y += EYE_CLOSED_Y_OFFSET;
        face->left_eye.radius = 2;
        break;
    case EXPR_WINK_RIGHT:
        face->right_eye.height = 4;
        face->right_eye.y += EYE_CLOSED_Y_OFFSET;
        face->right_eye.radius = 2;
        break;
    }

    // Protección: ningún ojo puede entrar en la zona amarilla (Y < 16)
    if (face->left_eye.y < YELLOW_ZONE_LIMIT)
        face->left_eye.y = YELLOW_ZONE_LIMIT;
    if (face->right_eye.y < YELLOW_ZONE_LIMIT)
        face->right_eye.y = YELLOW_ZONE_LIMIT;

    ESP_LOGI(TAG, "FW v2 | expr=%d  y=%d  h=%d", (int)new_expr, face->left_eye.y, face->left_eye.height);
}

void render_face(u8g2_t *u8g2, robot_face_t *face)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_DrawRBox(u8g2, face->left_eye.x, face->left_eye.y, face->left_eye.width, face->left_eye.height, face->left_eye.radius);
    u8g2_DrawRBox(u8g2, face->right_eye.x, face->right_eye.y, face->right_eye.width, face->right_eye.height, face->right_eye.radius);
    u8g2_SendBuffer(u8g2);
}

// ==========================================
// 3.1 TOUCH (GPIO4 / T0)
// ==========================================
static uint16_t touch_threshold;

static void touch_init(void)
{
    touch_pad_init();
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    touch_pad_config(TOUCH_PAD_BLINK, 0); // Si tu ESP-IDF da error, quita el segundo argumento
    vTaskDelay(pdMS_TO_TICKS(100));

    // Calibración: promedio en reposo (no toques el pin al arrancar)
    uint32_t sum = 0;
    uint16_t val = 0;
    for (int i = 0; i < 10; i++)
    {
        touch_pad_read(TOUCH_PAD_BLINK, &val);
        sum += val;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    touch_threshold = (sum / 10) * TOUCH_THRESHOLD_PCT / 100;
    ESP_LOGI(TAG, "Touch listo. Umbral = %u", touch_threshold);
}

static bool is_touched(void)
{
    uint16_t val = 0;
    touch_pad_read(TOUCH_PAD_BLINK, &val);
    return val < touch_threshold; // en ESP32 el valor BAJA al tocar
}

// ==========================================
// 4. MÓDULO WIFI Y OTA
// ==========================================
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Reintentando conexión WiFi...");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG, "WiFi Conectado!");
    }
}

void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

void ota_update_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Iniciando proceso OTA...");

    esp_http_client_config_t http_config = {
        .url = FIRMWARE_UPGRADE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "OTA exitoso, reiniciando...");
        esp_restart();
    }
    else
    {
        ESP_LOGE(TAG, "Fallo en la actualización OTA: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL);
}

// ==========================================
// 5. FUNCIÓN PRINCIPAL
// ==========================================
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_init();

    u8g2_t u8g2;
    // Controlador SSD1306
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2,
        U8G2_R0,
        u8g2_byte_i2c_esp32,
        u8g2_gpio_and_delay_esp32);

    u8x8_SetI2CAddress(&u8g2.u8x8, 0x78);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);

    // xTaskCreate(&ota_update_task, "ota_update_task", 8192, NULL, 5, NULL);

    robot_face_t my_robot;

    touch_init();

    // Cara fija: neutral
    update_face_128x64(&my_robot, EXPR_NEUTRAL);
    render_face(&u8g2, &my_robot);

    while (1)
    {
        if (is_touched())
        {
            // Parpadeo continuo mientras se mantenga el toque
            update_face_128x64(&my_robot, EXPR_BLINK);
            render_face(&u8g2, &my_robot);
            vTaskDelay(pdMS_TO_TICKS(150));

            update_face_128x64(&my_robot, EXPR_NEUTRAL);
            render_face(&u8g2, &my_robot);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        }
    }
}