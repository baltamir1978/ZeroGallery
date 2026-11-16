# ZeroGallery

Galería de fotos y vídeos para el **M5Stack Cardputer Zero** (Raspberry Pi CM0: 4 núcleos
Cortex-A53 a 1 GHz, 512 MB, pantalla de 320×170, Debian 13 arm64). En el lanzador se llama
**Gallery**. Qué hace y cómo se usa está en `README.md`; aquí va lo que hace falta para tocarla.

No es una app de Android: de `~/CLAUDE.md` valen el estilo (comentarios, documentación y
commits en español explicando el porqué; pocas dependencias; lógica pura con pruebas), que el
repositorio es **público** (ni secretos ni datos personales) y la entrega; lo de Gradle, firma
y GrapheneOS no aplica.

## Compilar y comprobar

En esta máquina no hay CMake ni compilador cruzado ni sudo: todo va en Docker.

```sh
Tools/compilar.sh pruebas   # simulador (SDL2) + ctest; tiene que pasar
Tools/compilar.sh deb       # dist/ZeroGallery_<versión>_rev1_arm64.deb
```

- La primera vez descarga LVGL 9.5 y el *sysroot* de M5Stack a `.cache/` (no se versiona).
- GitHub Actions hace lo mismo en cada push (`.github/workflows/build.yml`).
- **Probar sin pantalla**: el simulador corre bajo `Xvfb` dentro de la misma imagen
  (`SDL_RENDER_DRIVER=software`), se le mandan teclas con `xdotool key` y se captura con
  `import -window root`; la pantalla del aparato queda en `324x174+538+266` de esa captura.
  La app también guarda capturas exactas de 320×170 con `Print` en `~/Pictures/Screenshots`
  (de ahí salen las de `screenshot/`). En Docker no hay tarjeta de sonido, así que el audio
  no se puede probar aquí.
- Aparato aquí no hay: lo que no se ha probado en él se dice («sin probar en el aparato») en
  el «Estado» y se quita cuando Bruno lo confirma.

## De dónde viene

Es la plantilla oficial `CardputerZero/Template` (commit `477fa6d`), copiada y renombrada a
mano como hace `czdev new` (`template_app` → `zerogallery`, iconos `template*.png` →
`zerogallery*.png`). De ella se conservan tal cual el CMake, el simulador de escritorio
(`src/app/desktop_*`), el registro, los *bindings* reactivos, la barra de título, el aviso
flotante y la ayuda. Se quitaron las dos pantallas de demostración, la barra inferior de
iconos y lo que sólo usaban ellas.

Convenciones del aparato que la app respeta (documentadas en `CardputerZero/launcher` y
`m5stack/CardputerZero-AppBuilder`):

- Se instala como `.deb` con un `.desktop` en `/usr/share/APPLaunch/applications/`; el
  lanzador ejecuta el binario y le cede la pantalla (`/dev/fb0`) y el teclado (evdev).
- `Esc` corto es «atrás» y `Esc` mantenido sale; `Fn`+`H` ayuda, `Fn`+`J` captura.
- Las flechas están en `F`/`Z`/`X`/`C` con `Fn`. Aquí valen también sin `Fn` (lo pidió Bruno):
  las letras llegan tal cual y `gallery::action_for_key` las trata como flechas.
- El sonido del sistema lo lleva PipeWire/WirePlumber con `pipewire-pulse`.

## Decisiones

- **Empieza siempre en `$HOME`**, no en la última carpeta (lo pidió Bruno).
- **Sólo los JPEG se decodifican en la app** (libjpeg, con reducción en la DCT y giro EXIF).
  Todo lo demás y todos los vídeos pasan por el `ffmpeg` del sistema como proceso aparte, que
  entrega fotogramas RGB565 de 320×170 por una tubería. Añadir un formato es añadir su
  extensión en `gallery.cpp` si ffmpeg lo abre. PSD se quitó: el ffmpeg de Debian no lo abrió.
- **Vídeo a 15 fps** y con `-skip_loop_filter all`: la CM0 decodifica por software.
- **Sonido por la salida `pulse` de ffmpeg** si existe `$XDG_RUNTIME_DIR/pulse/native`, y si
  no por ALSA `default`; así quien elige altavoz, auriculares o Bluetooth es PipeWire. Si
  ffmpeg falla con sonido, se repite sin él. `ZEROGALLERY_AUDIO=none|pulse|alsa` lo fuerza.
- **Pausar es matar ffmpeg** y relanzarlo con `-ss`: con `-re`, congelar el proceso lo haría
  correr al soltarlo.
- **La interfaz está en inglés**, como las demás apps del aparato.
- El visor no tiene estado propio: pasar de foto mueve la selección de `gallery::Session`.

## Versiones y entrega

- La versión está en `project(... VERSION x.y.z)` de `CMakeLists.txt` y en `app-builder.json`:
  se suben a la vez con cada cambio que llegue al aparato, y se anota en «Estado» de aquí y
  del `README.md`.
- El `.deb` se copia a `~/Documentos/ZeroGallery-<versión>.deb`, sustituyendo al anterior.
- Commit en `main` con mensaje en español que empieza por la versión, y push.
- `app-builder.json` ya tiene la ficha para la tienda del Cardputer Zero (`czdev publish`),
  pero **no está publicada**: la primera publicación pide un vídeo de la app en el aparato.

## Estado

- **0.1.0 (16-nov-2026)**: primera versión. Lista de carpetas, visor de fotos y vídeos, GIF
  animados, tema claro/oscuro. Probada en el simulador; **sin probar en el aparato** (sonido,
  fluidez de vídeo en la CM0, que el teclado real mande las teclas esperadas).
