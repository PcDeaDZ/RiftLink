const REPO_OWNER = "PcDeaDZ";
const REPO_NAME = "RiftLink";
const ROADMAP_RAW_URL = `https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/master/docs/CUSTOM_PROTOCOL_PLAN.md`;
const EMBEDDED_RELEASE_URL = "./embedded-release.json";
const EMBEDDED_RELEASE_RAW = `https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/master/docs/flasher/embedded-release.json`;
/** То же содержимое, что в репо: без запроса к api.github.com (CORS / preflight). Сначала same-origin, затем raw. */
const RELEASE_MIRROR_RELATIVE = "./release-github.json";
const RELEASE_MIRROR_RAW = `https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/master/docs/flasher/release-github.json`;
const RELEASE_FETCH_TIMEOUT_MS = 12000;
const LANG_STORAGE_KEY = "riftlink_flasher_lang";
const DEFAULT_RELEASES_PAGE_URL = `https://github.com/${REPO_OWNER}/${REPO_NAME}/releases`;

/** Данные из репозитория (версия прошивки в комплекте с флешером). */
let embeddedReleaseRaw = null;

const STATIC_MANIFESTS = {
  "heltec-v3": "./manifests/heltec-v3.json",
  "heltec-v4": "./manifests/heltec-v4.json",
  "heltec-v3-paper": "./manifests/heltec-v3-paper.json",
  "lilygo-t-lora-pager": "./manifests/lilygo-t-lora-pager.json",
  "lilygo-t-beam": "./manifests/lilygo-t-beam.json",
};

/** Короткие метки платы / радио / экрана для выбранного устройства. */
const DEVICE_CHIPS = {
  "heltec-v3": ["ESP32-S3", "SX1262", "OLED"],
  "heltec-v4": ["ESP32-S3", "SX1262", "OLED"],
  "heltec-v3-paper": ["ESP32-S3", "SX1262", "E-Ink"],
  "lilygo-t-lora-pager": ["ESP32-S3", "SX1262", "ST7796"],
  "lilygo-t-beam": ["ESP32", "SX1262", "OLED"],
};

/** Подсказка под селектором (только для части плат). */
const DEVICE_HINTS = {
  ru: {
    "heltec-v3-paper":
      "E-Ink: при переходе с OLED-прошивки согласуйте Erase, чтобы избежать артефактов состояния.",
    "lilygo-t-lora-pager":
      "Пейджер: энкодер переключает вкладки; убедитесь, что кабель передаёт данные.",
    "lilygo-t-beam":
      "T-Beam V1.1/V1.2: при сомнениях проверьте разводку платы в документации LilyGO.",
  },
  en: {
    "heltec-v3-paper":
      "E-Ink: when moving from OLED firmware, Erase is recommended to avoid stale state.",
    "lilygo-t-lora-pager":
      "Pager: the encoder switches tabs; use a data-capable USB cable.",
    "lilygo-t-beam":
      "T-Beam V1.1/V1.2: verify your board revision in LilyGO docs if unsure.",
  },
};

const I18N = {
  ru: {
    title: "Web Flash Tool",
    subtitle:
      "Прошивка устройств через браузер с выбором модели и автоматической загрузкой правильного manifest.",
    flashStepsAria: "Шаги прошивки",
    releaseKpisAria: "Версии последнего релиза",
    step1: "Выберите модель устройства",
    step2: "Подключите USB и разрешите доступ в браузере",
    step3: "Нажмите Install и дождитесь завершения",
    envSecureOk: "Безопасный контекст (HTTPS / localhost)",
    envSecureWarn: "Нужен HTTPS или localhost — иначе Web Serial недоступен",
    envSerialOk: "Web Serial доступен в этом браузере",
    envSerialWarn: "Нужен Chrome, Edge или Opera (Web Serial)",
    deviceLabel: "Выберите устройство",
    eraseNote:
      "Примечание: окно подтверждения Erase device показывает сам ESP Web Tools, оно может быть на английском.",
    latestRelease: "Последний релиз",
    releaseTagLabel: "Тег:",
    firmwareLabel: "Прошивка:",
    appLabel: "Приложение:",
    aboutTitle: "О проекте RiftLink",
    aboutLead:
      "RiftLink — mesh-платформа для Heltec V3/V4/Paper с собственным протоколом, BLE/Wi-Fi управлением и мобильным приложением.",
    aboutFeature1Title: "LoRa Mesh",
    aboutFeature1Text:
      "Собственный сетевой протокол для автономной связи между узлами без сотовой сети.",
    aboutFeature2Title: "Безопасность",
    aboutFeature2Text:
      "Криптография и групповые механики с разделением ролей, маршрутов и состояний доставки.",
    aboutFeature3Title: "Клиенты",
    aboutFeature3Text:
      "Android-приложение, web-флешер и инструменты диагностики/восстановления для устройства.",
    linkGithub: "GitHub репозиторий",
    linkReleases: "Скачать релизы",
    linkDocs: "Документация",
    roadmapTitle: "Roadmap",
    roadmapLead: "Ближайшие пункты берутся автоматически из плана проекта.",
    roadmapFallback: "Roadmap сейчас недоступен.",
    whatsNewTitle: "Что нового в этой версии",
    whatsNewFallback: "Описание изменений пока не добавлено в release notes.",
    faqTitle: "FAQ",
    faqQ1: "Устройство не определяется в браузере. Что делать?",
    faqA1:
      "Проверьте USB-кабель (должен быть data), попробуйте другой порт, перезапустите вкладку и переподключите устройство. Закройте Serial Monitor/PlatformIO/другие вкладки, которые могут держать COM-порт открытым.",
    faqQ2: "Нужно ли стирать устройство перед прошивкой?",
    faqA2:
      "При переходе между разными конфигурациями (например, V3/Paper) лучше соглашаться на Erase. Это удалит старые данные и снизит риск конфликтов.",
    faqQ3: "Прошивка дошла до конца, но устройство ведет себя странно.",
    faqA3:
      "Выполните повторную прошивку с Erase и убедитесь, что выбрана правильная модель (V3, V4, Paper, LilyGO T-Lora Pager или LilyGO T-Beam).",
    footerTitle: "Сообщество и поддержка",
    footerIssues: "Сообщить о проблеме",
    footerReleases: "Все релизы",
    footerRepo: "Главная страница проекта",
    footerTelegram: "RiftLink в Telegram — присоединиться",
    tipsTitle: "Перед прошивкой",
    tip1: "Используйте Chrome/Edge/Opera (нужен Web Serial).",
    tip2: "Подключайте USB-кабель с поддержкой передачи данных.",
    tip3: "Для V4 при необходимости войдите в bootloader через PRG.",
    selected: "Выбрано",
    manifestLabel: "Manifest",
    loading: "загрузка...",
    latestUnavailable: "latest недоступен",
    loadFailed: "не удалось загрузить",
    apkNotFound: "APK пока не найден в latest release",
    apkBrowseReleases: "Релизы и APK на GitHub →",
    apkDownload: "Скачать APK (arm64-v8a) v{version}",
    deviceNames: {
      "heltec-v3": "Heltec WiFi LoRa 32 V3 (OLED)",
      "heltec-v4": "Heltec WiFi LoRa 32 V4 (OLED)",
      "heltec-v3-paper": "Heltec Wireless Paper (V3 Paper, E-Ink)",
      "lilygo-t-lora-pager": "LilyGO T-Lora Pager (SX1262, ST7796)",
      "lilygo-t-beam": "LilyGO T-Beam V1.1/V1.2 (ESP32, SX1262, OLED)",
    },
  },
  en: {
    title: "Web Flash Tool",
    subtitle:
      "Flash RiftLink devices from browser with model selection and automatic manifest loading.",
    flashStepsAria: "Flashing steps",
    releaseKpisAria: "Latest release versions",
    step1: "Pick your hardware model",
    step2: "Connect USB and allow browser access",
    step3: "Click Install and wait until finished",
    envSecureOk: "Secure context (HTTPS / localhost)",
    envSecureWarn: "Use HTTPS or localhost — Web Serial requires a secure context",
    envSerialOk: "Web Serial is available in this browser",
    envSerialWarn: "Use Chrome, Edge, or Opera (Web Serial)",
    deviceLabel: "Select device",
    eraseNote:
      "Note: the Erase device confirmation dialog is rendered by ESP Web Tools and may stay in English.",
    latestRelease: "Latest release",
    releaseTagLabel: "Tag:",
    firmwareLabel: "Firmware:",
    appLabel: "Application:",
    aboutTitle: "About RiftLink",
    aboutLead:
      "RiftLink is a mesh platform for Heltec V3/V4/Paper with a custom protocol, BLE/Wi-Fi control, and mobile app.",
    aboutFeature1Title: "LoRa Mesh",
    aboutFeature1Text:
      "Custom networking protocol for autonomous node-to-node communication without cellular network.",
    aboutFeature2Title: "Security",
    aboutFeature2Text:
      "Cryptography and group mechanics with role separation, routes, and delivery state tracking.",
    aboutFeature3Title: "Clients",
    aboutFeature3Text:
      "Android app, web flasher, and recovery/diagnostic tools for the device.",
    linkGithub: "GitHub repository",
    linkReleases: "Download releases",
    linkDocs: "Documentation",
    roadmapTitle: "Roadmap",
    roadmapLead: "Nearest items are loaded automatically from the project plan.",
    roadmapFallback: "Roadmap is currently unavailable.",
    whatsNewTitle: "What's new in this release",
    whatsNewFallback: "Release notes are not available yet.",
    faqTitle: "FAQ",
    faqQ1: "Browser cannot detect the device. What should I do?",
    faqA1:
      "Check the USB cable (must support data), try another port, reload the tab, and reconnect the device. Close Serial Monitor/PlatformIO/other tabs that may keep the COM port open.",
    faqQ2: "Should I erase the device before flashing?",
    faqA2:
      "When switching between different configurations (for example V3/Paper), Erase is recommended to avoid old state conflicts.",
    faqQ3: "Flashing completed, but device behavior is unstable.",
    faqA3:
      "Flash again with Erase enabled and make sure you selected the correct hardware model (V3, V4, Paper, LilyGO T-Lora Pager, or LilyGO T-Beam).",
    footerTitle: "Community and support",
    footerIssues: "Report an issue",
    footerReleases: "All releases",
    footerRepo: "Project homepage",
    footerTelegram: "RiftLink on Telegram — join",
    tipsTitle: "Before flashing",
    tip1: "Use Chrome/Edge/Opera (Web Serial is required).",
    tip2: "Use a USB cable with data support.",
    tip3: "For V4, enter bootloader mode with PRG if needed.",
    selected: "Selected",
    manifestLabel: "Manifest",
    loading: "loading...",
    latestUnavailable: "latest unavailable",
    loadFailed: "failed to load",
    apkNotFound: "APK not found in latest release",
    apkBrowseReleases: "Releases & APK on GitHub →",
    apkDownload: "Download APK (arm64-v8a) v{version}",
    deviceNames: {
      "heltec-v3": "Heltec WiFi LoRa 32 V3 (OLED)",
      "heltec-v4": "Heltec WiFi LoRa 32 V4 (OLED)",
      "heltec-v3-paper": "Heltec Wireless Paper (V3 Paper, E-Ink)",
      "lilygo-t-lora-pager": "LilyGO T-Lora Pager (SX1262, ST7796)",
      "lilygo-t-beam": "LilyGO T-Beam V1.1/V1.2 (ESP32, SX1262, OLED)",
    },
  },
};

const selectEl = document.getElementById("deviceSelect");
const installEl = document.getElementById("installButton");
const infoEl = document.getElementById("manifestInfo");
const eraseNoteEl = document.getElementById("eraseNote");
const titleTextEl = document.getElementById("titleText");
const subtitleTextEl = document.getElementById("subtitleText");
const deviceLabelEl = document.getElementById("deviceLabel");
const latestReleaseTitleEl = document.getElementById("latestReleaseTitle");
const releaseTagLabelEl = document.getElementById("releaseTagLabel");
const firmwareLabelEl = document.getElementById("firmwareLabel");
const appLabelEl = document.getElementById("appLabel");
const aboutTitleEl = document.getElementById("aboutTitle");
const aboutLeadEl = document.getElementById("aboutLead");
const aboutFeature1TitleEl = document.getElementById("aboutFeature1Title");
const aboutFeature1TextEl = document.getElementById("aboutFeature1Text");
const aboutFeature2TitleEl = document.getElementById("aboutFeature2Title");
const aboutFeature2TextEl = document.getElementById("aboutFeature2Text");
const aboutFeature3TitleEl = document.getElementById("aboutFeature3Title");
const aboutFeature3TextEl = document.getElementById("aboutFeature3Text");
const linkGithubEl = document.getElementById("linkGithub");
const linkReleasesEl = document.getElementById("linkReleases");
const linkDocsEl = document.getElementById("linkDocs");
const roadmapTitleEl = document.getElementById("roadmapTitle");
const roadmapLeadEl = document.getElementById("roadmapLead");
const roadmapListEl = document.getElementById("roadmapList");
const whatsNewTitleEl = document.getElementById("whatsNewTitle");
const whatsNewListEl = document.getElementById("whatsNewList");
const faqTitleEl = document.getElementById("faqTitle");
const faqQ1El = document.getElementById("faqQ1");
const faqA1El = document.getElementById("faqA1");
const faqQ2El = document.getElementById("faqQ2");
const faqA2El = document.getElementById("faqA2");
const faqQ3El = document.getElementById("faqQ3");
const faqA3El = document.getElementById("faqA3");
const tipsTitleEl = document.getElementById("tipsTitle");
const tip1El = document.getElementById("tip1");
const tip2El = document.getElementById("tip2");
const tip3El = document.getElementById("tip3");
const footerTitleEl = document.getElementById("footerTitle");
const footerLinkIssuesEl = document.getElementById("footerLinkIssues");
const footerLinkReleasesEl = document.getElementById("footerLinkReleases");
const footerLinkRepoEl = document.getElementById("footerLinkRepo");
const footerLinkTelegramEl = document.getElementById("footerLinkTelegram");
const releaseTagEl = document.getElementById("releaseTag");
const firmwareVersionEl = document.getElementById("firmwareVersion");
const appVersionEl = document.getElementById("appVersion");
const apkDownloadLinkEl = document.getElementById("apkDownloadLink");
const langRuBtn = document.getElementById("langRu");
const langEnBtn = document.getElementById("langEn");
const flashStepsEl = document.getElementById("flashSteps");
const step1TextEl = document.getElementById("step1Text");
const step2TextEl = document.getElementById("step2Text");
const step3TextEl = document.getElementById("step3Text");
const envBadgesEl = document.getElementById("envBadges");
const deviceMetaEl = document.getElementById("deviceMeta");
const deviceHintEl = document.getElementById("deviceHint");
const releaseKpisEl = document.getElementById("releaseKpis");

const runtimeManifestUrls = { ...STATIC_MANIFESTS };
let currentLang = "ru";
let releaseState = {
  tag: "...",
  firmwareVersion: "...",
  appVersion: "...",
  apkUrl: null,
  notes: [],
};
let roadmapState = [];

function getLangFromEnv() {
  const saved = localStorage.getItem(LANG_STORAGE_KEY);
  if (saved === "ru" || saved === "en") return saved;
  return (navigator.language || "").toLowerCase().startsWith("ru") ? "ru" : "en";
}

function t(key) {
  return I18N[currentLang][key] ?? I18N.ru[key] ?? key;
}

function tWithVersion(templateKey, version) {
  return t(templateKey).replace("{version}", version);
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/"/g, "&quot;");
}

function renderEnvBadges() {
  if (!envBadgesEl) return;
  const secureOk =
    typeof window !== "undefined" &&
    (window.isSecureContext === true ||
      (typeof location !== "undefined" &&
        (location.protocol === "https:" ||
          location.hostname === "localhost" ||
          location.hostname === "127.0.0.1")));
  const serialOk =
    typeof navigator !== "undefined" && typeof navigator.serial !== "undefined";

  envBadgesEl.innerHTML = "";
  const mk = (ok, text) => {
    const span = document.createElement("span");
    span.className = `env-badge ${ok ? "ok" : "warn"}`;
    span.textContent = text;
    envBadgesEl.appendChild(span);
  };
  mk(secureOk, secureOk ? t("envSecureOk") : t("envSecureWarn"));
  mk(serialOk, serialOk ? t("envSerialOk") : t("envSerialWarn"));
}

function renderDeviceMeta(deviceKey) {
  if (!deviceMetaEl) return;
  deviceMetaEl.innerHTML = "";
  const chips = DEVICE_CHIPS[deviceKey] ?? [];
  for (const c of chips) {
    const span = document.createElement("span");
    span.className = "device-chip";
    span.textContent = c;
    deviceMetaEl.appendChild(span);
  }

  if (deviceHintEl) {
    const hintMap = DEVICE_HINTS[currentLang] ?? DEVICE_HINTS.ru;
    const hint = hintMap[deviceKey];
    if (hint) {
      deviceHintEl.textContent = hint;
      deviceHintEl.hidden = false;
    } else {
      deviceHintEl.textContent = "";
      deviceHintEl.hidden = true;
    }
  }
}

function setManifestLine(deviceKey, manifest) {
  if (!infoEl) return;
  const label = I18N[currentLang].deviceNames[deviceKey] ?? deviceKey;
  const prefix = `${t("selected")}: ${label}. ${t("manifestLabel")}: `;
  infoEl.innerHTML = `${escapeHtml(prefix)}<code class="manifest-code">${escapeHtml(manifest)}</code>`;
}

function renderDeviceOptions() {
  const names = I18N[currentLang].deviceNames;
  for (const option of selectEl.options) {
    option.textContent = names[option.value] ?? option.value;
  }
}

function renderReleaseState() {
  if (releaseTagEl) releaseTagEl.textContent = releaseState.tag;
  if (firmwareVersionEl) firmwareVersionEl.textContent = releaseState.firmwareVersion;
  if (appVersionEl) appVersionEl.textContent = releaseState.appVersion;
  setApkLink(releaseState.apkUrl, releaseState.appVersion);
  renderWhatsNew();
}

function normalizeReleaseNotes(bodyText) {
  if (!bodyText || typeof bodyText !== "string") return [];
  const lines = bodyText
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean);
  const bulletLines = lines
    .filter((line) => line.startsWith("- ") || line.startsWith("* "))
    .map((line) => line.replace(/^[-*]\s+/, "").trim())
    .filter(Boolean);
  if (bulletLines.length > 0) return bulletLines.slice(0, 6);

  const plain = bodyText.replace(/\s+/g, " ").trim();
  if (!plain) return [];
  return plain
    .split(/(?<=[.!?])\s+/)
    .map((part) => part.trim())
    .filter(Boolean)
    .slice(0, 4);
}

function renderWhatsNew() {
  if (!whatsNewListEl) return;
  whatsNewListEl.innerHTML = "";
  const notes = Array.isArray(releaseState.notes) ? releaseState.notes : [];
  if (notes.length === 0) {
    const li = document.createElement("li");
    li.textContent = t("whatsNewFallback");
    whatsNewListEl.appendChild(li);
    return;
  }
  for (const note of notes) {
    const li = document.createElement("li");
    li.textContent = note;
    whatsNewListEl.appendChild(li);
  }
}

function normalizeRoadmapText(raw) {
  return raw
    .replace(/^\-\s*\[[ xX]\]\s*/, "")
    .replace(/^\-\s*/, "")
    .replace(/\*\*/g, "")
    .replace(/`/g, "")
    .replace(/\[(.*?)\]\((.*?)\)/g, "$1")
    .trim();
}

function extractRoadmapItems(markdown) {
  if (!markdown || typeof markdown !== "string") return [];
  const lines = markdown.replace(/\r/g, "").split("\n");
  const sectionStart = lines.findIndex((line) =>
    /^##\s*6\.1/.test(line) || /Планы на будущее|future plans/i.test(line),
  );

  let source = lines;
  if (sectionStart >= 0) {
    const sub = [];
    for (let i = sectionStart + 1; i < lines.length; i += 1) {
      if (/^##\s+/.test(lines[i])) break;
      sub.push(lines[i]);
    }
    source = sub;
  }

  const bullet = source
    .map((line) => line.trim())
    .filter((line) => /^-\s+/.test(line) || /^-\s*\[[ xX]\]\s+/.test(line))
    .map(normalizeRoadmapText)
    .filter(Boolean);

  const uncheckedGlobal = lines
    .map((line) => line.trim())
    .filter((line) => /^-\s*\[\s\]\s+/.test(line))
    .map(normalizeRoadmapText)
    .filter(Boolean);

  const merged = [...bullet, ...uncheckedGlobal];
  const unique = [];
  for (const item of merged) {
    if (!unique.includes(item)) unique.push(item);
  }
  return unique.slice(0, 8);
}

function renderRoadmap() {
  if (!roadmapListEl) return;
  roadmapListEl.innerHTML = "";
  const items = Array.isArray(roadmapState) ? roadmapState : [];
  if (items.length === 0) {
    const li = document.createElement("li");
    li.textContent = t("roadmapFallback");
    roadmapListEl.appendChild(li);
    return;
  }
  for (const item of items) {
    const li = document.createElement("li");
    li.textContent = item;
    roadmapListEl.appendChild(li);
  }
}

function embeddedNotesForLang() {
  if (!embeddedReleaseRaw) return [];
  const key = currentLang === "en" ? "notesEn" : "notesRu";
  const arr = embeddedReleaseRaw[key];
  return Array.isArray(arr) ? arr : [];
}

function applyReleaseStateFromEmbedded(data) {
  if (!data) return;
  const fv = String(data.firmwareVersion ?? "?");
  const av = String(data.appVersion ?? data.firmwareVersion ?? "?");
  const tagRaw = data.tag != null ? String(data.tag) : `v${fv}`;
  const tag = tagRaw.startsWith("v") ? tagRaw : `v${tagRaw}`;
  const embeddedApk =
    typeof data.apkDownloadUrl === "string" && data.apkDownloadUrl.trim().length > 0
      ? data.apkDownloadUrl.trim()
      : null;
  releaseState = {
    tag,
    firmwareVersion: fv,
    appVersion: av,
    apkUrl: embeddedApk,
    notes: embeddedNotesForLang(),
  };
}

async function fetchWithTimeout(url, options = {}, timeoutMs = RELEASE_FETCH_TIMEOUT_MS) {
  const controller = new AbortController();
  const id = setTimeout(() => controller.abort(), timeoutMs);
  try {
    return await fetch(url, { ...options, signal: controller.signal });
  } finally {
    clearTimeout(id);
  }
}

async function fetchEmbeddedReleaseJson() {
  let res = await fetchWithTimeout(EMBEDDED_RELEASE_URL, { cache: "no-store" });
  if (res.ok) return res.json();
  res = await fetchWithTimeout(EMBEDDED_RELEASE_RAW, { cache: "no-store" });
  if (!res.ok) return null;
  return res.json();
}

async function loadEmbeddedRelease() {
  try {
    const data = await fetchEmbeddedReleaseJson();
    if (!data || typeof data !== "object") {
      console.warn("embedded-release.json: same-origin and raw both failed or empty");
      return null;
    }
    embeddedReleaseRaw = data;
    applyReleaseStateFromEmbedded(data);
    renderReleaseState();
    return data;
  } catch (e) {
    console.warn("embedded-release.json unavailable:", e);
    return null;
  }
}

function renderLanguage() {
  document.documentElement.lang = currentLang;
  if (embeddedReleaseRaw) {
    releaseState.notes = embeddedNotesForLang();
  }
  if (titleTextEl) titleTextEl.textContent = t("title");
  if (subtitleTextEl) subtitleTextEl.textContent = t("subtitle");
  if (deviceLabelEl) deviceLabelEl.textContent = t("deviceLabel");
  if (eraseNoteEl) eraseNoteEl.textContent = t("eraseNote");
  if (latestReleaseTitleEl) latestReleaseTitleEl.textContent = t("latestRelease");
  if (releaseTagLabelEl) releaseTagLabelEl.textContent = t("releaseTagLabel");
  if (firmwareLabelEl) firmwareLabelEl.textContent = t("firmwareLabel");
  if (appLabelEl) appLabelEl.textContent = t("appLabel");
  if (aboutTitleEl) aboutTitleEl.textContent = t("aboutTitle");
  if (aboutLeadEl) aboutLeadEl.textContent = t("aboutLead");
  if (aboutFeature1TitleEl) aboutFeature1TitleEl.textContent = t("aboutFeature1Title");
  if (aboutFeature1TextEl) aboutFeature1TextEl.textContent = t("aboutFeature1Text");
  if (aboutFeature2TitleEl) aboutFeature2TitleEl.textContent = t("aboutFeature2Title");
  if (aboutFeature2TextEl) aboutFeature2TextEl.textContent = t("aboutFeature2Text");
  if (aboutFeature3TitleEl) aboutFeature3TitleEl.textContent = t("aboutFeature3Title");
  if (aboutFeature3TextEl) aboutFeature3TextEl.textContent = t("aboutFeature3Text");
  if (linkGithubEl) linkGithubEl.textContent = t("linkGithub");
  if (linkReleasesEl) linkReleasesEl.textContent = t("linkReleases");
  if (linkDocsEl) linkDocsEl.textContent = t("linkDocs");
  if (roadmapTitleEl) roadmapTitleEl.textContent = t("roadmapTitle");
  if (roadmapLeadEl) roadmapLeadEl.textContent = t("roadmapLead");
  if (whatsNewTitleEl) whatsNewTitleEl.textContent = t("whatsNewTitle");
  if (faqTitleEl) faqTitleEl.textContent = t("faqTitle");
  if (faqQ1El) faqQ1El.textContent = t("faqQ1");
  if (faqA1El) faqA1El.textContent = t("faqA1");
  if (faqQ2El) faqQ2El.textContent = t("faqQ2");
  if (faqA2El) faqA2El.textContent = t("faqA2");
  if (faqQ3El) faqQ3El.textContent = t("faqQ3");
  if (faqA3El) faqA3El.textContent = t("faqA3");
  if (tipsTitleEl) tipsTitleEl.textContent = t("tipsTitle");
  if (tip1El) tip1El.textContent = t("tip1");
  if (tip2El) tip2El.textContent = t("tip2");
  if (tip3El) tip3El.textContent = t("tip3");
  if (footerTitleEl) footerTitleEl.textContent = t("footerTitle");
  if (footerLinkIssuesEl) footerLinkIssuesEl.textContent = t("footerIssues");
  if (footerLinkReleasesEl) footerLinkReleasesEl.textContent = t("footerReleases");
  if (footerLinkRepoEl) footerLinkRepoEl.textContent = t("footerRepo");
  if (footerLinkTelegramEl) footerLinkTelegramEl.textContent = t("footerTelegram");
  if (langRuBtn) langRuBtn.classList.toggle("active", currentLang === "ru");
  if (langEnBtn) langEnBtn.classList.toggle("active", currentLang === "en");
  if (flashStepsEl) flashStepsEl.setAttribute("aria-label", t("flashStepsAria"));
  if (step1TextEl) step1TextEl.textContent = t("step1");
  if (step2TextEl) step2TextEl.textContent = t("step2");
  if (step3TextEl) step3TextEl.textContent = t("step3");
  if (releaseKpisEl) releaseKpisEl.setAttribute("aria-label", t("releaseKpisAria"));
  renderEnvBadges();
  renderDeviceOptions();
  renderReleaseState();
  renderRoadmap();
  setDevice(selectEl.value);
}

function setLanguage(lang) {
  currentLang = lang === "en" ? "en" : "ru";
  localStorage.setItem(LANG_STORAGE_KEY, currentLang);
  renderLanguage();
}

function parseSemverFromName(name) {
  const match = name.match(/(\d+\.\d+\.\d+)/);
  return match ? match[1] : null;
}

function setApkLink(url, versionText) {
  if (!apkDownloadLinkEl) return;
  const trimmed = url && typeof url === "string" ? url.trim() : "";

  if (!trimmed) {
    apkDownloadLinkEl.href = DEFAULT_RELEASES_PAGE_URL;
    apkDownloadLinkEl.textContent = t("apkBrowseReleases");
    apkDownloadLinkEl.setAttribute("aria-disabled", "false");
    apkDownloadLinkEl.setAttribute("target", "_blank");
    apkDownloadLinkEl.setAttribute("rel", "noopener noreferrer");
    return;
  }

  const titleVersion = versionText ?? "unknown";
  apkDownloadLinkEl.href = trimmed;
  apkDownloadLinkEl.textContent = tWithVersion("apkDownload", titleVersion);
  apkDownloadLinkEl.setAttribute("aria-disabled", "false");
  apkDownloadLinkEl.setAttribute("target", "_blank");
  apkDownloadLinkEl.setAttribute("rel", "noopener noreferrer");
}

async function fetchReleaseMirrorJson() {
  let res = await fetchWithTimeout(RELEASE_MIRROR_RELATIVE, { cache: "no-store" });
  if (res.ok) return res.json();
  res = await fetchWithTimeout(RELEASE_MIRROR_RAW, { cache: "no-store" });
  if (!res.ok) return null;
  return res.json();
}

/**
 * Дополняет embedded: APK и releaseBody из release-github.json
 * (same-origin или raw — простой GET без api.github.com / CORS preflight).
 */
async function loadReleaseMirror() {
  const snapshot = {
    ...releaseState,
    notes: Array.isArray(releaseState.notes) ? [...releaseState.notes] : [],
  };

  if (!embeddedReleaseRaw) {
    releaseState = {
      tag: t("latestUnavailable"),
      firmwareVersion: t("loadFailed"),
      appVersion: t("loadFailed"),
      apkUrl: null,
      notes: [],
    };
    renderReleaseState();
    return;
  }

  try {
    const data = await fetchReleaseMirrorJson();
    if (!data || typeof data !== "object") {
      return;
    }

    const mirrorApk =
      typeof data.apkDownloadUrl === "string" && data.apkDownloadUrl.trim().length > 0
        ? data.apkDownloadUrl.trim()
        : null;
    const apkUrl = mirrorApk ?? snapshot.apkUrl;

    const mirrorNotes = normalizeReleaseNotes(
      typeof data.releaseBody === "string" ? data.releaseBody : "",
    );
    const useMirrorNotes = mirrorNotes.length > 0;

    let appVersion = snapshot.appVersion;
    if (apkUrl) {
      const fromUrl = parseSemverFromName(mirrorApk || apkUrl);
      appVersion = fromUrl || snapshot.appVersion;
    }

    releaseState = {
      ...snapshot,
      apkUrl,
      appVersion,
      notes: useMirrorNotes ? mirrorNotes : snapshot.notes,
    };
    renderReleaseState();
  } catch (error) {
    releaseState = {
      ...snapshot,
      notes: [...snapshot.notes],
    };
    renderReleaseState();
    console.warn("release-github.json:", error);
  }
}

async function loadRoadmap() {
  try {
    const response = await fetchWithTimeout(ROADMAP_RAW_URL, { cache: "no-store" }, RELEASE_FETCH_TIMEOUT_MS);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const markdown = await response.text();
    roadmapState = extractRoadmapItems(markdown);
    renderRoadmap();
  } catch (error) {
    roadmapState = [];
    renderRoadmap();
    console.error("Failed to load roadmap:", error);
  }
}

function setDevice(deviceKey) {
  const manifest = runtimeManifestUrls[deviceKey] ?? STATIC_MANIFESTS[deviceKey];
  if (!manifest || !installEl) return;
  installEl.setAttribute("manifest", manifest);
  renderDeviceMeta(deviceKey);
  setManifestLine(deviceKey, manifest);
}

selectEl.addEventListener("change", (event) => {
  setDevice(event.target.value);
});

langRuBtn.addEventListener("click", () => setLanguage("ru"));
langEnBtn.addEventListener("click", () => setLanguage("en"));

currentLang = getLangFromEnv();
releaseState = {
  tag: t("loading"),
  firmwareVersion: t("loading"),
  appVersion: t("loading"),
  apkUrl: null,
  notes: [],
};
renderLanguage();
await loadEmbeddedRelease();
await loadReleaseMirror();
await loadRoadmap();
setDevice(selectEl.value);

