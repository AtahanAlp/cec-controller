#ifndef CEC_TASK_H
#define CEC_TASK_H

#include <stdint.h>
#include "cec-control.h"
#include "cec-frame.h"

#define CEC_TASK_NAME "cec"

uint16_t cec_get_physical_address(void);
uint8_t cec_get_logical_address(void);
void cec_set_physical_address(uint16_t physical_address);
cec_tx_result_t cec_send_msg_sync(const cec_message_t *msg, uint32_t timeout_ms);
cec_power_query_result_t cec_query_power_status(uint8_t *power_status, uint32_t timeout_ms);
void cec_task(void *param);

#endif
