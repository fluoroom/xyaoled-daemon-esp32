# Preguntas pendientes

Decisiones que tomé solo mientras no estabas. Cada una dice qué hice por
defecto; respondé debajo de **Respuesta:** (alcanza con "ok" si te parece
bien) y lo ajusto.

Estado general al dejar esto: todo el firmware está escrito y compila. La
lógica pura (protocolo, subidas con acks, imágenes, parsers HTTP) pasa 31
tests en la PC, y el firmware completo corre en QEMU (con un panel
simulado en lugar de BLE y Ethernet en lugar de Wi-Fi) y pasa 16 pruebas
de integración contra la API real; los GIFs que recibe el panel coinciden
con lo que genera el daemon Go. Lo que falta probar en la placa es lo que
QEMU no emula: BLE con el panel de verdad, el Wi-Fi y el portal. Los
commits son locales, no los pusheé (ver pregunta 1).

---

### 1. ¿Pusheo los commits?

Hay varios commits locales sobre `main` que no subí a
`fluoroom/xyaoled-daemon-esp32`. La vez anterior me pediste commitear y
pushear, pero eso fue para aquel cambio.

**Por defecto:** quedan locales hasta que digas.

**Respuesta:**

---

### 2. Passthrough: ¿frames parciales y transparencia?

El passthrough manda tal cual un GIF que ya mide lo mismo que el panel.
Lo limité a GIFs en los que **cada frame cubre la pantalla entera y no
tiene transparencia**. El daemon Go siempre mandó frames completos y
opacos (componía todo antes), así que no sabemos si el decodificador del
panel compone bien los frames parciales con disposal y transparencia. Si
no lo hace, el GIF se vería roto.

Esto deja afuera muchos GIFs reales: los editores suelen optimizar con
frames parciales, y el encoder de Go marca como transparente cualquier
color de la paleta con alfa 0. Esos GIFs no fallan: pasan por el
procesamiento completo, que da el mismo resultado visual.

**Por defecto:** la versión conservadora. Con la placa se puede probar:
desactivo ese chequeo, subo un GIF optimizado de 96×16 y vemos si se ve
bien.

**Respuesta:**

---

### 3. `/display` con `"type"` después de `"image"`

El JSON de `/display` se decodifica a medida que llega, así que el tipo
se tiene que saber antes de que empiece la imagen. Si `"type"` viene
después de `"image"`, se ignora y el tipo se detecta solo: un GIF
animado es animación, lo demás es imagen fija. En Go el orden no
importaba. Solo cambia algo si alguien fuerza `"static"` sobre un GIF
animado poniendo `type` al final; el `curl` del README pone `image`
solo, así que no lo afecta.

**Por defecto:** así como está.

**Respuesta:**

---

### 4. GIF de un solo frame con `type: "animated"`

En Go, `ShowAnimatedGif` con un GIF de 1 frame lo manda como animación
(trailer animado, el delay del frame). Acá todo lo que tiene un solo
frame se manda como imagen fija (600 s, trailer estático), que es lo que
hace Go con `type: "static"` o vacío.

**Por defecto:** imagen fija.

**Respuesta:**

---

### 5. El servidor HTTP atiende un pedido a la vez

`esp_http_server` tiene una sola tarea. Mientras procesa y manda un GIF
grande (decenas de segundos esperando los acks del panel), los demás
pedidos esperan: `/health`, la página e incluso los frames del
`/stream`. El daemon Go atendía en paralelo. Se puede agregar una tarea
aparte para los trabajos largos (unos 10 KB de RAM más) y que el
servidor siga respondiendo; los pedidos al panel quedan en fila igual,
porque el panel acepta un upload por vez.

**Por defecto:** una tarea; lo agrego si molesta en la práctica.

**Respuesta:**

---

### 6. Límites que no tenía el daemon Go

- Frame de `/stream`: hasta **64 KB** (en Go, 4 MB). Los frames de la
  página de test son de 96×16 y pesan unos pocos KB.
- GIF animado por `/stream`: hasta **4 frames** (se decodifica en RAM
  para no gastar la flash a 4 fps).
- Playlist: hasta **32 items**.
- Animaciones: hasta ~450 frames de 96×16, según el espacio libre en
  scratch. El tamaño del archivo de origen no importa.

**Por defecto:** esos valores.

**Respuesta:**

---

### 7. ¿Conecto por MAC a tu panel?

El daemon Go se conectaba al primer dispositivo con `XYAO` en el nombre;
la MAC del `.env` (`2A:4D:FD:AB:AE:16`) se leía pero nunca se usaba. Acá
podés elegir en `/config`: por nombre (por defecto, igual que Go) o por
MAC. Si tenés un solo panel, por nombre alcanza.

**Por defecto:** por nombre, `XYAO`.

**Respuesta:**

---

### 8. `/config` sin contraseña

Cualquiera en tu red puede abrir `/config`, cambiar ajustes, hacer reset
o descargar el export (que incluye la contraseña del Wi-Fi). El daemon Go
tampoco tenía autenticación en su API. Se podría proteger `/config` y
`/api/*` con una clave (HTTP Basic, configurable).

**Por defecto:** sin clave.

**Respuesta:**

---

### 9. RAM: código de Wi-Fi fuera de IRAM

En el C3, el código que corre desde IRAM le quita lugar al heap. Saqué de
IRAM las optimizaciones de Wi-Fi y usé el driver de flash de la ROM: el
heap disponible pasó de 195 a 224 KB (antes de que arranquen la radio y
las tareas). A cambio, el Wi-Fi rinde un poco menos. No debería
notarse: lo que limita la velocidad de subida es el ack BLE del panel.

**Por defecto:** así. Si en la placa el Wi-Fi anda lento, se revierte en
`firmware/sdkconfig.defaults`.

**Respuesta:**

---

### 10. Credenciales Wi-Fi para desarrollo

Para no pasar por el portal cada vez que borrás la flash, podés crear
`firmware/sdkconfig.defaults.local` (git lo ignora) con:

```
CONFIG_XY_WIFI_SSID="tu-red"
CONFIG_XY_WIFI_PASSWORD="tu-clave"
```

y borrar `firmware/sdkconfig` antes de compilar. Lo guardado desde
`/config` tiene prioridad sobre esto.

**Por defecto:** no creé el archivo; necesito tus datos o lo creás vos.

**Respuesta:**

---

## Para la primera prueba en la placa

Cuando estés en `uucp` (`sudo usermod -aG uucp $USER` y volver a
iniciar sesión), el orden que propongo:

1. `idf.py -p /dev/ttyACM0 flash monitor` y anotar los logs de heap
   (`boot`, `Wi-Fi up`, `BLE up`, `HTTP up`).
2. Sin Wi-Fi guardado, conectarse a `xyaoled-setup` y configurar la red
   desde el portal.
3. Ver en el log que encuentra y conecta el panel (`✓ connected to the
   panel`, tamaño y brillo).
4. `/test`: brillo, una imagen fija, un GIF animado, el stream de la
   webcam.
5. Medir cuánto tarda en subir un GIF grande y cuántos fps da el stream,
   comparado con el daemon Go (~7 fps).
6. Recorrer `/config` en el navegador: estado, escaneo de redes, guardar
   ajustes, exportar e importar. Su API pasó los tests en QEMU, pero la
   interfaz en sí (el JavaScript de la página) nunca corrió en un
   navegador: para eso tendría que haber abierto Chrome o Firefox, que
   escriben en tu home, y me pediste no tocar nada fuera de esta carpeta.
