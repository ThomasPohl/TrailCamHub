import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { DemoGalleryApi } from './demo';
import { GalleryApp } from './GalleryApp';
import { HttpGalleryApi } from './http';
import { takeToken } from './token';
import type { GalleryApi } from './types';
import './gallery.css';

// Installable app (#263): the service worker only has to exist.
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('./gallery-sw.js').catch(() => undefined);
}

const root = document.getElementById('root');
if (!root) {
  throw new Error('Missing #root element');
}

/**
 * The real backend when the build has an API URL and the user opened the
 * set-up link once; the demo without an API URL or with `?demo`.
 */
function chooseApi(): GalleryApi | undefined {
  const baseUrl = import.meta.env.VITE_API_BASE_URL?.trim();
  const params = new URLSearchParams(window.location.search);
  if (!baseUrl || params.has('demo')) {
    // `?demo=admin` shows the admin view as well.
    // `?demo=admin-code` asks for a code first (123456), like the real one.
    const demo = params.get('demo');
    const admin = demo === 'admin' || demo === 'admin-code';
    return new DemoGalleryApi(undefined, undefined, admin, demo === 'admin-code');
  }
  const token = takeToken();
  const debug = params.has('debug');
  return token ? new HttpGalleryApi(baseUrl, token, undefined, debug) : undefined;
}

const api = chooseApi();

createRoot(root).render(
  <StrictMode>
    {api ? (
      <GalleryApp api={api} />
    ) : (
      <div className="gallery">
        <h1>Wildkamera</h1>
        <p className="g-folder">
          Diese Seite ist noch nicht eingerichtet. Bitte den Einrichtungslink öffnen, den Sie
          bekommen haben.
        </p>
        <p className="g-hint">
          Zum Ausprobieren ohne Hub: <a href="?demo">Vorführung öffnen</a>
        </p>
      </div>
    )}
  </StrictMode>,
);
