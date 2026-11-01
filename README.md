# StageTracks: publicar en Vercel y conectar la API de Comunidad

La pagina se despliega en Vercel como sitio estatico. La API de Comunidad corre en Render y usa Google Apps Script para guardar el indice, las sugerencias, los paquetes y las caratulas en una carpeta privada de Google Drive. Los archivos se comparten con enlace publico solo cuando una secuencia queda aprobada.

## Como queda conectado

- Web: esta carpeta (raiz del repositorio) -> Vercel -> `dist/`.
- API: `server/api.js` -> `https://stagetracks-api.onrender.com`.
- Datos duraderos: Google Apps Script -> carpeta `StageTracks Community` en el Drive de la cuenta que despliega el script.
- La web llama a `${STAGETRACKS_COMMUNITY_API}/api/...` y `${STAGETRACKS_COMMUNITY_API}/media/...`.
- `STAGETRACKS_COMMUNITY_API` es solo el origen, por ejemplo `https://stagetracks-api.onrender.com`. No agregues `/api` al final.

No ejecutes esta API como Vercel Function para las publicaciones: Vercel Functions limita cada cuerpo de solicitud/respuesta a 4.5 MB. Render recibe y valida el paquete; cuando se configura Apps Script, la API inicia una carga reanudable y transmite el archivo directamente a Drive. En el plan gratuito de Render el disco local es temporal, por eso los archivos finales no deben depender de ese disco. [Limites de Vercel Functions](https://vercel.com/docs/functions/limitations), [cargas reanudables de Drive](https://developers.google.com/workspace/drive/api/guides/manage-uploads)

## Preparar este proyecto para GitHub

Sube **el contenido de esta carpeta** como raiz de un repositorio; no subas la carpeta contenedora `StageTracks-routing`. Asi Vercel y Render pueden usar `.` como Root Directory. `.gitignore` excluye dependencias, compilados, datos locales y archivos `.env`; el ZIP pequeno de archivos para REAPER queda incluido. Los instaladores y ejecutables grandes deben alojarse en GitHub Releases u otro almacenamiento, y sus enlaces se configuran como variables de Vercel.

Despues de crear un repositorio vacio en GitHub, conecta y sube esta carpeta desde PowerShell:

```powershell
git remote add origin https://github.com/TU-USUARIO/TU-REPOSITORIO.git
git push -u origin main
```

Reemplaza la URL de ejemplo por la URL de tu repositorio. No agregues secretos, tokens ni archivos `.env` al repositorio.

## 1. Publicar la API

La API ya esta desplegada como Web Service gratuito de Render desde el repositorio `Edu-space-IDC/StageTrack`, rama `main`. Su URL es `https://stagetracks-api.onrender.com`; el puerto lo proporciona Render.

1. En Render abre el servicio `stagetracks-api`.
2. Confirma **Runtime** `Node`, **Build Command** `npm ci` y **Start Command** `npm start`.
3. Agrega o conserva estas variables en **Environment Variables**:

| Variable | Valor |
| --- | --- |
| `HOST` | `0.0.0.0` |
| `COMMUNITY_DATA_DIR` | `/tmp/stagetracks` para archivos temporales durante la validacion |
| `MAX_UPLOAD_MB` | `100` para la ruta multipart antigua |
| `MAX_CHUNKED_UPLOAD_MB` | `1024` para publicaciones de hasta 1 GB en bloques de 8 MB |
| `CORS_ORIGIN` | `https://stagetrack-ten.vercel.app` |
| `COMMUNITY_ADMIN_TOKEN` | Un token largo y privado para moderar publicaciones |
| `AUTO_APPROVE` | `false` o deja la variable sin definir para revisar las publicaciones antes de mostrarlas |

Render proporciona `PORT` automaticamente; no lo fijes manualmente. Sin la integracion de Drive, los datos locales de `/tmp/stagetracks` se pierden cuando Render duerme, se reinicia o se vuelve a desplegar. Render Free duerme despues de inactividad y no admite discos persistentes; la seccion siguiente configura Drive para conservar los datos. [Limites de Render Free](https://render.com/docs/free).

La API activa es `https://stagetracks-api.onrender.com`. Prueba estas rutas:

```text
https://stagetracks-api.onrender.com/api/health
https://stagetracks-api.onrender.com/api/sequences
```

La primera debe devolver JSON con `"ok": true`; la segunda, `results` y `total`. Usa la URL base de este servicio en la siguiente seccion.

**Tamano de las subidas:** la web envia paquetes de hasta 1 GB en bloques de 8 MB al servicio Node; configura `MAX_CHUNKED_UPLOAD_MB=1024` en Render. Render los guarda temporalmente en `COMMUNITY_DATA_DIR`, valida el ZIP y luego envia el archivo a Google Drive. El disco temporal debe tener espacio libre para el paquete completo y no conserva datos tras reiniciar o desplegar; Drive sigue siendo el almacenamiento permanente. La ruta multipart antigua conserva el limite `MAX_UPLOAD_MB`.

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
6. Revisa que `CORS_ORIGIN` o `CORS_ORIGINS` en el servicio API contenga cada origen de Production de Vercel, separados por coma y sin rutas ni barras finales (por ejemplo, `https://stagetrack-ten.vercel.app,https://stagetrack-project.vercel.app`). Los dos dominios de StageTracks ya estan permitidos por la API. Si agregas otro dominio, anadelo a `CORS_ORIGINS` en Render y vuelve a desplegar la API. StageTracks de escritorio usa origenes loopback en los puertos 47821-47840, tambien permitidos por la API.

Puedes configurar la API sin variables de Vercel editando `public/community-config.js` y reemplazando `window.location.origin` con la URL base HTTPS de la API; despues guarda el cambio y vuelve a desplegar. Si defines una variable de Vercel, su valor sustituye la configuracion de ese archivo durante el build.

## 3. Prueba de extremo a extremo

1. Abre el dominio Production de Vercel y entra a **Comunidad**. La consola del navegador ya no debe mostrar errores CORS o `404` en `/api/sequences`.
2. Busca secuencias. Si la lista esta vacia, la pagina debe indicar que no hay secuencias disponibles; eso no significa que la API este desconectada.
3. Publica un paquete `.rpseq` o `.zip` valido. La interfaz debe confirmar que fue recibido.
4. Como `AUTO_APPROVE` no esta activado, la publicacion queda pendiente. Para verla, apruebala desde el servidor API usando el token privado:

```powershell
$api = 'https://stagetracks-api.onrender.com'
$token = Read-Host 'Token de moderacion'
$headers = @{ Authorization = "Bearer $token" }
$pending = Invoke-RestMethod -Uri "$api/api/admin/pending" -Headers $headers
$pending.results
```

Copia el `id` de la secuencia y apruebala:

```powershell
$id = 'ID_DE_LA_SECUENCIA'
Invoke-RestMethod -Method Post -Uri "$api/api/admin/sequences/$id/approve" -Headers $headers
```

Vuelve a cargar la comunidad; ya debe aparecer y poder descargarse. Los paquetes, caratulas y el indice estan bajo `COMMUNITY_DATA_DIR`, en el disco persistente.

### Donde se guardan las secuencias

Cuando Apps Script queda configurado, la API guarda los datos en la cuenta Google que autorizó el script, dentro de la carpeta **StageTracks Community**:

- **sequences.json**: datos y estado de las secuencias.
- **feedback.json**: sugerencias anónimas.
- **files/**: paquetes .rpseq/.zip y carátulas.

Los archivos permanecen privados mientras una publicación está pendiente. Al aprobarla, el script activa **Cualquier persona con el enlace puede ver** para el paquete y la carátula, porque la comunidad necesita descargarlos. La carpeta y los archivos de moderación siguen privados. El almacenamiento está sujeto al espacio disponible en la cuenta Google.

Hasta configurar Apps Script y sus variables en Render, la API sigue usando su carpeta temporal local. Ese almacenamiento se borra cuando Render Free suspende o reinicia el servicio.

### Consultar y moderar sugerencias

El botón **Enviar sugerencia** no pide nombre ni correo. Con la integración lista, la API guarda el mensaje en feedback.json de Drive y Apps Script lo reenvía a jminaq26@gmail.com.

Para consultar sugerencias o secuencias pendientes, usa PowerShell. El token de moderación es el valor privado de COMMUNITY_ADMIN_TOKEN en Render:

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
   - API_TOKEN: secreto compartido entre Apps Script y Render.
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
7. En Render > stagetracks-api > Environment, define estos valores privados. Usa la URL /exec que ya compartiste y el mismo secreto en ambas variables de token:

| Variable | Valor |
| --- | --- |
| GOOGLE_DRIVE_APPS_SCRIPT_URL | URL del Web App de Apps Script que termina en /exec |
| GOOGLE_DRIVE_APPS_SCRIPT_TOKEN | El mismo secreto de API_TOKEN |
| FEEDBACK_APPS_SCRIPT_TOKEN | El mismo secreto de API_TOKEN |

8. Guarda los cambios y redepliega stagetracks-api. Cuando /api/health responda correctamente, secuencias, carátulas e índice se guardarán en Drive. Envía una sugerencia de prueba desde la web y confirma que llegue al correo.

Si el correo no llega, confirma que API_TOKEN coincida exactamente con ambos tokens de Render, que ejecutaste authorizeServices y que FEEDBACK_RECIPIENT sea correcto. Al cambiar el código en el futuro, crea una nueva versión desde Manage deployments.

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

- **CORS o `Failed to fetch`:** confirma que `STAGETRACKS_COMMUNITY_API` sea el origen HTTPS base y que `CORS_ORIGIN` en la API sea el dominio de la web, sin `/` final. Guarda cambios y vuelve a desplegar la web.
- **`404` en `/api/sequences`:** revisa que el servicio API este activo y que la variable no termine en `/api`; el codigo agrega esa ruta.
- **`413 Payload Too Large`:** confirma `MAX_CHUNKED_UPLOAD_MB=1024` en Render y que la web use la nueva carga por bloques. Cada bloque es de 8 MB y se envia directamente al servicio Node externo, no a una Vercel Function.
- **Las publicaciones desaparecen tras reiniciar:** configura `GOOGLE_DRIVE_APPS_SCRIPT_URL` y `GOOGLE_DRIVE_APPS_SCRIPT_TOKEN` en Render, actualiza la implementación de Apps Script y confirma que la carpeta `StageTracks Community` aparezca en Drive.
- **La publicacion se recibe pero no aparece:** con moderacion activada hay que aprobarla mediante `/api/admin/pending` y `/api/admin/sequences/:id/approve`.
- **Los botones de instalador no descargan:** define sus variables `STAGETRACKS_*_URL` con enlaces HTTPS publicos y vuelve a desplegar Vercel.

## Reaper Control Studio

`packaging/windows/ControlStudio/` contiene el proyecto original de escritorio .NET 8/WinForms/WebView2. `packaging/windows/build-release.ps1` integra allí la interfaz HTML y el icono actuales, publica el `.exe` con `dotnet publish`, crea el ZIP para REAPER con `__startup.lua`, `Reaper Control Studio.html`, `app-icon.png` y `LEEME-REAPER.txt`, y recompila StageTracks desde `web/studio.html`.

En Vercel los ejecutables se eliminan de `dist/` porque se alojan como assets externos. Configura `STAGETRACKS_REAPER_STUDIO_EXE_URL` con la URL del ejecutable recién publicado para habilitar ese botón en producción.

## Descargas publicadas: 0.5.2

- [Instalador de StageTracks](https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.2/StageTracks-Setup-windows-x64.exe)
- [StageTracks portable](https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.2/StageTracks-portable-windows-x64.zip)
- [Reaper Control Studio](https://github.com/Edu-space-IDC/StageTrack/releases/download/v0.5.2/Reaper.Control.Studio.exe)

La web de Vercel usa estos mismos enlaces en las variables `STAGETRACKS_INSTALLER_URL`, `STAGETRACKS_PORTABLE_URL` y `STAGETRACKS_REAPER_STUDIO_EXE_URL` para Production y Preview.
