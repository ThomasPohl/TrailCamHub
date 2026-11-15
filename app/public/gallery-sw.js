// Service worker of the gallery (#263). It caches nothing: the gallery needs
// the backend anyway, and every visit should load the newest version. It only
// makes the page installable as an app.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', () => {});
