/**
 * Legacy update stub kept for compatibility.
 */

#pragma once

namespace ota {

/** Запуск отключён (всегда false). */
bool start();

/** Совместимость: no-op. */
void stop();

/** Совместимость: no-op. */
void update();

/** Всегда false. */
bool isActive();

}  // namespace ota
