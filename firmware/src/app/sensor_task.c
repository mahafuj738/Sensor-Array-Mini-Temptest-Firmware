#include "sensor_task.h"
#include "sensor_app.h"
#include "definitions.h"
#include "FreeRTOS.h"
#include "task.h"

/* Measured need is small -- one lsm_frame_t (about 400 bytes) plus the encoded
 * line (672) both live in statics inside sensor_app.c, not on this stack. 512
 * words is generous; raise it if uxTaskGetStackHighWaterMark says otherwise. */
#define SENSOR_TASK_STACK_WORDS   512u

/* Below the BLE stack's own tasks on purpose. A missed sensor tick costs a few
 * milliseconds of LED timing; a missed radio deadline drops the connection. */
#define SENSOR_TASK_PRIORITY      1u

static void sensor_task(void *p_param)
{
    (void)p_param;

    sensor_app_init();

    for (;;) {
        sensor_app_step();
        vTaskDelay(pdMS_TO_TICKS(SENSOR_APP_TICK_MS));
    }
}

void sensor_task_start(void)
{
    (void)xTaskCreate(sensor_task, "SENSOR", SENSOR_TASK_STACK_WORDS,
                      NULL, SENSOR_TASK_PRIORITY, NULL);
}
