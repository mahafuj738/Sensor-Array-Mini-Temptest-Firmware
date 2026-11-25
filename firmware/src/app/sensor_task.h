/* sensor_task.h -- the FreeRTOS task that drives the sensor state machine.
 *
 * This exists because peripheral_trp_uart's own APP_Tasks() blocks forever on
 * OSAL_QUEUE_Receive(..., OSAL_WAIT_FOREVER). Nothing can be bolted onto that
 * loop: it only wakes when the BLE stack posts a message. So the sensor gets
 * its own task, and the two never block each other.
 */
#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

/* Call once from APP_STATE_INIT, after APP_BleStackInit(). */
void sensor_task_start(void);

#endif /* SENSOR_TASK_H */
