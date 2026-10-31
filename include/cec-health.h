#ifndef CEC_HEALTH_H
#define CEC_HEALTH_H
#include <stdbool.h>
#include <stdint.h>
static inline bool cec_health_fresh(uint32_t now, uint32_t boot,
                                    uint32_t cec, uint32_t parser) {
  return (uint32_t)(now - boot) < 10000
      || ((uint32_t)(now - cec) < 3000 && (uint32_t)(now - parser) < 3000);
}
#endif
