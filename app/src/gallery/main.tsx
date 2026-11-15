import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { DemoGalleryApi } from './demo';
import { GalleryApp } from './GalleryApp';
import './gallery.css';

// Installable app (#263): the service worker only has to exist.
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('./gallery-sw.js').catch(() => undefined);
}

const root = document.getElementById('root');
if (!root) {
  throw new Error('Missing #root element');
}

// Only the demo backend exists so far; the real one follows with #259.
createRoot(root).render(
  <StrictMode>
    <GalleryApp api={new DemoGalleryApi()} />
  </StrictMode>,
);
