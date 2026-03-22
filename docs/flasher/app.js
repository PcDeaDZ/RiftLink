const MANIFESTS = {
  "heltec-v3": "./manifests/heltec-v3.json",
  "heltec-v4": "./manifests/heltec-v4.json",
  "heltec-v3-paper": "./manifests/heltec-v3-paper.json",
};

const DEVICE_LABELS = {
  "heltec-v3": "Heltec V3",
  "heltec-v4": "Heltec V4",
  "heltec-v3-paper": "Heltec V3 Paper",
};

const selectEl = document.getElementById("deviceSelect");
const installEl = document.getElementById("installButton");
const infoEl = document.getElementById("manifestInfo");

function setDevice(deviceKey) {
  const manifest = MANIFESTS[deviceKey];
  if (!manifest || !installEl) return;

  installEl.setAttribute("manifest", manifest);
  const label = DEVICE_LABELS[deviceKey] ?? deviceKey;
  infoEl.textContent = `Выбрано: ${label}. Manifest: ${manifest}`;
}

setDevice(selectEl.value);

selectEl.addEventListener("change", (event) => {
  setDevice(event.target.value);
});

