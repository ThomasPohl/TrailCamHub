import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [react()],
  // Relative asset paths: the app is served from a sub-path on GitHub Pages
  // (e.g. https://<user>.github.io/TrailCamHub/app/).
  base: './',
  build: {
    outDir: 'dist',
    sourcemap: true,
    // Two pages: the Mini App (index.html) and the gallery for the laptop (#258).
    rollupOptions: {
      input: {
        main: 'index.html',
        gallery: 'gallery.html',
      },
    },
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/test-setup.ts'],
    css: false,
  },
});
