#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cecctl.h"

#define EDID_SIZE 256U

static void checksum(uint8_t *block) {
  unsigned int sum = 0;
  for (size_t i = 0; i < 127; i++) {
    sum += block[i];
  }
  block[127] = (uint8_t)(0U - sum);
}

static void make_edid(uint8_t edid[EDID_SIZE], uint16_t address) {
  memset(edid, 0, EDID_SIZE);
  static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  memcpy(edid, header, sizeof(header));
  edid[126] = 1;
  checksum(edid);

  uint8_t *cta = edid + 128;
  cta[0] = 0x02;
  cta[1] = 0x03;
  cta[2] = 10;
  cta[4] = (3U << 5) | 5U;
  cta[5] = 0x03;
  cta[6] = 0x0c;
  cta[7] = 0x00;
  cta[8] = (uint8_t)(address >> 8);
  cta[9] = (uint8_t)address;
  checksum(cta);
}

static void test_addresses(void) {
  uint16_t address = 0;
  assert(cecctl_parse_physical_address("1.0.0.0", &address) == 0 && address == 0x1000);
  assert(cecctl_parse_physical_address("2100", &address) == 0 && address == 0x2100);
  assert(cecctl_parse_physical_address("0.0.0.0", &address) != 0);
  assert(cecctl_parse_physical_address("1.0.2.0", &address) != 0);
  assert(cecctl_parse_physical_address("10000", &address) != 0);
  char formatted[8];
  cecctl_format_physical_address(0x3210, formatted);
  assert(strcmp(formatted, "3.2.1.0") == 0);
}

static void test_edid(void) {
  uint8_t edid[EDID_SIZE];
  uint16_t address = 0;
  make_edid(edid, 0x1000);
  assert(cecctl_edid_physical_address(edid, sizeof(edid), &address) == 0);
  assert(address == 0x1000);

  edid[255]++;
  assert(cecctl_edid_physical_address(edid, sizeof(edid), &address) != 0);
  make_edid(edid, 0x1020);
  assert(cecctl_edid_physical_address(edid, sizeof(edid), &address) != 0);
  make_edid(edid, 0x1000);
  edid[132] = (3U << 5) | 31U;
  checksum(edid + 128);
  assert(cecctl_edid_physical_address(edid, sizeof(edid), &address) != 0);
  make_edid(edid, 0x1000);
  assert(cecctl_edid_physical_address(edid, 128, &address) != 0);
}

static void test_replies(void) {
  cecctl_reply_t reply;
  assert(cecctl_parse_reply("CECCTRL/1 OK command=on attempts=2", "on", &reply) == 0);
  assert(reply.ok && reply.attempts == 2);
  assert(cecctl_parse_reply("> CECCTRL/1 ERR command=standby code=no_ack attempts=2",
                            "standby", &reply) == 1);
  assert(!reply.ok && strcmp(reply.code, "no_ack") == 0);
  assert(cecctl_parse_reply("CECCTRL/2 OK command=on", "on", &reply) < 0);
  assert(cecctl_parse_reply("noise", "on", &reply) < 0);
  assert(cecctl_parse_reply("CECCTRL/1 OK command=status power=standby value=1", "status",
                            &reply) == 0);
  assert(strcmp(reply.power, "standby") == 0);
}

int main(void) {
  test_addresses();
  test_edid();
  test_replies();
  puts("cecctl tests passed");
  return 0;
}
