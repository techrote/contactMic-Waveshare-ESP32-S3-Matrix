#ifndef CM_TEST_GPIO_H
#define CM_TEST_GPIO_H
#include "esp_err.h"
#include <stdint.h>
typedef int gpio_num_t;
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_POSEDGE 1
typedef struct {uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type;} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_install_isr_service(int);
void gpio_uninstall_isr_service(void);
esp_err_t gpio_isr_handler_add(gpio_num_t, void (*fn)(void *), void *);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_intr_enable(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
int gpio_get_level(gpio_num_t);
#endif
