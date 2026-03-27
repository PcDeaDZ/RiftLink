#pragma once

#include "ui_topbar_model.h"

namespace ui_topbar {

/** Заполнить модель из telemetry / neighbors / GPS / BLE / Wi‑Fi / регион. */
void fill(Model& m);

}  // namespace ui_topbar
