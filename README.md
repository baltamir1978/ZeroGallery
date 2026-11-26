# ZeroGallery

Galería de fotos y vídeos para el [M5Stack Cardputer Zero](https://docs.m5stack.com/): se elige
una carpeta y se pasan sus fotos y vídeos a pantalla completa con las flechas del teclado.
Con `V`, lo que se está viendo **sale en la tele** —una Smart TV por DLNA (LG, Samsung, Sony,
Hisense…) o un Chromecast, Google TV o Nest Hub por Google Cast, o una pantalla enchufada a su
**HDMI**— y se sigue manejando desde el Cardputer.

<p>
  <img src="screenshot/folders.png" alt="Lista de una carpeta" width="320" />
  <img src="screenshot/photo.png" alt="Una foto a pantalla completa" width="320" />
</p>

Está hecha sobre la [plantilla oficial](https://github.com/CardputerZero/Template) (LVGL 9.5 +
CMake), así que se ve y se maneja como el resto de apps del aparato: barra de título con hora,
red y batería, tema claro u oscuro, ayuda con `Fn`+`H` y captura con `Fn`+`J`.

## Qué hace

- **Empieza en la carpeta personal** del usuario y deja entrar en cualquier subcarpeta (y subir
  hasta `/`, para llegar a una tarjeta o un USB montados). Carpetas primero y, dentro de cada
  grupo, en orden natural (`IMG_2` antes que `IMG_10`). Los ficheros ocultos no salen.
- **Fotos**: los JPEG se abren con libjpeg, ya reducidos al decodificar (una foto de 12 Mpx no
  ocupa 48 MB en memoria) y enderezados según su orientación EXIF. El resto de formatos los abre
  el `ffmpeg` del sistema: PNG, GIF, BMP, WebP, HEIC/HEIF, AVIF, JPEG XL, JPEG 2000, TIFF, TGA,
  ICO, SVG, QOI, PNM, EXR, HDR…
- **GIF y APNG animados** se reproducen en bucle.
- **Vídeos** con `ffmpeg`: MP4, MOV, MKV, WebM, AVI, MPEG, TS/MTS, WMV, FLV, OGV, 3GP… con
  cualquier códec que traiga el ffmpeg de Debian (H.264, HEVC, VP9, AV1, MPEG-2/4, Theora…).
  Se ven a su propia velocidad (25, 24, 30…) con 30 fotogramas por segundo de tope, ajustados a la
  pantalla de 320×170: un vídeo de 60 sale a 30 y uno de 50 a 25, sin repetir ni saltarse fotogramas.
- **Gira con el aparato**: en el visor, al poner el Cardputer de lado o boca abajo la foto o
  el vídeo se dan la vuelta para seguir viéndose derechos (lo dice el acelerómetro), y de lado
  se encajan en 170×320, que es como mejor se ven las fotos y los vídeos verticales. Tumbado
  en la mesa se queda como estaba. La lista de carpetas no gira.
- **Saltar en un vídeo** con las flechas: 10 segundos a los lados y 30 arriba y abajo.
- **Sonido** por PipeWire (el servidor de sonido del aparato): suena por donde esté saliendo
  el sonido del sistema —altavoz, auriculares o Bluetooth— y con su volumen. Si un vídeo no
  tiene sonido o no hay salida, se reproduce sólo la imagen.
- **Mandar a la tele** (`V`) por DLNA o Google Cast (Chromecast, Google TV, Nest Hub): lo que
  se está viendo sale en la tele, a su resolución y con su sonido, y se sigue manejando desde
  el Cardputer (ver abajo). También **por HDMI**, a una pantalla enchufada al Cardputer.

## Mandar a la tele

Con una foto o un vídeo delante, `V` busca las teles de la red y las lista (las DLNA con su
nombre; los aparatos Google Cast, con el modelo entre paréntesis). `Enter` manda lo que se está
viendo a la señalada; la última usada sale ya señalada la próxima vez. Desde ese momento, pasar de foto o de vídeo lo cambia
también en la tele, y en un vídeo las flechas saltan y `Enter` pausa **en la tele**. Arriba a
la izquierda el rótulo lleva delante `TV` mientras se está mandando. Otra `V` (también desde la
lista de carpetas) deja de mandar y para la tele; salir de la app también la para.

- La tele no recibe una copia: recibe una dirección y se baja el fichero del Cardputer, que
  hace de servidor mientras se está mandando (sólo da los ficheros mandados, con una dirección
  imposible de adivinar, y se cierra al dejar de mandar). Decodifica la tele, así que un vídeo
  se ve con toda su calidad y el Cardputer apenas gasta: aquí no se reproduce mientras tanto.
- **Fotos**: las teles sólo aceptan con seguridad JPEG, y no todos. Las fotos que no lo son
  (PNG, HEIC, WebP, GIF…), los JPEG progresivos, en CMYK, de más de 4096 px o con la orientación
  en el EXIF (las de móvil) se convierten antes a un JPEG derecho de hasta 4K con `ffmpeg`; es
  lo que tarda un par de segundos más. El resto va tal cual.
- **Vídeos** van tal cual: si la tele no sabe abrir un formato, lo dice ella (y el Cardputer lo
  enseña). A una tele DLNA se le pregunta antes qué tipos acepta; un MOV de iPhone (que por
  dentro es MP4) se le anuncia como MP4, que es como lo aceptan LG, Samsung, Sony y Chromecast.
- **Google Cast** abre en el aparato el reproductor de Google (el mismo que usan muchas apps
  del móvil para «enviar») y le pasa la dirección del fichero; al dejar de mandar se cierra y el
  aparato vuelve a su pantalla de reposo. La conexión va cifrada (TLS) con el `libssl` del
  sistema, que el Cardputer ya trae.
- Tienen que estar **en la misma red** (Wi-Fi o cable de casa): las teles se buscan por
  multicast (SSDP para DLNA, mDNS para Cast), que no pasa por Tailscale. Para una red sin
  multicast se puede dar a mano: `ZEROGALLERY_TV=<url de la descripción DLNA>` o
  `ZEROGALLERY_CAST=<ip>`. `ZEROGALLERY_DLNA_DEBUG=1` cuenta en el terminal todo lo que pasa.

### Por HDMI

Con una pantalla o una tele enchufada al HDMI del Cardputer, la lista de `V` empieza por
**HDMI**, y se maneja igual que una tele: pasar de foto, saltar y pausar. Aquí no decodifica
la tele sino el Cardputer, con `ffmpeg`, y lo pinta en esa salida sin escritorio de por medio:

- Se pone el modo que prefiere la pantalla, hasta **1920×1200 a 60 Hz**, que es lo más que saca
  el HDMI del Cardputer (con una 4K, el mayor hasta ahí). Las fotos se encajan a ese tamaño; los
  vídeos llegan a su tamaño y la GPU los amplía (o reduce) a pantalla completa.
- Los **H.264 hasta 1080p** usan el **decodificador por hardware** si la memoria de la GPU es de
  **64 MB** o más (en ZeroPower, *GPU memory*; con 16 no existe y con 32 no le basta): a velocidad real con la mitad de
  CPU. Si no, por software, que también llega a 1080p a 30 fps pero gasta casi dos núcleos.
  Por software van también HEVC (el chip no lo decodifica), los vídeos de móvil grabados de pie
  (girarlos con hardware sería lentísimo) y los de más de 1080p.
- Hasta 30 fps (60 en 720p con hardware, comprobado). Sin tirones ni medio fotograma a la
  vista: cada fotograma cambia en un refresco de la pantalla. **Un 1080p a 60 fps no llega a velocidad real** (se
  queda en un 90 %): pasar tantos fotogramas grandes de la GPU a la pantalla es más de lo que da
  el Cardputer.
- El sonido va **a la tele por el HDMI** si la pantalla acepta sonido (un monitor sin altavoces
  no). Con `A` se cambia (ver abajo).
- Con un escritorio en la salida HDMI (lightdm), la salida está ocupada y lo dice.
- Al dejar de mandar, la pantalla se queda en negro (la consola del sistema vuelve a coger el
  HDMI), no en «sin señal».
- Mientras se manda al HDMI, la pantalla del Cardputer sigue enseñando la foto; con un vídeo,
  sólo cómo va (para no decodificarlo dos veces).

Lo que pide cada aparato (DLNA es un estándar, pero cada marca lo pone a su manera):

| Tele | Qué hacer | Probado |
|---|---|---|
| LG (webOS) | Nada. Tarda 3-4 s en enseñar una foto; un vídeo ya visto lo sigue donde se dejó | Sí: 50UK6500PLA (2018) |
| Samsung (Tizen) | La primera vez pregunta en la tele si se deja conectar: **Permitir**. Si se dijo que no: Ajustes → General → Administrador de dispositivos externos → Administrador de conexión → Lista de dispositivos | No |
| Sony Bravia | Ajustes → Red → Red doméstica → **Renderizador** activado, y el Cardputer permitido en su control de acceso | No |
| Hisense (VIDAA) | Ajustes → Red (o Sistema) → **Compartir contenido** activado | No |
| Google Cast: Chromecast, Google TV, Nest Hub (y teles con Chromecast integrado) | Nada. Sale en la lista con su nombre y el modelo entre paréntesis, «Salón (Chromecast HD)». Se usa el reproductor de Google, que abre MP4, WebM, MKV (según el códec), MP3, AAC, FLAC, OGG, WAV y JPEG/PNG/GIF/WebP: lo demás no lo reproduce | Sí, en un Google Nest Hub: foto, vídeo, saltos, pausa, volumen. Chromecast HD, sin probar |

## Por dónde suena

Con una foto o un vídeo delante, `A` cambia por dónde suena: **altavoz** del Cardputer,
**cascos** (sólo si hay unos enchufados) y **HDMI** (sólo si hay una pantalla que acepta sonido).
Arriba sale «Sound: …» un momento. Si un vídeo está en marcha, sigue desde donde iba por la
salida nueva.

- Al mandar al HDMI con `V`, el sonido pasa solo a la tele; al dejar de mandar, vuelve al
  Cardputer.
- Altavoz y cascos comparten la salida del Cardputer: al enchufar unos cascos el altavoz se
  calla solo. Con «Speaker» y los cascos puestos suenan los dos.
- Por el HDMI el volumen es el de la tele.

## Teclas

Las flechas del Cardputer Zero están en `F`, `Z`, `X` y `C`: valen **con o sin `Fn`**.

| Tecla | En la lista | Viendo una foto | Viendo un vídeo |
|---|---|---|---|
| `Z` / `C` (← →) | Carpeta de arriba / abrir | Anterior / siguiente | 10 s atrás / adelante |
| `F` / `X` (↑ ↓) | Subir y bajar (dejándola pulsada, corre) | Anterior / siguiente | 30 s adelante / atrás |
| `,` / `.` | Página arriba / abajo | Anterior / siguiente | Anterior / siguiente |
| `Enter` o espacio | Abrir carpeta, foto o vídeo | — | Pausar o reanudar; repetirlo si acabó |
| Girar el aparato | — | Gira la foto | Gira el vídeo |
| `Esc` | Carpeta de arriba | Volver a la lista | Volver a la lista |
| `Esc` **mantenido** | Salir de la app | Salir de la app | Salir de la app |
| `V` | Dejar de mandar a la tele | Mandar a la tele / dejar de mandar | Lo mismo |
| `A` | — | Por dónde suena: altavoz, cascos o HDMI | Lo mismo (sigue donde iba) |
| `Fn`+`D` / `Fn`+`S` | — | Subir / bajar el volumen (de 5 en 5; mantenida, seguido) | Lo mismo |
| `Fn`+`A` | — | Silenciar / quitar el silencio | Lo mismo |
| `T` | Tema claro / oscuro (se recuerda) | Lo mismo | Lo mismo |
| `Fn`+`H` | Ayuda | Ayuda | Ayuda |
| `Fn`+`J` | Captura en `~/Pictures/Screenshots` | Lo mismo | Lo mismo |

En la carpeta inicial un `Esc` corto no sale: recuerda que hay que mantenerlo.

Al saltar en un vídeo, arriba a la izquierda sale un momento por dónde va (`1:23`). Saltar
adelante llega como mucho a un segundo del final, y con el vídeo acabado saltar atrás lo vuelve
a poner en marcha desde ahí. Los saltos valen también en pausa. Un GIF animado no es un vídeo:
en él las flechas pasan al fichero anterior o siguiente, como en una foto.

## Instalar

Hace falta el `.deb` (ver más abajo cómo se compila). En el aparato:

```sh
scp dist/ZeroGallery_*_arm64.deb pi@<ip-del-cardputer>:
ssh pi@<ip-del-cardputer> 'sudo apt install ./ZeroGallery_*_arm64.deb'
```

`apt` instala también `ffmpeg` si falta. La app aparece en el lanzador como **Gallery**.

## Compilar

Con Docker no hace falta nada más: `Tools/compilar.sh` monta el entorno (Debian 13 con CMake,
SDL2 y el compilador cruzado) la primera vez. Sin Docker, el mismo script usa las herramientas
de la máquina si están instaladas (los paquetes de `Tools/Dockerfile`).

```sh
Tools/compilar.sh pruebas     # simulador de escritorio + pruebas de la lógica
Tools/compilar.sh deb         # dist/ZeroGallery_<versión>_rev1_arm64.deb
Tools/compilar.sh simulador   # abre el simulador con la carpeta personal de este PC
```

Sin Docker, con las dependencias instaladas (ver `Tools/Dockerfile`), son los *presets* de la
plantilla: `cmake --preset linux-x86-64` para el simulador y
`cmake --workflow --preset cp0-cross-package` para el `.deb`. La primera compilación descarga
LVGL y, para el aparato, el *sysroot* de M5Stack a `.cache/`.

## Cómo está hecho

```
src/model/gallery.*          lógica sin LVGL: listar, ordenar, navegar, encajar, EXIF, teclas
src/viewmodel/               qué hace cada tecla en cada pantalla; avisa a las vistas
src/platform/media_player.*  hilo que decodifica (libjpeg) y maneja ffmpeg
src/platform/dlna_caster.*   tele: búsqueda (SSDP y mDNS), servidor HTTP de ficheros y órdenes
src/platform/google_cast.*   conexión Google Cast (TLS con el libssl del sistema)
src/platform/hdmi_player.*   HDMI: hilo que maneja ffmpeg para la salida HDMI
src/platform/kms_display.*   HDMI: modo y planos por DRM/KMS (ioctls del kernel, sin libdrm)
src/platform/ffmpeg_process.* lanzar ffmpeg/ffprobe y leer su tubería (pantalla y HDMI)
src/model/hdmi.*             HDMI sin hardware: modo, tamaño del vídeo, ffprobe
src/model/dlna.*             DLNA sin red: respuestas, descripciones, SOAP, tipos, cabeceras
src/model/gcast.*            Google Cast sin red: mDNS, mensajes protobuf, JSON, peticiones
src/platform/linux_input.*   teclado (evdev) y repetición de flechas
src/view/screens/            lista de carpeta (browser) y visor a pantalla completa (viewer)
src/app/                     arranque, ESC corto/largo, simulador de escritorio
tests/                       pruebas de src/model (con la descripción real de una LG)
```

- Cargar y reproducir va en un hilo aparte: en la CM0 abrir una foto grande tarda décimas de
  segundo y en el hilo de la interfaz dejaría el teclado sin responder. Pedir otra foto
  abandona la anterior sin esperar.
- `ffmpeg` corre como proceso aparte y entrega fotogramas RGB565 de 320×170 por una tubería.
  Así la app no enlaza las librerías de códecs y un fichero roto no la tira. Para el vídeo
  saca la imagen y el sonido del mismo proceso, que es lo que los mantiene a la par.
- Pausar un vídeo es parar ese proceso y, al reanudar, lanzarlo otra vez desde donde iba.
- La lista sólo crea las seis filas que caben en pantalla, tenga la carpeta las fotos que tenga.

## Estado

**0.7.1 (26-nov-2026)** — los vídeos se ven fluidos en la pantalla del aparato: a su propia
velocidad (30 de tope) y con todos sus fotogramas (antes llegaban a la pantalla unos 6 de cada
25). Por HDMI también: llegan todos los fotogramas (antes, 14 de 25). Volumen con `Fn`+`D`/`S` y
silencio con `Fn`+`A`, con una barra durante un segundo.

**0.7.0 (25-nov-2026)** — `A` elige por dónde suena: altavoz, cascos o HDMI, y al mandar al
HDMI el sonido va a la tele. Probado con una tele simulada; sin probar los cascos ni con una
tele de verdad.

**0.6.3 (25-nov-2026)** — el decodificador por hardware sólo con 64 MB de GPU o más (con 32 no
funciona) y, si se cuelga, el vídeo sigue por software a los 6 s.

**0.6.2 (25-nov-2026)** — HDMI a 1920×1200 a 60 Hz con fotos a pantalla completa (antes
fallaban a esa resolución) y vídeo de 720p a 60 fps. Probado simulando un monitor de
1920×1200; sin probar con una pantalla de verdad.

**0.6.1 (25-nov-2026)** — HDMI con el decodificador por hardware (H.264 hasta 1080p, con
`gpu_mem` de 32 MB o más) y hasta 1920×1200. Probado con la salida forzada; sin probar con una
pantalla de verdad.

**0.6.0 (25-nov-2026)** — por HDMI: la lista de `V` empieza por «HDMI» con una pantalla
enchufada. Probado en el Cardputer con la salida forzada a «conectada» y sin cable (fotos,
vídeo 1080p a velocidad real, pausa, saltos, final, GIF; el contenido se comprobó en los planos
de la GPU). **Sin probar con una pantalla de verdad.**

**0.5.0 (24-nov-2026)** — Google Cast: los Chromecast, Google TV y Nest Hub salen en la misma
lista de `V` y funcionan como una tele DLNA. Probado con un Google Nest Hub (el mismo código,
desde otra máquina de esa red). Un Chromecast con Google TV, sin probar.

**0.4.1 (24-nov-2026)** — el código de DLNA es el mismo que usa la versión propia de Music para
oír la música en la tele. Sin cambios a la vista.

**0.4.0 (24-nov-2026)** — mandar a la tele por DLNA con `V`. Probado desde el Cardputer con
una LG webOS de 2018: fotos (también PNG convertida y JPEG girado por EXIF, que llega derecho),
un MOV, saltos, pausa, cambiar de fichero, dejar de mandar y salir de la app con la tele en
marcha; comprobado también en el aparato. Samsung, Sony y Hisense, sin probar: van por el estándar con los arreglos conocidos de
cada una.

**0.1.0 (16-nov-2026)** — primera versión. Probada en el simulador de escritorio: carpetas
(también largas), fotos con giro EXIF, GIF animado, vídeo con pausa, reanudación y repetición,
y salida con `Esc`. Las órdenes de `ffmpeg` que usa la app se han comprobado con 23 formatos de
imagen y 11 combinaciones de contenedor y códec de vídeo.

**0.3.0 (16-nov-2026)** — en un vídeo las flechas saltan 10 o 30 segundos, y con `,` y `.` se
cambia de fichero. Probado en el simulador (en marcha, en pausa, al final y pasándose del
final) y comprobado en el aparato.

**0.2.0 (16-nov-2026)** — fotos y vídeos giran con el aparato. Probado en el simulador con un
acelerómetro de mentira en las cuatro posiciones (foto, vídeo en marcha y en pausa) y
comprobado en el aparato.

## Licencia

MIT, como la plantilla de M5Stack de la que parte (ver `LICENSE`). Los iconos son de
[Phosphor Icons](https://phosphoricons.com/) (MIT; ver `assets/fonts/LICENSE.txt`).
