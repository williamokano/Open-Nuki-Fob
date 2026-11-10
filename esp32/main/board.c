#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"

#define BUTTON_GPIO GPIO_NUM_39
#define LED_GPIO GPIO_NUM_27

static led_strip_handle_t led;

void board_init(void) {
    // GPIO39 is input-only and has no internal pull resistors; the board has an external pull-up.
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_SK6812,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led));
    led_strip_clear(led);
}

bool board_button_pressed(void) {
    return gpio_get_level(BUTTON_GPIO) == 0;
}

void board_led(uint8_t r, uint8_t g, uint8_t b) {
    static uint32_t current = UINT32_MAX;
    uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if(color == current) { return; }
    current = color;

    if(color == 0) {
        led_strip_clear(led);
        return;
    }
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}
