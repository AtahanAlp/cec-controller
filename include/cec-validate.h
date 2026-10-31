#ifndef CEC_VALIDATE_H
#define CEC_VALIDATE_H

#include <stdbool.h>
#include "cec-message.h"

bool cec_message_valid(const cec_message_t *message);
bool cec_physical_address_valid(uint16_t address);

#endif
