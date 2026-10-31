import { defineConfig } from 'vite';
import { createReadStream, existsSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { readFile, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

export default defineConfig({
  plugins: [
    {
      name: 'serve-local-release-artifacts',
      apply: 'serve',
      configureServer(server) {
        const artifactDir = fileURLToPath(new URL('../../release-artifacts/', import.meta.url));
        const available = new Map([
          ['StageTracks-Setup-windows-x64.exe', 'StageTracks-Setup-windows-x64.exe'],
          ['StageTracks-portable-windows-x64.zip', 'StageTracks-portable-windows-x64.zip'],
          ['Reaper Control Studio.exe', 'Reaper Control Studio.exe'],
        ]);
        server.middlewares.use((req, res, next) => {
          if (req.method !== 'GET' && req.method !== 'HEAD') return next();
          let pathname;
          try { pathname = decodeURIComponent(new URL(req.url || '/', 'http://localhost').pathname); }
          catch { return next(); }
          const fileName = pathname.startsWith('/downloads/') ? pathname.slice('/downloads/'.length) : '';
          const artifactName = available.get(fileName);
          if (!artifactName) return next();
          const artifactPath = resolve(artifactDir, artifactName);
          if (!existsSync(artifactPath)) {
            res.statusCode = 404;
            res.end('Falta compilar este artefacto. Ejecuta packaging/windows/build-release.ps1 desde la raiz del proyecto.');
            return;
          }
          const stat = statSync(artifactPath);
          res.statusCode = 200;
          res.setHeader('Content-Type', 'application/octet-stream');
          res.setHeader('Content-Disposition', `attachment; filename="${artifactName}"`);
          res.setHeader('Content-Length', stat.size);
          if (req.method === 'HEAD') { res.end(); return; }
          createReadStream(artifactPath).pipe(res);
        });
      },
    },
    {
      name: 'exclude-large-release-assets',
      apply: 'build',
      async closeBundle() {
        for (const file of ['StageTracks-Setup-windows-x64.exe', 'StageTracks-portable-windows-x64.zip', 'Reaper Control Studio.exe']) {
          await rm(new URL(`./dist/downloads/${file.split(' ').join('%20')}`, import.meta.url), { force: true });
        }
        const configPath = new URL('./dist/community-config.js', import.meta.url);
        let config = await readFile(configPath, 'utf8');
        const apiUrl = String(process.env.STAGETRACKS_COMMUNITY_API || '').trim().replace(/\/+$/, '');
        if (apiUrl) {
          config = config.replace(/window\.STAGETRACKS_COMMUNITY_API\s*=\s*window\.location\.origin\s*;/,
            () => `window.STAGETRACKS_COMMUNITY_API = ${JSON.stringify(apiUrl)};`);
        }
        const releaseUrls = [
          ['installer', 'STAGETRACKS_INSTALLER_URL'],
          ['portable', 'STAGETRACKS_PORTABLE_URL'],
          ['reaperStudioExe', 'STAGETRACKS_REAPER_STUDIO_EXE_URL'],
        ];
        for (const [field, envName] of releaseUrls) {
          const value = String(process.env[envName] || '').trim();
          if (value) {
            const fieldPattern = new RegExp(`(\\b${field}\\s*:\\s*)""`);
            config = config.replace(fieldPattern, (_match, prefix) => `${prefix}${JSON.stringify(value)}`);
          }
        }
        await writeFile(configPath, config, 'utf8');
      },
    },
  ],
  server: {
    host: '127.0.0.1',
    proxy: { '/api': 'http://127.0.0.1:47822', '/media': 'http://127.0.0.1:47822' },
  },
});
