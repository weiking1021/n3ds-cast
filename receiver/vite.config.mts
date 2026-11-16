import { defineConfig } from 'vite';

// The page is hosted from any path (a TV loads it from GitHub Pages under /<repository>/), so every URL in it is
// relative. The target is old enough for the Chromium of older Cast devices.
export default defineConfig({
  base: './',
  build: { outDir: 'dist', target: 'es2018', emptyOutDir: true },
  // when the page was built: shown next to the version in the debug panel
  define: { __BUILD__: JSON.stringify(new Date().toISOString().slice(5, 16).replace('T', ' ')) },
});
