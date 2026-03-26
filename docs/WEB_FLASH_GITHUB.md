# Веб-прошивка (GitHub Pages / ESP Web Tools)

Статическая страница для прошивки ESP32-S3 из браузера (Chrome / Edge / Opera, Web Serial).

## Где лежит код

| Путь | Назначение |
|------|------------|
| [`docs/flasher/index.html`](flasher/index.html) | Точка входа |
| [`docs/flasher/app.js`](flasher/app.js) | Логика: выбор устройства, манифесты, `embedded-release.json`, roadmap из [`CUSTOM_PROTOCOL_PLAN.md`](CUSTOM_PROTOCOL_PLAN.md) (raw) |
| [`docs/flasher/manifests/`](flasher/manifests/) | Манифесты ESP Web Tools: Heltec V3, V4, V3 Paper, LilyGO T-Lora Pager, LilyGO T-Beam |
| [`docs/flasher/firmware/`](flasher/firmware/) | Образы `*_full.bin` (после сборки прошивки копируются сюда для публикации) |
| [`docs/flasher/embedded-release.json`](flasher/embedded-release.json) | Версия прошивки/приложения, заметки релиза, опционально URL APK |
| [`docs/flasher/release-github.json`](flasher/release-github.json) | Зеркало: APK и `releaseBody` без обращения к api.github.com |

## Деплой

Репозиторий настроен так, что **GitHub Pages** может отдавать каталог `docs/` (или подкаталог — по настройкам проекта). Убедитесь, что в опубликованной ветке присутствуют актуальные `embedded-release.json`, манифесты и бинарники в `docs/flasher/firmware/`.

Блок «Последний релиз» и Roadmap подгружаются с **same-origin**; при 404 — fallback на `raw.githubusercontent.com/.../master/docs/flasher/...` (см. [`app.js`](flasher/app.js)).

## Сборка образов локально

После `pio run -e <env>` merged-файлы появляются в `firmware/out/<env>/`. Скопируйте `*_full.bin` в `docs/flasher/firmware/` с именами из манифестов (`heltec_v3_full.bin`, `heltec_v4_full.bin`, `heltec_v3_paper_full.bin`, `lilygo_t_lora_pager_full.bin`, `lilygo_t_beam_full.bin`). Скрипт: [`docs/flasher/scripts/update-manifests.ps1`](flasher/scripts/update-manifests.ps1) — обновление поля `version` в JSON манифестах (параметр `-Version`, например `1.5.25`).

**Текущая версия артефактов** должна совпадать с [`embedded-release.json`](flasher/embedded-release.json) (`firmwareVersion`, `appVersion`) и с [`firmware/src/version.h`](../firmware/src/version.h).

## Roadmap и «Что нового» на странице

- **Roadmap** — автоматически из раздела **6.1** [`CUSTOM_PROTOCOL_PLAN.md`](../CUSTOM_PROTOCOL_PLAN.md) (same-origin или raw GitHub).
- **Последний релиз / Что нового** — из [`embedded-release.json`](flasher/embedded-release.json) (`notesRu` / `notesEn`), без вызова GitHub API.
