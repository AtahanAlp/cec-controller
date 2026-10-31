#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "cec-frame.h"
#include "cec-log.h"
#include "cec-validate.h"

TaskHandle_t xCECTask;
#define RX_QUEUE_LEN 4
#define EDGES (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)

typedef struct { cec_message_t message; uint64_t at; } received_t;
static StaticQueue_t rx_queue_static;
static uint8_t rx_storage[RX_QUEUE_LEN * sizeof(received_t)];
static QueueHandle_t rx_queue;
static cec_frame_stats_t stats;
static volatile uint64_t last_edge;
static uint64_t received_at;
static uint64_t sent_at;
static alarm_id_t ack_alarm = -1;
static uint8_t address = 0x0f;

typedef enum { RX_START, RX_START_RISE, RX_BIT_FALL, RX_BIT_RISE,
               RX_EOM_FALL, RX_EOM_RISE, RX_ACK_FALL, RX_ACK_RISE } rx_state_t;
static struct {
  rx_state_t state;
  cec_message_t message;
  uint64_t start, began;
  unsigned bit;
  bool first, eom, ack;
} rx;

static void rx_reset(void) {
  if (ack_alarm >= 0) cancel_alarm(ack_alarm);
  ack_alarm = -1;
  gpio_set_dir(CEC_PIN, GPIO_IN);
  rx.state = RX_START;
  rx.message.len = 0;
  rx.bit = 0;
}

static void rx_abort(void) { stats.rx_abort_frames++; rx_reset(); }

static int64_t ack_release(alarm_id_t id, void *context) {
  (void)id; (void)context;
  ack_alarm = -1;
  gpio_set_dir(CEC_PIN, GPIO_IN);
  return 0;
}

static bool in_range(uint64_t n, uint64_t minimum, uint64_t maximum) {
  return n >= minimum && n <= maximum;
}

static void frame_rx_isr(uint gpio, uint32_t events) {
  if (gpio != CEC_PIN) return;
  const uint64_t now = time_us_64();
  last_edge = now;
  if ((events & EDGES) == EDGES) { rx_abort(); return; }
  bool falling = (events & GPIO_IRQ_EDGE_FALL) != 0;
  bool expect_fall = rx.state == RX_START || rx.state == RX_BIT_FALL
                  || rx.state == RX_EOM_FALL || rx.state == RX_ACK_FALL;
  if (falling != expect_fall) return;
  if (rx.state != RX_START && now - rx.began > 450000) { rx_abort(); return; }
  uint64_t duration = now - rx.start;
  switch (rx.state) {
    case RX_START:
      memset(&rx.message, 0, sizeof(rx.message));
      rx.start = rx.began = now;
      rx.first = true; rx.ack = false; rx.bit = 0;
      rx.state = RX_START_RISE;
      break;
    case RX_START_RISE:
      if (!in_range(duration, 3500, 3900)) { rx_abort(); return; }
      rx.state = RX_BIT_FALL;
      break;
    case RX_BIT_FALL:
    case RX_EOM_FALL:
    case RX_ACK_FALL: {
      if (!in_range(duration, rx.first ? 4300 : 2050, rx.first ? 4700 : 2750)) {
        rx_abort(); return;
      }
      rx.first = false; rx.start = now;
      if (rx.state == RX_BIT_FALL) {
        if (rx.message.len >= CEC_FRAME_MAX_LEN) { rx_abort(); return; }
        rx.state = RX_BIT_RISE;
      } else if (rx.state == RX_EOM_FALL) {
        rx.state = RX_EOM_RISE;
      } else {
        rx.state = RX_ACK_RISE;
        if ((rx.message.header & 15) == address && address < 15) {
          gpio_set_dir(CEC_PIN, GPIO_OUT);
          ack_alarm = add_alarm_at(from_us_since_boot(now + 1500), ack_release, NULL, true);
          if (ack_alarm < 0) { rx_abort(); return; }
          rx.ack = true;
        }
      }
      break;
    }
    case RX_BIT_RISE:
    case RX_EOM_RISE: {
      bool one = in_range(duration, 400, 800);
      if (!one && !in_range(duration, 1300, 1700)) { rx_abort(); return; }
      if (rx.state == RX_BIT_RISE) {
        if (rx.message.len >= CEC_FRAME_MAX_LEN) { rx_abort(); return; }
        uint8_t *byte = &rx.message.data[rx.message.len];
        *byte = (uint8_t)((*byte << 1) | one);
        if (++rx.bit == 8) {
          rx.bit = 0; rx.message.len++; rx.state = RX_EOM_FALL;
        } else rx.state = RX_BIT_FALL;
      } else {
        rx.eom = one;
        if (!one && rx.message.len == CEC_FRAME_MAX_LEN) { rx_abort(); return; }
        rx.state = RX_ACK_FALL;
      }
      break;
    }
    case RX_ACK_RISE:
      if (!in_range(duration, 400, 800) && !in_range(duration, 1300, 1700)) {
        rx_abort(); return;
      }
      if (rx.eom) {
        if (cec_message_valid(&rx.message)) {
          received_t item = { .message = rx.message, .at = now };
          if (xQueueSendFromISR(rx_queue, &item, NULL) == pdTRUE) {
            stats.rx_frames++;
            xTaskNotifyIndexedFromISR(xCECTask, NOTIFY_RX, NOTIFY_RX_RX, eSetBits, NULL);
          } else stats.rx_abort_frames++;
        } else stats.rx_abort_frames++;
        rx_reset();
      } else rx.state = RX_BIT_FALL;
      break;
  }
}

void cec_frame_recv(cec_message_t *message, uint8_t own_address) {
  uint32_t irq = save_and_disable_interrupts();
  address = own_address;
  if (rx.state != RX_START && (time_us_64() - last_edge > 5000
                           || time_us_64() - rx.began > 450000)) rx_abort();
  restore_interrupts(irq);
  received_t item;
  if (xQueueReceive(rx_queue, &item, 0) != pdTRUE) {
    uint32_t events;
    xTaskNotifyWaitIndexed(NOTIFY_RX, 0, UINT32_MAX, &events, pdMS_TO_TICKS(10));
    if (xQueueReceive(rx_queue, &item, 0) != pdTRUE) { message->len = 0; return; }
  }
  *message = item.message;
  received_at = item.at;
  cec_frame_t log_frame = { .message = message };
  cec_log_frame(&log_frame, true);
}

uint64_t cec_frame_received_at(void) { return received_at; }
uint64_t cec_frame_sent_at(void) { return sent_at; }

typedef enum { TX_START_LOW, TX_START_RELEASE, TX_DATA_LOW, TX_DATA_RELEASE,
               TX_DATA_SAMPLE, TX_DATA_END, TX_EOM_LOW, TX_EOM_RELEASE,
               TX_ACK_LOW, TX_ACK_RELEASE, TX_ACK_SAMPLE, TX_NEXT, TX_END } tx_state_t;
static struct {
  cec_message_t message;
  tx_state_t state;
  unsigned byte, bit;
  uint64_t start, due, deadline;
  bool one;
  volatile bool active;
  volatile cec_tx_result_t result;
} tx;

static int64_t tx_finish(cec_tx_result_t result) {
  gpio_set_dir(CEC_PIN, GPIO_IN);
  tx.result = result; tx.active = false;
  xTaskNotifyIndexedFromISR(xCECTask, NOTIFY_TX, 0, eIncrement, NULL);
  return 0;
}

static int64_t tx_next(uint64_t offset) {
  tx.due = tx.start + offset;
  int64_t remaining = (int64_t)tx.due - (int64_t)time_us_64();
  if (remaining <= 0 || tx.due >= tx.deadline) return tx_finish(CEC_TX_TIMEOUT);
  return remaining;
}

static int64_t frame_tx_callback(alarm_id_t id, void *context) {
  (void)id; (void)context;
  uint64_t now = time_us_64();
  if (!tx.active) return 0;
  if (now >= tx.deadline || (tx.due && now > tx.due + 150)) return tx_finish(CEC_TX_TIMEOUT);
  switch (tx.state) {
    case TX_START_LOW:
      gpio_set_dir(CEC_PIN, GPIO_OUT); tx.start = now;
      sent_at = now;
      tx.state = TX_START_RELEASE; return tx_next(3700);
    case TX_START_RELEASE:
      gpio_set_dir(CEC_PIN, GPIO_IN);
      tx.state = TX_DATA_LOW; return tx_next(4500);
    case TX_DATA_LOW:
      tx.one = (tx.message.data[tx.byte] & (1u << tx.bit)) != 0;
      gpio_set_dir(CEC_PIN, GPIO_OUT); tx.start = now;
      tx.state = TX_DATA_RELEASE; return tx_next(tx.one ? 600 : 1500);
    case TX_DATA_RELEASE:
      gpio_set_dir(CEC_PIN, GPIO_IN);
      tx.state = tx.one ? TX_DATA_SAMPLE : TX_DATA_END;
      return tx_next(tx.one ? 1050 : 2400);
    case TX_DATA_SAMPLE:
      if (!gpio_get(CEC_PIN))
        return tx_finish(tx.byte == 0 && tx.bit >= 4 ? CEC_TX_ARBITRATION_LOST : CEC_TX_BUS_ERROR);
      tx.state = TX_DATA_END; return tx_next(2400);
    case TX_DATA_END:
      if (tx.bit) { tx.bit--; tx.state = TX_DATA_LOW; }
      else { tx.byte++; tx.state = TX_EOM_LOW; }
      return frame_tx_callback(id, context);
    case TX_EOM_LOW:
      gpio_set_dir(CEC_PIN, GPIO_OUT); tx.start = now;
      tx.state = TX_EOM_RELEASE;
      return tx_next(tx.byte < tx.message.len ? 1500 : 600);
    case TX_EOM_RELEASE:
      gpio_set_dir(CEC_PIN, GPIO_IN);
      tx.state = TX_ACK_LOW; return tx_next(2400);
    case TX_ACK_LOW:
      gpio_set_dir(CEC_PIN, GPIO_OUT); tx.start = now;
      tx.state = TX_ACK_RELEASE; return tx_next(600);
    case TX_ACK_RELEASE:
      gpio_set_dir(CEC_PIN, GPIO_IN);
      tx.state = TX_ACK_SAMPLE; return tx_next(1050);
    case TX_ACK_SAMPLE: {
      bool low = !gpio_get(CEC_PIN);
      bool broadcast = (tx.message.header & 15) == 15;
      if (low == broadcast) {
        tx.result = broadcast ? CEC_TX_BUS_ERROR : CEC_TX_NO_ACK;
        tx.state = TX_END;
      } else tx.state = tx.byte < tx.message.len ? TX_NEXT : TX_END;
      return tx_next(2400);
    }
    case TX_NEXT:
      tx.bit = 7; tx.state = TX_DATA_LOW;
      return frame_tx_callback(id, context);
    case TX_END: return tx_finish(tx.result);
  }
  return tx_finish(CEC_TX_BUS_ERROR);
}

bool cec_frame_idle(void) {
  uint32_t irq = save_and_disable_interrupts();
  bool idle = !tx.active && rx.state == RX_START && gpio_get(CEC_PIN)
           && time_us_64() - last_edge >= 16800;
  restore_interrupts(irq);
  return idle;
}

cec_tx_result_t cec_frame_send_timeout(const cec_message_t *message, uint32_t timeout_ms) {
  if (!cec_message_valid(message) || !timeout_ms) return CEC_TX_UNAVAILABLE;
  uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
  uint64_t idle_deadline = time_us_64() + 1000000;
  if (idle_deadline > deadline) idle_deadline = deadline;
  while (true) {
    uint32_t irq = save_and_disable_interrupts();
    if (cec_frame_idle()) {
      gpio_set_irq_enabled(CEC_PIN, EDGES, false);
      rx_reset();
      restore_interrupts(irq);
      break;
    }
    if (rx.state != RX_START && time_us_64() - last_edge > 5000) rx_abort();
    restore_interrupts(irq);
    if (time_us_64() >= idle_deadline) {
      stats.tx_timeout_frames++; stats.tx_idle_timeout_frames++;
      return CEC_TX_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  memset(&tx, 0, sizeof(tx));
  tx.message = *message; tx.bit = 7; tx.state = TX_START_LOW;
  tx.result = CEC_TX_ACK; tx.active = true;
  uint64_t frame_deadline = time_us_64() + 500000;
  tx.deadline = frame_deadline < deadline ? frame_deadline : deadline;
  ulTaskNotifyTakeIndexed(NOTIFY_TX, pdTRUE, 0);
  alarm_id_t alarm = -1;
  if (tx.deadline > time_us_64()) {
    alarm = add_alarm_at(from_us_since_boot(time_us_64() + 10), frame_tx_callback, NULL, true);
  }
  if (alarm < 0) tx.result = CEC_TX_TIMEOUT;
  else {
    int64_t remaining = (int64_t)tx.deadline - (int64_t)time_us_64();
    if (remaining <= 0 || !ulTaskNotifyTakeIndexed(NOTIFY_TX, pdTRUE,
          pdMS_TO_TICKS((uint32_t)((remaining + 999) / 1000)))) tx.result = CEC_TX_TIMEOUT;
  }
  uint32_t irq = save_and_disable_interrupts();
  tx.active = false;
  if (alarm >= 0) cancel_alarm(alarm);
  gpio_set_dir(CEC_PIN, GPIO_IN);
  last_edge = time_us_64();
  gpio_acknowledge_irq(CEC_PIN, EDGES);
  gpio_set_irq_enabled(CEC_PIN, EDGES, true);
  cec_tx_result_t result = tx.result;
  if (result == CEC_TX_TIMEOUT) { stats.tx_timeout_frames++; stats.tx_alarm_timeout_frames++; }
  else if (result == CEC_TX_NO_ACK) stats.tx_noack_frames++;
  else if (result == CEC_TX_ARBITRATION_LOST) stats.tx_arbitration_frames++;
  else if (result == CEC_TX_BUS_ERROR) stats.tx_bus_error_frames++;
  else stats.tx_frames++;
  restore_interrupts(irq);
  cec_frame_t log_frame = { .message = &tx.message, .ack = result == CEC_TX_ACK };
  cec_log_frame(&log_frame, false);
  return result;
}

cec_tx_result_t cec_frame_send(const cec_message_t *message) {
  return cec_frame_send_timeout(message, 1500);
}

void cec_frame_get_stats(cec_frame_stats_t *destination) {
  uint32_t irq = save_and_disable_interrupts();
  *destination = stats;
  restore_interrupts(irq);
}

void cec_frame_init(void) {
  rx_queue = xQueueCreateStatic(RX_QUEUE_LEN, sizeof(received_t), rx_storage, &rx_queue_static);
  gpio_init(CEC_PIN); gpio_disable_pulls(CEC_PIN); gpio_put(CEC_PIN, false);
  rx_reset(); last_edge = time_us_64();
  gpio_set_irq_callback(frame_rx_isr);
  irq_set_enabled(IO_IRQ_BANK0, true);
  gpio_acknowledge_irq(CEC_PIN, EDGES);
  gpio_set_irq_enabled(CEC_PIN, EDGES, true);
}
