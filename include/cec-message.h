#ifndef CEC_MESSAGE_H
#define CEC_MESSAGE_H

#include <stdint.h>

#define CEC_FRAME_MAX_LEN (16)
#define CEC_FRAME_MAX_OPERAND_LEN (CEC_FRAME_MAX_LEN - 2)

#define HEADER0(iaddr, daddr) (((iaddr) << 4) | (daddr))

typedef struct __attribute__((packed)) {
  union {
    struct {
      uint8_t header;
      uint8_t opcode;
      uint8_t operand[CEC_FRAME_MAX_OPERAND_LEN];
    };
    uint8_t data[CEC_FRAME_MAX_LEN];
  };
  uint8_t len;
} cec_message_t;

typedef enum {
  CEC_TX_ACK = 1,
  CEC_TX_NO_ACK = 2,
  CEC_TX_TIMEOUT = 3,
  CEC_TX_UNAVAILABLE = 4,
} cec_tx_result_t;

#endif
