#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "can_link.h"  // Pointing to your actual header

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Semaphore to bridge the Library Callback to the Main Loop */
K_SEM_DEFINE(blink_sem, 0, 1);

/* 1. THE CALLBACK: This is what can_link calls when a message arrives */
void can_rx_handler(const uint8_t *data, size_t len, uint8_t src_addr, 
                       bool is_broadcast, void *user_data) 
{
    LOG_INF("RX from node %u: first byte 0x%02x", src_addr, data[0]);
    
    // If the first byte is our "Blink" command (0x01)
    if (len > 0 && data[0] == 0x01) {
        k_sem_give(&blink_sem);
    }
}

int main() {
    int ret;
    uint8_t cmd_data[] = {0x01}; 

    /* 2. INITIALIZE: Use the correct library name and pass the callback */
    ret = can_link_init(can_rx_handler, NULL);
    if (ret < 0) {
        LOG_ERR("CAN Link Init Failed: %d", ret);
        return 0;
    }
    
    if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&btn)) {
        LOG_ERR("GPIO Hardware not ready");
        return 0;
    }

    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&btn, GPIO_INPUT);

    LOG_INF("System Ready! Node ID: %u", can_link_node_id());

    while (true) {
        /* TRANSMITTER: Send message when button is pressed */
        if (gpio_pin_get_dt(&btn) > 0) {
            gpio_pin_set_dt(&led, 1);
            
            // Using your library's specific send function
            ret = can_link_send(cmd_data, sizeof(cmd_data));
            
            if (ret == 0) {
                LOG_INF("ISO-TP Message Queued for Peer!");
            } else {
                LOG_ERR("Send failed: %d", ret);
            }
            
            k_msleep(300); // Debounce
            gpio_pin_set_dt(&led, 0);
        }

        /* RECEIVER: React to the semaphore given in the callback */
        if (k_sem_take(&blink_sem, K_MSEC(1)) == 0) {
            LOG_INF("Blinking LED...");
            for (int i = 0; i < 10; i++) {
                gpio_pin_toggle_dt(&led);
                k_msleep(100);
            }
        }
        k_msleep(10);
    }
    return 0;
}