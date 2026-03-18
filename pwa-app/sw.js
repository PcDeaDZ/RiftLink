const CACHE = 'riftlink-app-v1';

self.addEventListener('install', (e) => {
  e.waitUntil(
    caches.open(CACHE).then((cache) =>
      cache.addAll(['./', './index.html', './manifest.json', './style.css', './app.js'])
    )
  );
  self.skipWaiting();
});

self.addEventListener('fetch', (e) => {
  e.respondWith(caches.match(e.request).then((r) => r || fetch(e.request)));
});
