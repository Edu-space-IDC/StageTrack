# StageTracks: publicar en Vercel y conectar la API de Comunidad

La pagina se despliega en Vercel como sitio estatico. La API de Comunidad es un servicio Node/Express separado: recibe secuencias, mantiene el indice JSON y guarda paquetes y caratulas en disco persistente.

## Como queda conectado

- Web: esta carpeta (raiz del repositorio) -> Vercel -> `dist/`.
- API: `server/api.js` -> un servicio Node publico con almacenamiento persistente.
- La web llama a `${STAGETRACKS_COMMUNITY_API}/api/...` y `${STAGETRACKS_COMMUNITY_API}/media/...`.
- `STAGETRACKS_COMMUNITY_API` es solo el origen, por ejemplo `https://stagetracks-api.onrender.com`. No agregues `/api` al final.

No ejecutes esta API como Vercel Function para las publicaciones: el sitio admite paquetes de hasta 1 GB y las Vercel Functions limitan cada cuerpo de solicitud/respuesta a 4.5 MB. Ademas, este servidor guarda archivos en disco. Usa un servicio Node de larga duracion con un volumen persistente. [Limites de Vercel Functions](https://vercel.com/docs/functions/limitations)

## Preparar este proyecto para GitHub

Sube **el contenido de esta carpeta** como raiz de un repositorio; no subas la carpeta contenedora `StageTracks-routing`. Asi Vercel y Render pueden usar `.` como Root Directory. `.gitignore` excluye dependencias, compilados, datos locales y archivos `.env`; el ZIP pequeno de archivos para REAPER queda incluido. Los instaladores y ejecutables grandes deben alojarse en GitHub Releases u otro almacenamiento, y sus enlaces se configuran como variables de Vercel.

Despues de crear un repositorio vacio en GitHub, conecta y sube esta carpeta desde PowerShell:

```powershell
git remote add origin https://github.com/TU-USUARIO/TU-REPOSITORIO.git
git push -u origin main
```

Reemplaza la URL de ejemplo por la URL de tu repositorio. No agregues secretos, tokens ni archivos `.env` al repositorio.

## 1. Publicar la API

Este ejemplo usa un Web Service de Render. Puedes usar otro proveedor si admite Node.js 20.19 o posterior, solicitudes del tamano que configures y un disco persistente.

1. Sube el repositorio a GitHub y conecta ese repositorio en Render: **New > Web Service**.
2. Si conectas el repositorio de esta carpeta, deja **Root Directory** en `.`.
3. Configura **Runtime** `Node`, **Build Command** `npm ci` y **Start Command** `npm start`.
4. En **Advanced**, agrega un disco persistente con punto de montaje `/var/data`. Elige capacidad para el indice, las caratulas y todos los paquetes que esperas recibir.
5. Configura la comprobacion de salud en `/api/health`.
6. Agrega estas variables en **Environment Variables** del servicio:

| Variable | Valor |
| --- | --- |
| `HOST` | `0.0.0.0` |
| `COMMUNITY_DATA_DIR` | `/var/data/stagetracks` |
| `MAX_UPLOAD_MB` | `1024` para permitir hasta 1 GB por paquete |
| `CORS_ORIGIN` | El origen publico de la web, por ejemplo `https://tu-proyecto.vercel.app` |
| `COMMUNITY_ADMIN_TOKEN` | Un token largo y privado para moderar publicaciones |
| `AUTO_APPROVE` | `false` o deja la variable sin definir para revisar las publicaciones antes de mostrarlas |

Render proporciona `PORT` automaticamente y el servidor lo utiliza. No fijes un puerto manual. El disco debe estar montado en `/var/data`; la API crea dentro `stagetracks/packages`, `stagetracks/covers` y `stagetracks/sequences.json`. Los discos persistentes de Render requieren un servicio de pago; sin disco, los cambios locales se pierden al reiniciar o desplegar. [Desplegar Node/Express en Render](https://render.com/docs/deploy-node-express-app), [Web Services](https://render.com/docs/web-services), [Persistent Disks](https://render.com/docs/disks).

Espera a que el servicio este activo y copia su URL publica HTTPS, por ejemplo `https://stagetracks-api.onrender.com`. Prueba estas rutas:

```text
https://stagetracks-api.onrender.com/api/health
https://stagetracks-api.onrender.com/api/sequences
```

La primera debe devolver JSON con `"ok": true`; la segunda, `results` y `total`. Usa la URL base de este servicio en la siguiente seccion.

**Tamano de las subidas:** `MAX_UPLOAD_MB` es el limite de la aplicacion. El proveedor tambien debe permitir solicitudes de ese tamano y duracion. Si tu proveedor limita las solicitudes, reduce `MAX_UPLOAD_MB` a ese limite o cambia la API para subir directamente a almacenamiento de objetos. No apuntes las subidas grandes a una Vercel Function.

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
6. Copia el dominio de Production que Vercel asigno al sitio y revisa que `CORS_ORIGIN` en el servicio API coincida exactamente con ese origen: `https://tu-proyecto.vercel.app`, sin ruta ni barra final. Si usas un dominio propio, configura ese dominio canonico en `CORS_ORIGIN` y en las variables de Vercel.

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

Las subidas no se guardan en Vercel ni en GitHub. La API las guarda en el directorio definido por `COMMUNITY_DATA_DIR`. En la configuracion recomendada de Render, el disco persistente esta montado en `/var/data` y el directorio de datos es `/var/data/stagetracks`:

- Indice y estado de cada secuencia: `/var/data/stagetracks/sequences.json`.
- Paquetes `.rpseq` y `.zip`: `/var/data/stagetracks/packages/`.
- Portadas: `/var/data/stagetracks/covers/`.
- Sugerencias: `/var/data/stagetracks/feedback.json`.

Si usas otro proveedor, revisa el valor de `COMMUNITY_DATA_DIR` y confirma que apunta a un volumen persistente. Sin ese disco, los archivos locales pueden borrarse al reiniciar o volver a desplegar la API.

En una ejecucion local sin `COMMUNITY_DATA_DIR`, la misma estructura queda dentro de `server/data/`.

### Leer las sugerencias anónimas

El botón flotante **Enviar sugerencia** guarda los mensajes en `feedback.json`, dentro de `COMMUNITY_DATA_DIR`. El formulario no pregunta nombre ni correo y la API no guarda la IP ni otros datos del visitante. Para consultar los mensajes, usa la terminal de PowerShell en tu computadora; no pongas el token en la web ni lo compartas:

```powershell
$api = 'https://TU-API.onrender.com'
$token = Read-Host 'Token privado de COMMUNITY_ADMIN_TOKEN'
$headers = @{ Authorization = "Bearer $token" }
$feedback = Invoke-RestMethod -Uri "$api/api/admin/feedback" -Headers $headers
$feedback.results | Format-List id, createdAt, message
```

Reemplaza `https://TU-API.onrender.com` por la URL real de tu API. El token es el mismo `COMMUNITY_ADMIN_TOKEN` privado configurado en Render. Para borrar una sugerencia después de revisarla:

```powershell
$id = 'ID_DE_LA_SUGERENCIA'
Invoke-RestMethod -Method Delete -Uri "$api/api/admin/feedback/$id" -Headers $headers
```

Las sugerencias se conservan entre despliegues si `COMMUNITY_DATA_DIR` está dentro del disco persistente de Render, como en `/var/data/stagetracks`. Si el disco no está montado, se perderán al reiniciar el servicio.

### Enviar cada sugerencia a tu correo con Google Apps Script

La API ya tiene configurada la URL de Apps Script que compartiste. Apps Script manda el correo a `jminaq26@gmail.com`; tambien se conserva una copia en `feedback.json`.

1. Abre [script.google.com](https://script.google.com) con la cuenta Gmail donde quieres recibir las sugerencias y crea un proyecto.
2. Reemplaza el contenido del editor por este codigo:

```javascript
const RECIPIENT = 'jminaq26@gmail.com';

function doPost(e) {
  try {
    const data = JSON.parse(e.postData.contents || '{}');
    const expected = PropertiesService.getScriptProperties().getProperty('FEEDBACK_TOKEN');
    if (!expected || data.token !== expected) return output({ ok: false, error: 'Unauthorized' });
    const message = String(data.message || '').trim();
    if (message.length < 10 || message.length > 2000) return output({ ok: false, error: 'Invalid message' });
    const date = String(data.createdAt || new Date().toISOString());
    MailApp.sendEmail({
      to: RECIPIENT,
      subject: 'Nueva sugerencia para StageTracks',
      body: 'Recibiste una sugerencia anonima para StageTracks.\n\n' + message + '\n\nFecha: ' + date + '\nID: ' + String(data.id || '')
    });
    return output({ ok: true });
  } catch (error) {
    return output({ ok: false, error: String(error) });
  }
}

function output(value) {
  return ContentService.createTextOutput(JSON.stringify(value))
    .setMimeType(ContentService.MimeType.JSON);
}

// Run this function once in the editor to authorize sending email.
function authorizeMail() {
  MailApp.getRemainingDailyQuota();
}
```

3. En **Project Settings > Script Properties**, crea `FEEDBACK_TOKEN`. Pon como valor un secreto largo aleatorio (por ejemplo, 32 o mas caracteres). Usa exactamente ese mismo valor en Render, pero no lo incluyas en la pagina ni lo subas a GitHub.
4. Ejecuta `authorizeMail` una vez desde el editor y acepta el permiso para enviar correo.
5. Pulsa **Deploy > New deployment > Web app**. Elige **Execute as: Me** y **Who has access: Anyone**. El codigo valida el token compartido antes de enviar. Pulsa **Deploy** y copia la URL que termina en `/exec`.
6. En Render, abre el servicio de la API y agrega estas variables privadas:

| Variable | Valor |
| --- | --- |
| `FEEDBACK_APPS_SCRIPT_TOKEN` | El mismo secreto configurado como `FEEDBACK_TOKEN` en Apps Script |

La URL ya esta definida como destino predeterminado en `server/api.js`; si en el futuro cambia, puedes sobrescribirla con la variable opcional `FEEDBACK_APPS_SCRIPT_URL`.

7. Guarda el token y redepliega el servicio API de Render. Envia una sugerencia de prueba desde la web y confirma que llegue al correo. Si el correo falla, la API conserva el mensaje y la interfaz avisara que no se pudo enviar.

Al editar el codigo de Apps Script mas adelante, crea una nueva version desde **Deploy > Manage deployments > Edit > New version**. Si generas otro token, actualizalo en ambos lugares.

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
- **`413 Payload Too Large`:** redujiste `MAX_UPLOAD_MB` o el proveedor tiene un limite de solicitud inferior. El limite de Vercel Functions es 4.5 MB, por eso las publicaciones van directamente al servicio Node externo.
- **Las publicaciones desaparecen tras reiniciar:** el disco no esta conectado, o `COMMUNITY_DATA_DIR` no apunta dentro del punto de montaje persistente.
- **La publicacion se recibe pero no aparece:** con moderacion activada hay que aprobarla mediante `/api/admin/pending` y `/api/admin/sequences/:id/approve`.
- **Los botones de instalador no descargan:** define sus variables `STAGETRACKS_*_URL` con enlaces HTTPS publicos y vuelve a desplegar Vercel.

## Reaper Control Studio

`packaging/windows/ControlStudio/` contiene el proyecto original de escritorio .NET 8/WinForms/WebView2. `packaging/windows/build-release.ps1` integra allí la interfaz HTML y el icono actuales, publica el `.exe` con `dotnet publish`, crea el ZIP para REAPER con `__startup.lua`, `Reaper Control Studio.html`, `app-icon.png` y `LEEME-REAPER.txt`, y recompila StageTracks desde `web/studio.html`.

En Vercel los ejecutables se eliminan de `dist/` porque se alojan como assets externos. Configura `STAGETRACKS_REAPER_STUDIO_EXE_URL` con la URL del ejecutable recién publicado para habilitar ese botón en producción.
