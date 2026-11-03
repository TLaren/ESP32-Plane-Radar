#include "services/net_lock.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace services::net {

namespace {
SemaphoreHandle_t lockHandle() {
  static SemaphoreHandle_t h = xSemaphoreCreateMutex();  // thread-safe static init
  return h;
}
}  // namespace

Guard::Guard(unsigned long timeout_ms)
    : ok_(xSemaphoreTake(lockHandle(), pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {}

Guard::~Guard() {
  if (ok_) {
    xSemaphoreGive(lockHandle());
  }
}

}  // namespace services::net
