# ZeroGallery

Galería de fotos y vídeos para el **M5Stack Cardputer Zero** (Raspberry Pi CM0: 4 núcleos
Cortex-A53 a 1 GHz, 512 MB, pantalla de 320×170, Debian 13 arm64). En el lanzador se llama
**Gallery**. Qué hace y cómo se usa está en `README.md`; aquí va lo que hace falta para tocarla.

No es una app de Android: de `~/CLAUDE.md` valen el estilo (comentarios, documentación y
commits en español explicando el porqué; pocas dependencias; lógica pura con pruebas), que el
repositorio es **público** (ni secretos ni datos personales) y la entrega; lo de Gradle, firma
y GrapheneOS no aplica.

## Compilar y comprobar

`Tools/compilar.sh` usa Docker si lo hay (así se hizo en el servidor, que no tiene CMake ni
compilador cruzado) y, si no, las herramientas de la máquina: los paquetes de `Tools/Dockerfile`.

```sh
Tools/compilar.sh pruebas   # simulador (SDL2) + ctest; tiene que pasar
Tools/compilar.sh deb       # dist/ZeroGallery_<versión>_rev1_arm64.deb
```

- `.cache` es un enlace a `~/.local/share/cardputer-zero-sdk` (LVGL y el *sysroot*, uno por máquina para todas las apps Zero; ver `~/Proyectos/CLAUDE.md`). En un clon nuevo: `mkdir -p ~/.local/share/cardputer-zero-sdk && ln -s ~/.local/share/cardputer-zero-sdk .cache`; si está vacío, la primera compilación lo descarga ahí.
- GitHub Actions hace lo mismo en cada push (`.github/workflows/build.yml`).
- **Probar sin pantalla**: el simulador corre bajo `Xvfb` dentro de la misma imagen
  (`SDL_RENDER_DRIVER=software`), se le mandan teclas con `xdotool key` y se captura con
  `import -window root`; la pantalla del aparato queda en `324x174+538+266` de esa captura.
  La app también guarda capturas exactas de 320×170 con `Print` en `~/Pictures/Screenshots`
  (de ahí salen las de `screenshot/`). En Docker no hay tarjeta de sonido, así que el audio
  no se puede probar aquí.
- **El aparato** es `cpzero` en la red de Tailscale (`ssh cpzero`, usuario `bruno`, con
  `sudo` sin contraseña): el `.deb` se copia y se instala con `sudo apt-get install -y`. Lo
  que haya que mirar en la pantalla lo confirma Bruno: hasta entonces se dice («sin probar en
  el aparato») en el «Estado».

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
- **El giro lo decide `gallery::rotation_from_gravity`** con el acelerómetro (BMI270 por IIO,
  `/sys/bus/iio/devices/*/in_accel_*_raw`, legible sin permisos especiales). Los ejes son los
  de la app oficial `CardputerZero/Compass` (`mapBmi270ToScreen`): de pie como se usa, el
  sensor marca `-X`. Cambia al pasar 60° (no 45°) y tras dos lecturas iguales, para que no
  baile; tumbado no cambia. Sólo se lee con el visor abierto (`platform::OrientationSensor`).
- **Girado de lado, la imagen se compone en 170×320** (libjpeg o ffmpeg encajan ahí) y se
  gira a la pantalla en `gallery::rotate_to_screen`. Un vídeo en marcha **relanza ffmpeg
  desde donde iba** (el tamaño va en sus argumentos), con un corte breve de sonido; en pausa
  o acabado se pide un fotograma suelto. Los rótulos giran con `transform_rotation` de LVGL.
- **Probar el giro sin aparato**: `ZEROGALLERY_IIO=<carpeta>` con un `iio:device1/` dentro y
  los tres `in_accel_?_raw` como ficheros corrientes (se reescriben con el simulador en
  marcha); `ZEROGALLERY_ROTATE=0..3` lo fija sin sensor.
- **En un vídeo las flechas saltan** (`gallery::seek_seconds_for`: ±10 s a los lados, ±30 s
  arriba y abajo) y de fichero se cambia con `,` y `.`, que en la lista son página arriba y
  abajo; lo pidió Bruno. En fotos y GIF las flechas siguen cambiando de fichero. Saltar es
  **otro ffmpeg con `-ss`**, como reanudar; los saltos pedidos mientras tanto se suman. La
  duración se pregunta a `ffprobe` la primera vez que se salta adelante, para no pasarse del
  final (se cae como mucho a un segundo de él).
- **Mandar a la tele es DLNA (UPnP AV), dentro de la galería y no en una app aparte** (lo
  decidió Bruno, 24-nov-2026): lo útil es «esta foto en la tele» y eso sólo lo sabe el visor;
  copiar la pantalla de 320×170 no merece la pena. La lógica sin red está en
  `src/model/dlna.*` (con pruebas, usando la descripción real de la LG de Bruno con los
  identificadores cambiados, en `tests/fixtures`), y la red en `src/platform/dlna_caster.*`:
  SSDP por cada interfaz con multicast, un servidor HTTP propio (con `Range`, que las teles
  usan para leer el índice de un MP4 del final del fichero) que sólo vive mientras se manda,
  y órdenes SOAP. Sin librerías: libupnp o gupnp serían más que toda la app.
- **`dlna.*`, `dlna_caster.*`, `tests/dlna_test.cpp` y `tests/fixtures` son los mismos
  ficheros que en el fork de Music** (`~/Proyectos/CardputerZero-Music/src/cast`, desde el
  24-nov-2026): no dependen de nada de la galería. Un arreglo en uno se copia tal cual al otro
  y se pasan las pruebas de los dos. Lo que sólo usa Music (audio con carátula y artista,
  volumen, `set_next` para encadenar canciones con `SetNextAVTransportURI`) vive ahí también.
- **Cosas de las teles que ya se han visto** (no quitarlas sin probar): la LG no contesta a
  `SetAVTransportURI` ni a `Play` hasta que ha abierto el fichero (3-6 s una foto): el tiempo
  máximo de esas dos es 20 s. Cierra la conexión a medias para pedir otro trozo: sin
  `SIGPIPE` ignorado, `sendfile()` mataba la app. Los metadatos DIDL-Lite van siempre (LG y
  Samsung no reproducen sin ellos), el tipo se elige con lo que la tele dice aceptar
  (`GetProtocolInfo`; la LG no dice `video/quicktime` pero abre los MOV como `video/mp4`),
  `Play` se reintenta si contesta 701 y `SetAVTransportURI` se repite tras un `Stop` si falla
  (Samsung). Las fotos se convierten a JPEG secuencial, derecho y de hasta 4K
  (`dlna::photo_needs_conversion`): el EXIF lo respetan unas teles sí y otras no.
- **Google Cast (0.5.0) va dentro del mismo `DlnaCaster`**: un Chromecast es un
  `dlna::Renderer` con `cast_host`/`cast_port`, y cada orden va por SOAP o por Cast según
  `is_cast()`; la lista, `V` y el servidor de ficheros son los mismos. `model/gcast.*` (sin red,
  con pruebas y una respuesta mDNS real de un Nest Hub) y `platform/google_cast.*` (conexión).
  Decisiones: **TLS con el `libssl.so.3` del sistema cargado con `dlopen`** (sin cabeceras ni
  dependencia en el `.deb`; si no está, Cast no sale y DLNA sigue), el certificado del aparato no
  se comprueba (es propio, como hacen todos); **un solo hilo lee y escribe** la conexión
  (OpenSSL no admite dos a la vez); búsqueda mDNS «legacy unicast» desde el mismo socket que
  SSDP, sin unirse al grupo; app CC1AD845 (el reproductor de Google). Cast no encadena la
  siguiente canción (`set_next` no hace nada): Music la manda al acabar. `ZEROGALLERY_CAST=ip`
  añade uno a mano.
- **Probar Cast sin ir a la granja**: `granja-pi` (Tailscale) está en su Wi-Fi, es aarch64 con
  Debian 13 como el Cardputer y tiene libssl (no ffmpeg). Allí hay dos Google Nest Hub
  («Cocina» y «Salón»); el Chromecast HD de la tele suele estar apagado. Se compila un
  programa de prueba con `dlna_caster` + `google_cast` y se lanza en `granja-pi`; el volumen del
  Nest se apunta, se baja y se devuelve. Así se probó la 0.5.0.
- **Con un vídeo en la tele, aquí no se reproduce** (se cierra el ffmpeg local): ahorra CPU y
  batería. Al dejar de mandar, el visor lo vuelve a abrir.
- **El cortafuegos del portátil (`ufw`) no deja probar con la tele desde el PC**: se come las
  respuestas SSDP y no dejaría a la tele bajarse el fichero. Se prueba desde el aparato, que no
  tiene cortafuegos y está en la misma Wi-Fi que la LG.
- **HDMI (0.6.0) sin libdrm ni mpv**: `platform/kms_display.*` usa los ioctls del kernel
  (`drm/drm_mode.h`): abre la tarjeta que tenga un HDMI `connected` en /sys (`card0`, `vc4`; la
  pantalla interna es `card1`, el panel SPI, y no se toca), pone el modo con un fondo XR24 negro
  y cada fotograma va a un **plano superpuesto YU12** que la GPU escala y centra. El modo:
  el preferido de la pantalla hasta 1920×1200 (lo más que saca el HDMI de la CM0). `ffmpeg`
  entrega I420 por la tubería (`platform/hdmi_player.*`): las fotos al tamaño de la pantalla,
  los vídeos al suyo si cabe (`hdmi::video_size`). Por qué así y no otra cosa (probado el
  25-nov-2026 en `cpzero`):
  - **Decodificador H.264 por hardware (0.6.1)**: `h264_v4l2m2m`
    (`/sys/class/video4linux/video*/name` = `bcm2835-codec-decode`). **Hace falta `gpu_mem` de
    64**: con 16 el firmware es `start_cd` y no existe; **con 32 el dispositivo aparece pero no
    funciona** (dmesg: «failed to create component -62 (Not enough GPU mem?)»; un 1080p se colgó
    28 s antes de fallar; probado el 25-nov-2026). Por eso (0.6.3) se pregunta también
    `vcgencmd get_mem gpu` (≥ 64, `hdmi::kHardwareDecoderGpuMem`) y, si el decodificador no da
    el primer fotograma en 6 s, se sigue por software (probado con un `ZEROGALLERY_FFMPEG` que se
    cuelga al pedir el hardware: a los 6 s, por software). `cpzero` va con 64 desde el 25-nov-2026 (puesto por ZeroPower a
    petición de Bruno). Se usa si el vídeo es H.264, hasta 1920×1080 y **sin giro** en los
    metadatos (`hdmi::hardware_decodable`); si ffmpeg falla sin sacar ningún fotograma, se
    repite por software. `ZEROGALLERY_HWDEC=0` lo desactiva.
  - **El decodificador deja los fotogramas en memoria sin caché**: leerlos con la CPU es
    lentísimo. Reducir un 1080p a 1024×576 con `scale` iba a 0,4× (24 s para 10 s); girarlo,
    igual. Por eso con hardware el fotograma va **a su tamaño** (`hdmi::hardware_size`) y lo
    reduce el plano de la GPU, y los vídeos de móvil grabados de pie van por software. Medido:
    1080p25 a tiempo real con ffmpeg al 100 % de un núcleo y la app al 29 % (antes, por software,
    186 %); 720p, 54 % + 31 %.
  - **1080p a 60 fps no llega a tiempo real** de ninguna forma: copiar 60 fotogramas de 3 MB
    por segundo de esa memoria es el límite (0,9× con hardware aunque se pidan 30, porque `fps`
    tira los sobrantes después de copiarlos; 0,8× por software). El tope es 60 fps hasta 720p
    con hardware y 30 en lo demás (`hdmi::max_fps`). La única salida sería no copiar (DRM PRIME),
    y este ffmpeg no lo da con el decodificador de la Pi 3: `-pixel_format drm_prime` →
    «v4l2 capture format not supported»; `hwdownload`/`hwmap` tampoco negocian.
  - Por software: un H.264 1080p se decodifica a 1,6× (2× sin el filtro antibloques, que se
    salta por encima de 720p) y se reduce a la pantalla antes de pasarlo (cuesta menos que pasar
    el fotograma entero). La copia al búfer DRM tarda menos de 27 ms el peor fotograma.
  - La salida `vout_drm` del ffmpeg de Raspberry Pi **no sirve**: sólo pinta en una CRTC ya
    encendida por otro y sólo acepta fotogramas DRM PRIME, que aquí no salen de ningún sitio
    (ni del decodificador, ver arriba, ni con `hwupload`).
  - `mpv` sería una dependencia de cientos de MB para lo mismo.
  - La app es la dueña (DRM master) de `card0` mientras manda al HDMI; con lightdm en marcha
    `SETCRTC` da EACCES y se avisa. Al dejar de mandar se apaga la salida, pero al cerrar la
    tarjeta la vuelve a coger la consola del kernel (fbdev, `fb1`, que se crea al enchufar) y la
    deja en negro: la tele no dice «sin señal». Es así sin la app también.
  - **El plano de fondo se apaga tras poner el modo (0.6.2)**: con él (XR24 a pantalla
    completa), a 1920×1200@60 la GPU **rechaza** una foto a pantalla completa encima
    (`SETPLANE` → ENOSPC: lo cuenta `vc4_load_tracker`, `hvs_load_tracker` = Y); a 1920×1150
    entraba justo. Apagado (con `DRM_CLIENT_CAP_UNIVERSAL_PLANES` y SETPLANE con fb 0 al
    primario), la GPU rellena de negro sola y son ~550 MB/s menos de lectura de memoria.
  - **Tres búferes y un hilo para SETPLANE (0.6.2)**: SETPLANE espera al refresco (8,5 ms de
    media, hasta 16,7): sin medio fotograma a la vista, pero en el hilo del reproductor un
    720p60 se quedaba en 45 fps. Ahora el reproductor copia y sigue; si llega un fotograma con
    otro esperando, el que esperaba se tira. 720p60 por hardware a 60 fps; 0 underruns.
  - Fotos a 1920×1200: 12 Mpx en ~2 s. `-lowres` de ffmpeg apenas ayuda (1,90 → 1,78 s: lo
    que pesa es leer el JPEG comprimido), así que no se usa.
  - El sonido no va por HDMI (`snd_bcm2835.enable_hdmi=0` en `cpzero`): va por PipeWire, como siempre.
- **Probar el HDMI sin cable**: `sudo sh -c 'echo on > /sys/class/drm/card0-HDMI-A-1/status'`
  lo da por conectado (modos de serie, 1024×768) y `echo detect` lo devuelve. **Para simular
  una pantalla concreta** (así se probó 1920×1200@60): se escribe un EDID de 128 bytes en
  `/sys/kernel/debug/dri/0/HDMI-A-1/edid_override`, se pone `off` y `on` en `status` y se
  sondea (`kmsprint -m`). **Al acabar, `printf reset >` ese fichero** (con `echo` da I/O error):
  si no, una pantalla de verdad enchufada antes de reiniciar usaría el EDID falso. Los fallos
  de la GPU al refrescar se cuentan en `/sys/kernel/debug/dri/0/hvs_underrun`. Lo que hay en
  cada plano se ve en `sudo cat /sys/kernel/debug/dri/0/state` (tamaño y formato del búfer) y
  quién es el dueño en `.../clients`; `kmsgrab` no captura planos YUV. Para probar sin interfaz
  se compila con `aarch64-linux-gnu-g++` un `main` que use `HdmiPlayer` con
  `hdmi_player.cpp kms_display.cpp ffmpeg_process.cpp` y `model/{hdmi,gallery,dlna}.cpp`
  (`-static-libstdc++`), con `ZEROGALLERY_AUDIO=none` para que no suene.
- **Las teclas del aparato** pasan por `map_evdev_key` (`linux_input.cpp`), que sólo deja pasar
  las letras que usa la app: una tecla nueva hay que añadirla ahí (la `V` faltaba y en el
  simulador sí iba).
- **La interfaz está en inglés**, como las demás apps del aparato.
- El visor no tiene estado propio: pasar de foto mueve la selección de `gallery::Session`.

## Versiones y entrega

- La versión está en `project(... VERSION x.y.z)` de `CMakeLists.txt` y en `app-builder.json`:
  se suben a la vez con cada cambio que llegue al aparato, y se anota en «Estado» de aquí y
  del `README.md`.
- El `.deb` se copia a `~/Documentos/Software/ZeroGallery-<versión>.deb`, sustituyendo al anterior.
- Commit en `main` con mensaje en español que empieza por la versión, y push.
- `app-builder.json` ya tiene la ficha para la tienda del Cardputer Zero (`czdev publish`),
  pero **no está publicada**: la primera publicación pide un vídeo de la app en el aparato.

## Pendiente

- **Probar el HDMI con un cable y una pantalla de verdad** (Bruno lo hará más adelante): que
  la imagen se vea (colores, encaje, sin tirones), el modo que coge con su EDID y desenchufar con
  algo en marcha.
- **Probar Cast en el Chromecast HD (Google TV) de la granja**: con el Nest Hub funciona todo,
  pero el Chromecast no se ha visto nunca encendido (Bruno lo probará allí).
- **Probar en Samsung, Sony y Hisense** (teles de amigos de Bruno): están los arreglos
  conocidos, pero sólo se ha probado en la LG.

## Estado

- **0.6.3 (25-nov-2026)**: el decodificador por hardware sólo con `gpu_mem` ≥ 64 (con 32 aparece
  pero no funciona) y, si no da el primer fotograma en 6 s, por software. Probado en `cpzero`
  con 32 (falla, ver «Decisiones») y de vuelta a 64 (desde ZeroPower): hardware normal, y con
  el decodificador colgado a propósito pasa a software a los 6 s.
- **0.6.2 (25-nov-2026)**: HDMI a 1920×1200@60 de verdad. Probado con un EDID simulado de
  1920×1200@59,95 (CVT-RB, 154 MHz): antes las fotos fallaban («cannot show the image»: la GPU
  no daba para el fondo y la foto); ahora sin plano de fondo y con tres búferes y un hilo para
  cambiarlos: foto de 12 Mpx (2,5 s) y pequeña, 1080p25 por hardware (93 % + 36 %), 720p60 por
  hardware a 60 fps (antes 45), pausa y saltos, 0 underruns. Sin probar con una pantalla de verdad.
- **0.6.1 (25-nov-2026)**: HDMI con el decodificador H.264 por hardware cuando está (gpu_mem
  ≥ 32; en `cpzero` ya 64) y hasta 1920×1200 (antes 1080p). Probado en `cpzero` con el conector
  forzado: 1080p25 y 720p por hardware a tiempo real con la mitad de CPU, pausa y saltos; el
  1080p60 de prueba a 0,9× (límite conocido). Sin probar con una pantalla de verdad.
- **0.6.0 (25-nov-2026)**: por HDMI (ver «Decisiones»). `V` lista «HDMI» primero si hay algo
  enchufado. Probado en `cpzero` con el conector forzado y sin cable: el reproductor (foto JPEG y
  PNG, vídeo 1080p y 320×240, pausa, saltos parado y en marcha, final y volver a empezar, GIF)
  con un programa de prueba, y la interfaz con ZeroRemote (lista con «HDMI» y la LG, foto,
  pasar a la siguiente, vídeo con su reloj y pausa, `V` suelta la tarjeta). El reproductor de
  la pantalla pasa a usar `platform/ffmpeg_process.*`, compartido con el HDMI (sin cambios a la
  vista). **Sin probar con una pantalla de verdad** ni el sonido del vídeo mandado al HDMI.
- **0.5.0 (24-nov-2026)**: Google Cast. Probado con un Google Nest Hub de la granja desde
  `granja-pi` (búsqueda, foto, música con carátula y su final, vídeo, saltar, pausa, volumen);
  la LG sigue igual (probado desde el aparato). **Sin probar por Bruno** y sin probar en el
  Chromecast HD.
- **0.4.1 (24-nov-2026)**: los ficheros de DLNA, generalizados para compartirlos con Music
  (audio, volumen, siguiente canción) y sin depender de `gallery`. Para la galería no cambia
  nada; comprobado desde el aparato con la LG (foto girada y vídeo).
- **0.4.0 (24-nov-2026)**: mandar a la tele por DLNA (`V`). Probado desde el aparato con la LG
  50UK6500PLA de Bruno: búsqueda (la encuentra en menos de un segundo), PNG convertida, JPEG
  con EXIF 6 (llega derecho a 1600×1200), MOV anunciado como MP4, saltos de ±10 s, pausa y
  reanudar, pasar de foto a vídeo, dejar de mandar (el vídeo vuelve a verse aquí) y salir con
  la tele en marcha (se queda en `STOPPED`; la app sale en 1,5 s). Ficha de la tienda: permiso
  `network`. **Bruno lo confirma con su LG (24-nov-2026: «ha funcionado perfecto»)**; sin probar
  en Samsung, Sony o Hisense.
- **Probada en el aparato (24-nov-2026):** Bruno la da por buena («zerogallery ok»), con la 0.3.1.

- **0.3.1 (20-nov-2026)**: la app ya no tiene **un núcleo al 100 %** en el aparato. Sin
  `lv_delay_set_cb`, `lv_delay_ms(5)` del bucle principal esperaba dando vueltas (lo cazó
  ZeroGPS con `top` y gdb: el hilo de LVGL siempre dentro de `lv_delay_ms`); venía de la
  plantilla. Ahora `app.cpp` da a LVGL `clock_gettime` y `usleep` en el aparato y duerme lo
  que `lv_timer_handler()` dice que falta (1-50 ms). Instalada en el aparato; probada por Bruno el 24-nov-2026.
- **0.3.0 (16-nov-2026)**: saltos en el vídeo con las flechas y `,` / `.` para cambiar de
  fichero. Probado en el simulador y **en el aparato: Bruno confirma que van bien**.
- **0.2.0 (16-nov-2026)**: fotos y vídeos giran con el aparato (acelerómetro). Probado en el
  simulador con un acelerómetro de mentira y **en el aparato: Bruno confirma que funciona**.
- **0.1.0 (16-nov-2026)**: primera versión. Lista de carpetas, visor de fotos y vídeos, GIF
  animados, tema claro/oscuro. Probada en el simulador y en el aparato (Bruno, 16-nov-2026: «todo lo
  demás lo he probado y está bien»).
