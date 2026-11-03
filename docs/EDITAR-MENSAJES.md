# Editar mensajes de StageTracks

- Los avisos de la interfaz web, como confirmaciones, errores y mensajes de biblioteca, se llaman con `toast(...)` en `web/studio.html`. Busca el texto actual con Ctrl+F y cambia la cadena entre comillas.
- Los avisos que genera el motor nativo al importar o abrir archivos están en `src/PlayerBridge.cpp` y `src/AudioEngine.cpp`.
- Los mensajes del servidor local de descargas y control están en `src/WebControlServer.cpp`; los avisos de instalación de la descarga comunitaria se disparan desde `src/MainComponent.cpp`.

Guarda los archivos en UTF-8 para conservar tildes, eñes y símbolos. La biblioteca local está en `%APPDATA%\StageTracks\Multitracks`; cada canción crea una carpeta con su título y autor, archivos de audio, `sequence.json` y la carátula cuando existe. La lista de canciones continúa guardándose en `%APPDATA%\StageTracks\library.json`.
