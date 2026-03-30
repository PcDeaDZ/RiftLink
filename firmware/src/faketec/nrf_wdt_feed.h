/**
 * Аппаратный WDT nRF52 — кормление из loop и длинных delay-циклов (иначе «мёртвая» прошивка без heartbeat).
 * Без reset при зависании в HardFault/mutex/block — только автоматический reset при отсутствии feed ~30 с.
 */
#pragma once

#include <stdint.h>

void riftlink_wdt_init(void);
void riftlink_wdt_feed(void);
