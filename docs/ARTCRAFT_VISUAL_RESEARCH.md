# Referencia visual: ArtCraft

**Fecha de consulta:** 12 de noviembre de 2026  
**Sitio:** [getartcraft.com](https://getartcraft.com/)  
**Objetivo:** documentar patrones visuales y de movimiento que puedan inspirar una futura web de StageTracks, manteniendo la marca, el azul y el contenido propios de StageTracks.

## Alcance y nivel de evidencia

La extensión de navegador mencionada no está conectada a esta sesión y no hay un ejecutable `rea` disponible en el entorno. Por eso esta revisión no controló la página en vivo ni grabó una sesión de interacción. Se inspeccionaron la página pública renderizada, el HTML que entrega el servidor, la hoja CSS compilada y los chunks JavaScript públicos.

Las observaciones de HTML/CSS/JS describen lo que el sitio publica. La apariencia exacta de cada frame, el comportamiento en dispositivos reales, los tiempos percibidos en una conexión lenta y las variaciones por sesión requieren una inspección de navegador en vivo; quedan marcados como no verificados.

## Dirección visual observada

- **Estética editorial y técnica:** espacio negativo amplio, titulares compactos, reglas finas, etiquetas pequeñas monoespaciadas y numeración por sección. La página se siente más como un catálogo editorial de herramientas que como una landing de plantillas.
- **Paleta:** base clara cálida (`#f2f1ee`), superficies casi blancas, tinta oscura y líneas de baja opacidad. El azul vivo (`#2d81ff`) es el acento del producto; la paleta cambia a fondo casi negro en el modo oscuro y conserva el azul.
- **Tipografía:** Archivo para el wordmark y titulares, Instrument Serif como fuente de contraste editorial, más Inter y Geist Mono para texto de interfaz y microetiquetas.
- **Retícula:** contenido con ancho máximo cercano a 1280 px, bordes laterales y separadores de un píxel. El catálogo pasa de una columna en móvil a dos en tamaños intermedios y cuatro en escritorio amplio.
- **Navegación:** barra pegada arriba, de poca altura, con línea inferior y fondo translúcido con blur. El menú de escritorio se vuelve menú compacto en pantallas pequeñas.
- **Tarjetas:** cuadrícula separada por líneas, fondos planos, indicadores numerados y acentos propios por producto. El hover usa una línea superior que se despliega y pequeños movimientos en iconos y flechas, no una elevación fuerte con sombras grandes.

La página organiza el inicio en hero, catálogo de aplicaciones, lineup, principios, promoción de ArtCraft Studio, llamada a colaborar y pie de página. La revisión de la página pública muestra estas secciones y su función general. ([Página oficial de ArtCraft](https://getartcraft.com/))

## Animaciones y microinteracciones encontradas

### 1. Intro del hero

El HTML público divide el wordmark en letras y mantiene una pila central para título, descripción, acciones y señales de confianza. El código público expone una secuencia de introducción por etapas: el símbolo/logotipo, elementos gráficos, navegación y copy no entran todos a la vez.

Los parámetros publicados para esa secuencia indican, como valores por defecto, que el logo empieza primero, los elementos gráficos empiezan alrededor de 1 segundo, la navegación se incorpora cerca de 0,7 segundos y el bloque de copy después de aproximadamente 1,05 segundos. El ensamblaje del wordmark dura alrededor de 0,85 segundos. El código describe adelantar la secuencia cuando el visitante desplaza, hace clic o pulsa una tecla, en vez de cortarla de golpe; también contempla acelerar la visita repetida.

La escena gráfica del hero parece incluir elementos tipo tarjetas que salen del centro en brazos y una capa de blur/glow detrás del wordmark. El código menciona tarjetas tipo galaxia y contiene módulos para efectos gráficos. **No se confirma su render final en pantalla**, porque no se pudo inspeccionar la sesión interactiva con el navegador.

### 2. Reveal al entrar en viewport

Los bloques marcados para revelarse arrancan con opacidad baja, desplazamiento vertical de unos 28 px y blur de 8 px; al entrar en viewport pasan a opacidad completa, posición normal y blur cero. El efecto usa una duración aproximada de 0,9 s y una curva de salida suave. Los elementos normales se activan cuando su borde superior se acerca al 85 % del alto visible; cada elemento se anima una sola vez.

En grupos, el tiempo se distribuye según la posición horizontal de cada hijo para dar una cascada espacial. En el hero hay un pequeño stagger, aproximadamente 80 ms por elemento. Esta variación da sensación de orden sin convertir cada tarjeta en una animación independiente llamativa.

### 3. Entrada coreografiada de secciones

Las secciones con regla y marcas de esquina dibujan su línea al entrar en pantalla. La regla escala desde el origen durante unos 0,7 s; la etiqueta numerada aparece con fade y desplazamiento, y las marcas pequeñas entran en cascada con un stagger de unos 60 ms. La activación empieza cerca del 88 % del viewport y ocurre una sola vez.

Las bandas de etiqueta son sticky y translúcidas. Mientras el contenido pasa debajo, el rótulo numerado permanece visible y da continuidad entre bloques sin ocupar otra cabecera alta.

### 4. Hover de tarjetas y enlaces

- Una regla de acento en la parte superior de cada tarjeta pasa de escala cero a completa en aproximadamente 300 ms.
- Los iconos suben unos pocos píxeles (cerca de 4 px) en 300 ms.
- Las flechas se mueven ligeramente en diagonal y cambian de tono.
- Los enlaces usan cambios de opacidad/color más que grandes transformaciones.
- Los colores de producto pueden cambiar por tarjeta; en StageTracks conviene usar el azul de marca de forma consistente y dejar que el contenido diferencie las tarjetas.

### 5. Accesibilidad de movimiento

La hoja de estilos reduce animaciones y transiciones casi a cero con `prefers-reduced-motion: reduce`; el código de las coreografías también se ejecuta en el modo de movimiento normal. Este patrón debe conservarse si StageTracks adopta una intro más ambiciosa.

## Plano de adaptación para StageTracks

Esta es una traducción de patrones, no una reproducción de ArtCraft. Mantener los textos, las secciones y los enlaces que ya tiene StageTracks.

### Sistema visual

Conservar los tokens actuales del sitio:

| Token | Valor recomendado |
| --- | --- |
| `--color-bg` | `#f2f6fb` claro / `#0e131b` oscuro |
| `--color-surface` | `#ffffff` claro / `#161d29` oscuro |
| `--color-text` | `#0e1622` claro / `#e9eef5` oscuro |
| `--color-muted` | `#56627a` claro / `#93a0b4` oscuro |
| `--color-accent` | `#1d6fb8` claro / `#3da9fc` oscuro |
| `--color-border` | `#d3dce8` claro / `#27313f` oscuro |

Usar líneas de borde sutiles y superficies simples. Las secciones pueden tener un rótulo `01 / Características`, `02 / Pads`, etc., pero deben seguir siendo las secciones existentes. Mantener el azul como el único acento de marca; los brillos y fondos radiales deben ser transparencias de ese azul.

### Guion de movimiento sugerido

1. **Carga inicial:** marca y wordmark aparecen primero; título, texto, búsqueda y botones entran en cascada. Duración total aproximada: 0,8–1,2 s. El resto del sitio no debe quedar oculto si JavaScript no carga.
2. **Captura del reproductor:** entrar con fade, desplazamiento corto y blur moderado; mantener el tilt/parallax muy leve y solo con puntero fino.
3. **Secciones:** al acercarse al viewport, revelar encabezado y contenido una sola vez. Usar 0,6–0,8 s para que el scroll no se sienta lento.
4. **Tarjetas:** cascada corta (50–80 ms entre elementos) para las tarjetas de características y secuencias. Evitar retrasar tanto que el usuario espere el contenido.
5. **Rótulo de sección:** usar una línea superior que crece y una etiqueta numerada sticky/translúcida en bandas largas, en lugar de añadir nuevas secciones o grandes bloques decorativos.
6. **Hover:** línea azul corta que recorre el borde superior, cubierta que escala como máximo 1.02–1.03, flecha que avanza unos píxeles, elevación máxima de 2–4 px.
7. **Navegación:** barra sticky translúcida que gana blur y borde al desplazarse; conservar el menú hamburguesa existente en móvil.
8. **Movimiento reducido:** desactivar parallax, shimmer y cascadas; dejar el contenido visible y usar transiciones instantáneas o muy breves.

### Implementación adecuada al proyecto actual

La web de StageTracks es Vite con HTML, CSS y JavaScript vanilla. Para recrear el carácter sin añadir una dependencia pesada:

- Usar `IntersectionObserver` para aplicar `.is-visible` o `.on` a los bloques que entran en viewport.
- Definir tokens de duración y distancia en CSS; animar principalmente `opacity` y `transform`.
- Usar `requestAnimationFrame` solo para gestos vinculados al puntero, con un guardado del último evento por frame.
- Usar `matchMedia('(prefers-reduced-motion: reduce)')` en CSS y JS.
- Mantener los números de sección como decoración accesible: ocultarlos al lector de pantalla si repiten información y evitar que los pseudo-elementos tapen el encabezado.
- Reservar las animaciones más complejas para el hero y la captura del programa. El resto del sitio debe aparecer con transiciones cortas y consistentes.

## Lo que no conviene copiar

- El wordmark, nombres de productos, textos, composición exacta del catálogo, iconografía, ilustraciones o activos de ArtCraft.
- Su beige/neón o acentos de producto como identidad principal: StageTracks conserva su azul y los colores existentes.
- La escena tipo galaxia si exige WebGL continuo o reduce el rendimiento en equipos modestos.
- Animar todos los elementos en cada scroll; la referencia usa activaciones de una sola vez para gran parte de sus reveals.

## Pendiente para confirmar en navegador

- Capturar el hero al cargar, después del primer uso y en una visita repetida.
- Desplazarse por cada sección y medir el momento/duración visual de los reveals.
- Comparar hover, teclado, móvil y `prefers-reduced-motion`.
- Medir si las escenas gráficas del hero se pausan fuera de viewport y cuando la pestaña queda oculta.

La extensión o el MCP de REA tendrán que estar disponibles en una sesión posterior para completar esa verificación de runtime. La base de diseño anterior sí puede guiar ya el siguiente rediseño de StageTracks.
