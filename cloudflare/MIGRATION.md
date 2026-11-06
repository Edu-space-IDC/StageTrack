# StageTracks: API de comunidad

La API de Comunidad se esta migrando de Render a Cloudflare Workers para que las descargas grandes no consuman el limite gratuito de salida de Render. Google Apps Script y Google Drive siguen siendo el origen de los metadatos y los archivos. Consulta [`cloudflare/README.md`](cloudflare/README.md) para crear el namespace KV, cargar secretos, desplegar el Worker y cambiar la URL de Vercel y de StageTracks de escritorio.

Cada publicacion admite un paquete de hasta **2 GiB**, transferido en bloques de **80 MiB** para quedar debajo del limite de 100 MB por solicitud de Cloudflare. El Worker transmite los bloques y las descargas; no almacena los paquetes ni los carga completos en memoria.

El dominio de Render actual `stagetracks-api.onrender.com` puede quedar suspendido despues de validar el cambio a Cloudflare. No lo elimines hasta comprobar catalogo, descarga reanudable y una publicacion de prueba desde web y escritorio.
