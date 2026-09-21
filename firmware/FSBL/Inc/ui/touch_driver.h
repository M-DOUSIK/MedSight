#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Touch Driver API

void touch_driver_init(void);
bool touch_driver_get_touch(uint32_t *x, uint32_t *y);

void user_button_init(void);
bool user_button_is_pressed(void);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_DRIVER_H
