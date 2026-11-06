# Publicar una secuencia en StageTracks Community

## El archivo requerido

Sube **un paquete `.rpseq` autocontenido** o un `.zip`. Un `.rpseq` autocontenido es un ZIP renombrado a `.rpseq` y contiene:

```text
Mi canción.rpseq       # manifiesto JSON de StageTracks
audio/
  01-guia.mp3
  02-piano.wav
cover.jpg              # opcional; el formulario también admite otra carátula
```

El archivo `.rpseq` debe incluir un objeto `songs` con al menos una canción. Cada pista usa `tracks[].items[].file` para apuntar a una ruta **relativa** como `audio/01-guia.mp3`. Los audios referenciados deben estar dentro del mismo paquete. No uses rutas absolutas, unidades como `C:\` ni rutas con `..`.

Formatos de audio admitidos: MP3, WAV, FLAC, OGG, M4A, AIFF y WMA. El paquete puede pesar hasta 2 GB en el formulario; el administrador del servidor puede cambiar ese límite.

## Campos de publicación

- Nombre de canción y artista/grupo original.
- BPM entre 40 y 240.
- Compás, por ejemplo `4/4`, `7/4` o `6/8`.
- Tonalidad, género y carátula cuadrada opcional.
- La carátula puede buscarse automáticamente por canción/artista en el catálogo de Apple Music y luego elegirse, o se puede subir una imagen JPG, PNG o WebP (hasta 8 MB).

La portada escogida identifica la publicación en la página. Para que también se abra dentro de StageTracks, incluye `cover.jpg` en el ZIP y apunta a ella con el campo `cover` del manifiesto. La app ajusta la imagen al recuadro de la lista.

## Ejemplo mínimo del manifiesto `setlist.rpseq`

```json
{
  "songs": [
    {
      "name": "Mi canción",
      "tempo": 120,
      "timeSigNum": 7,
      "timeSigDen": 4,
      "cover": "cover.jpg",
      "tracks": [
        {
          "name": "Piano",
          "volume": 0.8,
          "items": [{ "file": "audio/02-piano.wav" }]
        }
      ],
      "regions": []
    }
  ]
}
```

## Revisión y derechos

Los envíos esperan aprobación antes de mostrarse públicamente. Publica únicamente archivos que tengas derecho a compartir. No incluyas grabaciones comerciales, stems con licencia restringida ni datos personales.
