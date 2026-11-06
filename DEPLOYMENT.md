# StageTracks: publicar en Vercel y conectar la API de Comunidad

La pagina se despliega en Vercel como sitio estatico. La API de Comunidad funciona en Cloudflare Workers y usa Google Apps Script para el indice y Google Drive para paquetes y caratulas. StageTracks 0.5.25 usa el Worker como destino de respaldo cuando no hay `community-config.json` junto al ejecutable.

## Como queda conectado

- Web: esta carpeta (raiz del repositorio) -> Vercel -> `dist/`.
- API en migracion: `cloudflare/worker.js` -> URL `workers.dev` que asigna Cloudflare. El servidor anterior esta en `server/api.js`.
- Datos duraderos: Google Apps Script -> carpeta `StageTracks Community` en el Drive de la cuenta que despliega el script.
- La web llama a `${STAGETRACKS_COMMUNITY_API}/api/...` y `${STAGETRACKS_COMMUNITY_API}/media/...`.
- `STAGETRACKS_COMMUNITY_API` es solo el origen; al terminar la migracion sera la URL HTTPS `workers.dev`. No agregues `/api` al final.

No ejecutes la API como Vercel Function: Vercel limita cada solicitud a 4.5 MB. Cloudflare recibe bloques de 80 MiB y transmite los archivos directamente desde/hacia Drive sin pasar por Render. [Limites de Vercel Functions](https://vercel.com/docs/functions/limitations), [cargas reanudables de Drive](https://developers.google.com/workspace/drive/api/guides/manage-uploads).

## Preparar este proyecto para GitHub

Sube **el contenido de esta carpeta** como raiz de un repositorio; no subas la carpeta contenedora `StageTracks-routing`. Asi Vercel y los servicios de API pueden usar `.` como Root Directory. `.gitignore` excluye dependencias, compilados, datos locales y archivos `.env`; el ZIP pequeno de archivos para REAPER queda incluido. Los instaladores y ejecutables grandes deben alojarse en GitHub Releases u otro almacenamiento, y sus enlaces se configuran como variables de Vercel.

Despues de crear un repositorio vacio en GitHub, conecta y sube esta carpeta desde PowerShell:

```powershell
git remote add origin https://github.com/TU-USUARIO/TU-REPOSITORIO.git
git push -u origin main
```

Reemplaza la URL de ejemplo por la URL de tu repositorio. No agregues secretos, tokens ni archivos `.env` al repositorio.

## 1. Desplegar la API gratuita en Cloudflare

La configuracion y el workflow automatico estan en [`cloudflare/README.md`](cloudflare/README.md). La API usa Apps Script para el indice y Google Drive para los paquetes y caratulas. Cloudflare transmite archivos para que Render no consuma salida de red. El Worker recibe bloques de 80 MiB (menos del limite de 100 MB) y admite paquetes de hasta 2 GiB.

1. Crea el namespace KV `COMMUNITY_KV` y guarda su ID.
2. Configura los secretos de Cloudflare y GitHub Actions indicados en `cloudflare/README.md`.
3. Despliega el Worker y comprueba `/api/health` y `/api/sequences` antes de cambiar la web o StageTracks de escritorio.
4. Configura Vercel y `packaging/windows/community-config.json` con el dominio `workers.dev` que asigne Cloudflare.

No retires Render hasta probar catalogo, publicacion por bloques y descarga reanudable desde web y escritorio.
## 2. Configurar el proyecto Vercel

1. En Vercel elige **Add New > Project** e importa el repositorio desde GitHub.
2. En **Root Directory**, deja `.` (esta carpeta sera la raiz del repositorio).
3. Usa estos ajustes de build (tambien estan en `vercel.json`):

| Ajuste | Valor |
| --- | --- |
| Framework Preset | `Vite` |
| Install Command | `npm ci` |
| Build Command | `npm run vercel-build` |
| Output Directory | `dist` |

4. En **Settings > Environment Variables**, agrega las variables necesarias. Selecciona **Production**; agrega tambien **Preview** si vas a probar previews y ya configuraste su CORS en la API.

| Variable de Vercel | Valor |
| --- | --- |
| `STAGETRACKS_COMMUNITY_API` | URL base HTTPS de la API, sin `/api` al final |
| `STAGETRACKS_INSTALLER_URL` | URL HTTPS externa del instalador de StageTracks |
| `STAGETRACKS_PORTABLE_URL` | URL HTTPS externa del ZIP portable |
| `STAGETRACKS_REAPER_STUDIO_EXE_URL` | URL HTTPS externa de Reaper Control Studio.exe |

Las tres variables de descarga son opcionales para la Comunidad, pero necesarias para que funcionen esos botones de descarga. Aloja los binarios grandes en GitHub Releases u otro almacenamiento de archivos, no dentro de la carpeta del sitio; `release-artifacts/` esta excluida del repositorio. El ZIP pequeno `Reaper-Control-Studio-Reaper-files.zip` ya se sirve desde `public/downloads/`.

`vite.config.js` inserta estas variables durante el build en `dist/community-config.js`. Son valores publicos que llegan al navegador: **no pongas** `COMMUNITY_ADMIN_TOKEN` ni otros secretos en Vercel para la web. El token de moderacion va unicamente en las variables privadas del servicio API.

5. Pulsa **Deploy**. Despues de guardar o modificar variables, crea un nuevo deployment de Production para que Vite regenere la configuracion publica.
6. Revisa que `CORS_ORIGIN` o `CORS_ORIGINS` en el servicio API contenga cada origen de Production de Vercel, separados por coma y sin rutas ni barras finales (por ejemplo, `https://stagetrack-ten.vercel.app,https://stagetrack-project.vercel.app`). Los dos dominios de StageTracks ya estan permitidos por la API. Si agregas otro dominio, configuralo en `CORS_ORIGINS` del Worker de Cloudflare. StageTracks de escritorio usa origenes loopback en los puertos 47821-47840, tambien permitidos por la API.

Puedes configurar la API sin variables de Vercel editando `public/community-config.js` y reemplazando `window.location.origin` con la URL base HTTPS de la API; despues guarda el cambio y vuelve a desplegar. Si defines una variable de Vercel, su valor sustituye la configuracion de ese archivo durante el build.

## 3. Prueba de extremo a extremo

1. Abre el dominio Production de Vercel y entra a **Comunidad**. La consola del navegador ya no debe mostrar errores CORS o `404` en `/api/sequences`.
2. Busca secuencias. Si la lista esta vacia, la pagina debe indicar que no hay secuencias disponibles; eso no significa que la API este desconectada.
3. Publica un paquete `.rpseq` o `.zip` valido. La interfaz debe confirmar que fue recibido.
4. La secuencia debe aparecer en la comunidad al terminar la carga. La API también publica al iniciar las secuencias antiguas que quedaron pendientes.

Vuelve a cargar la comunidad; ya debe aparecer y poder descargarse.

### Donde se guardan las secuencias

Cuando Apps Script queda configurado, la API guarda los datos en la cuenta Google que autorizó el script, dentro de la carpeta **StageTracks Community**:

- **sequences.json**: datos y estado de las secuencias.
- **feedback.json**: sugerencias anónimas.
- **files/**: paquetes .rpseq/.zip y carátulas.

Los archivos permanecen privados mientras una publicación está pendiente. Al aprobarla, el script activa **Cualquier persona con el enlace puede ver** para el paquete y la carátula, porque la comunidad necesita descargarlos. La carpeta y los archivos de moderación siguen privados. El almacenamiento está sujeto al espacio disponible en la cuenta Google.

Durante la migracion, las versiones existentes siguen dependiendo de Render. Despues de apuntar la web y el escritorio al Worker, los metadatos seguiran en Apps Script/Drive y los paquetes en Drive.

### Consultar y moderar sugerencias

El botón **Enviar sugerencia** no pide nombre ni correo. Con la integración lista, la API guarda el mensaje en feedback.json de Drive y Apps Script lo reenvía a jminaq26@gmail.com.

Para consultar sugerencias o secuencias pendientes, usa PowerShell. El token de moderación es el valor privado de COMMUNITY_ADMIN_TOKEN en los secretos del Worker:

```powershell
$api = 'https://stagetracks-api.onrender.com'
$token = Read-Host 'Token privado de COMMUNITY_ADMIN_TOKEN'
$headers = @{ Authorization = "Bearer $token" }
$feedback = Invoke-RestMethod -Uri "$api/api/admin/feedback" -Headers $headers
$feedback.results | Format-List id, createdAt, message
$pending = Invoke-RestMethod -Uri "$api/api/admin/pending" -Headers $headers
$pending.results
```

Para borrar una sugerencia después de revisarla:

```powershell
$id = 'ID_DE_LA_SUGERENCIA'
Invoke-RestMethod -Method Delete -Uri "$api/api/admin/feedback/$id" -Headers $headers
```

### Conectar Google Drive y activar el correo de feedback

El código completo de Apps Script está en apps-script/Code.gs; sus permisos están en apps-script/appsscript.json. No pegues tokens en el sitio ni los subas a GitHub.

1. Abre el proyecto de Apps Script asociado a la URL que ya compartiste. Reemplaza el contenido de Code.gs por apps-script/Code.gs.
2. En Project Settings, activa Show "appsscript.json" manifest file in editor y reemplaza ese archivo por apps-script/appsscript.json.
3. En el editor, ejecuta authorizeServices una vez y acepta los permisos para Drive, solicitudes HTTPS y correo. El script crea StageTracks Community y files/ al recibir la primera llamada.
4. En Project Settings > Script Properties, agrega:
   - API_TOKEN: secreto compartido entre Apps Script y Cloudflare Workers.
   - FEEDBACK_RECIPIENT: jminaq26@gmail.com.
5. Para generar un secreto en PowerShell, ejecuta este bloque y conserva el resultado para pegarlo en los dos servicios:

```powershell
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$bytes = New-Object byte[] 32
$rng.GetBytes($bytes)
$token = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
$token
```

6. En Apps Script, abre Deploy > Manage deployments > Edit en la implementación actual y selecciona New version. Déjala como Web app, Execute as: Me y Who has access: Anyone. Actualizar la implementación conserva la URL /exec que ya usa el proyecto.
7. **Para aceptar paquetes de hasta 2 GiB**, publica una nueva version despues de copiar el `Code.gs` actualizado. Apps Script inicia la carga resumible y Cloudflare transmite bloques de 80 MiB directamente a Drive.
8. En GitHub Actions configura `APPS_SCRIPT_URL` con la URL `/exec` y `API_TOKEN` con el mismo valor de Script Properties. El workflow de Cloudflare utiliza esos secretos al desplegar el Worker.

Si el correo no llega, confirma que `API_TOKEN` coincida con Script Properties, que ejecutaste `authorizeServices` y que `FEEDBACK_RECIPIENT` sea correcto. Al cambiar el codigo, crea una nueva version en Manage deployments y vuelve a desplegar el Worker.

## 4. Desarrollo y prueba local

Abre dos terminales en `Pagina oficial/site`.

Terminal 1, inicia la API local en el puerto `47822`:

```powershell
npm run api
```

Terminal 2, inicia Vite:

```powershell
npm run dev
```

Antes de iniciar Vite, ejecuta `packaging/windows/build-release.ps1` desde la raiz del proyecto para compilar StageTracks, Reaper Control Studio y el ZIP HTML+Lua. En desarrollo, `vite.config.js` sirve los artefactos de `release-artifacts/` en `/downloads/` sin duplicar los ejecutables grandes en `public/`. Abre `http://127.0.0.1:5173/`; Vite envia `/api` y `/media` a la API local. Para compilar y revisar la salida de produccion, ejecuta `npm run build` y luego `npm run preview`.

## 5. Errores comunes

- **CORS o `Failed to fetch`:** confirma que `STAGETRACKS_COMMUNITY_API` sea el origen HTTPS `workers.dev` y que el dominio de la web est? permitido por `CORS_ORIGINS` del Worker.
- **`404` en `/api/sequences`:** revisa que el servicio API este activo y que la variable no termine en `/api`; el codigo agrega esa ruta.
- **`413 Payload Too Large`:** confirma que la web use bloques de 80 MiB y que Cloudflare tenga el namespace KV conectado al Worker. El paquete completo puede pesar hasta 2 GiB.
- **Las publicaciones desaparecen tras reiniciar:** configura `APPS_SCRIPT_URL` y `API_TOKEN` en GitHub Actions/Cloudflare, actualiza la implementación de Apps Script y confirma que la carpeta `StageTracks Community` aparezca en Drive.
- **La publicacion se recibe pero no aparece:** revisa `/api/sequences` en el Worker de Cloudflare. Las publicaciones nuevas son inmediatas y la API recupera del estado de Drive las secuencias antiguas pendientes al iniciar.
- **Los botones de instalador no descargan:** define sus variables `STAGETRACKS_*_URL` con enlaces HTTPS publicos y vuelve a desplegar Vercel.

## Reaper Control Studio

`packaging/windows/ControlStudio/` contiene el proyecto original de escritorio .NET 8/WinForms/WebView2. `packaging/windows/build-release.ps1` integra allí la interfaz HTML y el icono actuales, publica el `.exe` con `dotnet publish`, crea el ZIP para REAPER con `__startup.lua`, `Reaper Control Studio.html`, `app-icon.png` y `LEEME-REAPER.txt`, y recompila StageTracks desde `web/studio.html`.

En Vercel los ejecutables se eliminan de `dist/` porque se alojan como assets externos. Configura `STAGETRACKS_REAPER_STUDIO_EXE_URL` con la URL del ejecutable recién publicado para habilitar ese botón en producción.

## Descargas publicadas: 0.5.2

- [Instalador de StageTracks](https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.2/StageTracks-Setup-windows-x64.exe)
- [StageTracks portable](https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.2/StageTracks-portable-windows-x64.zip)
- [Reaper Control Studio](https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.2/Reaper.Control.Studio.exe)

La web de Vercel usa estos mismos enlaces en las variables `STAGETRACKS_INSTALLER_URL`, `STAGETRACKS_PORTABLE_URL` y `STAGETRACKS_REAPER_STUDIO_EXE_URL` para Production y Preview.
