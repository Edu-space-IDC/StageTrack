Icono unico de StageTracks y Reaper Control Studio

Reemplaza este archivo fuente:
  assets/app-icon.png

Luego ejecuta desde la raiz del proyecto:
  powershell -ExecutionPolicy Bypass -File packaging\windows\build-release.ps1

El empaquetado genera assets/app-icon.ico y actualiza el icono de la app/instalador, el favicon y logo del sitio oficial, y agrega app-icon.png junto al HTML de Reaper Control Studio para que la pestana del navegador y el logo lo encuentren.
