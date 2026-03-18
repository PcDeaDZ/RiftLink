# Анализ зависаний при нажатии кнопки

## Поток выполнения main loop

```
loop() {
  ota::update() / return
  sendHello, telemetry, gps
  Serial (readStringUntil - может блокировать!)
  radio::receive() → handlePacket()  ← displaySetLastMsg → drawScreen(4) [2-5 сек!]
  ble::update()
  msg_queue::update()
  routing::update()
  displayUpdate()  ← обработка кнопки → drawScreen [2-5 сек]
  delay(10)
}
```

## Возможные моменты зависания

### 1. **disp->display(false) — ожидание BUSY**
- GxEPD2 ждёт пин BUSY (EINK_BUSY=7)
- Таймаут: `_busy_timeout` микросекунд (обычно 15–20 сек для 2.13")
- При сбое железа (питание, контакт) BUSY может не опуститься → ожидание до таймаута
- После таймаута дисплей может остаться в неконсистентном состоянии → следующий display() может зависнуть

### 2. **handlePacket → displaySetLastMsg при получении сообщения**
- При `s_currentScreen == 4` (вкладка Msg) вызывается `drawScreen(4)`
- Блокировка 2–5 сек в середине loop
- BLE, msg_queue, routing не обновляются всё это время
- Кнопка не опрашивается

### 3. **Пикеры (регион, язык) — серия display()**
- Каждое короткое нажатие → новый `display()` (2–5 сек)
- Несколько быстрых нажатий = несколько display() подряд = 6–15+ сек блокировки
- Нет cooldown между display() в пикерах
- Воспринимается как зависание

### 4. **Serial.readStringUntil('\n')**
- Блокирует loop до ввода строки
- Маловероятно при работе без Serial

### 5. **Watchdog (30 сек)**
- При блокировке >30 сек без yield — перезагрузка
- В `_waitWhileBusy` есть `yield()` для ESP32, но длительные блокировки возможны

## Рекомендации

1. **Пикеры**: добавить cooldown перед display() — не вызывать display(), если с прошлого прошло <500 мс
2. **Watchdog**: использовать setBusyCallback для периодического esp_task_wdt_reset() во время ожидания BUSY
3. **displaySetLastMsg**: при получении сообщения не вызывать drawScreen сразу, если идёт cooldown; отложить обновление
