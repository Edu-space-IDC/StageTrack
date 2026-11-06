# Migracion de la API de StageTracks a Cloudflare Workers

Esta API conserva Google Drive como almacenamiento y reemplaza la transferencia por Render. El Worker entrega la lista, valida el ZIP consultando el directorio central y el manifiesto mediante solicitudes Range, y transmite las descargas desde Drive. Las subidas llegan en bloques de **80 MiB** a Drive; el limite total sigue siendo **2 GiB**. Las transferencias grandes no se guardan en KV ni en memoria y no pasan por Render.

## Recursos gratuitos necesarios

- Cloudflare Workers Free y un subdominio `workers.dev` habilitado.
- Un namespace de Workers KV para guardar sesiones resumibles.
- El Apps Script actual, con acceso al mismo Drive donde ya estan las secuencias.
- Los secretos `APPS_SCRIPT_URL`, `API_TOKEN` y `COMMUNITY_ADMIN_TOKEN` configurados para el Worker.

No crees un bucket R2: las secuencias actuales permanecen en Drive. El plan Workers Free incluye 100.000 solicitudes diarias y Cloudflare no cobra egreso; cada bloque de 80 MiB de un paquete de 2 GiB usa una solicitud del Worker. [Precios](https://developers.cloudflare.com/workers/platform/pricing/) y [limites](https://developers.cloudflare.com/workers/platform/limits/).

## Primer despliegue

1. En Cloudflare habilita Workers y crea un namespace KV llamado `COMMUNITY_KV`.
2. Copia el ID del namespace en `cloudflare/wrangler.toml`.
3. Desde el editor de Apps Script, publica el `Code.gs` actualizado como Web App y confirma que conserva el `API_TOKEN` que usaba Render. No hace falta cambiar los archivos existentes del Drive.
4. Desde una terminal autenticada con `npx wrangler login`, ejecuta desde la raiz del repo:

   ```powershell
   npx wrangler secret put APPS_SCRIPT_URL
   npx wrangler secret put API_TOKEN
   npx wrangler secret put COMMUNITY_ADMIN_TOKEN
   npx wrangler deploy --config cloudflare/wrangler.toml
   ```

   Al pedir cada secreto, pega su valor. `APPS_SCRIPT_URL` es la URL `/exec` del Apps Script; `API_TOKEN` debe coincidir con `API_TOKEN` o `FEEDBACK_TOKEN` en Script Properties. El token de administrador es el que usas para moderar.
5. Configura `CORS_ORIGINS` como variable de texto con los dominios oficiales y cualquier preview que necesites. Por ejemplo: `https://stagetrack-project.vercel.app,https://stagetrack-ten.vercel.app`.
6. Wrangler mostrara la URL `https://stagetracks-community-api.<tu-subdominio>.workers.dev`. Verifica `/api/health` y `/api/sequences` antes de cambiar clientes.
7. En Vercel, cambia `STAGETRACKS_COMMUNITY_API` a esa URL y despliega Production. Para StageTracks de escritorio, cambia `apiBase` en `packaging/windows/community-config.json`, crea la siguiente version del instalador y publicala.

## Validacion

- `GET /api/health` responde `ok: true`.
- `GET /api/sequences` devuelve el indice de Apps Script/Drive existente.
- Prueba una descarga y confirma que permite `Range`/reanudar.
- Publica primero un paquete pequeno y comprueba que aparece en la comunidad.
- Luego prueba un archivo mayor a 100 MiB y observa que se divida en bloques de 80 MiB.

La migracion no elimina ni copia los archivos de Drive. Cuando la web y una version de escritorio ya apunten al Worker, Render se puede dejar suspendido sin afectar el catalogo.
## Despliegue automatico desde GitHub

Crea el namespace KV en Cloudflare una vez y agrega estos GitHub Actions secrets al repositorio: `CLOUDFLARE_API_TOKEN` (Workers Scripts: Edit y Workers KV Storage: Edit), `CLOUDFLARE_ACCOUNT_ID`, `APPS_SCRIPT_URL`, `API_TOKEN` y `COMMUNITY_ADMIN_TOKEN`. El ID del namespace no es secreto; se guarda en `cloudflare/wrangler.toml`. Cada push a `main` que modifique `cloudflare/` o `apps-script/Code.gs` desplegara el Worker.
