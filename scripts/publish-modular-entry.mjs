import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const site = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const html = (await readFile(path.join(site, 'index.html'), 'utf8'))
  .replace('href="/styles.css"', 'href="site/public/styles.css"')
  .replace('src="/community-config.js"', 'src="site/public/community-config.js"')
  .replace('src="/app.js"', 'src="site/public/app.js"')
  .replaceAll('="/assets/', '="site/public/assets/');
await writeFile(path.resolve(site, '..', 'StageTracks — Sitio oficial.html'), html, 'utf8');
