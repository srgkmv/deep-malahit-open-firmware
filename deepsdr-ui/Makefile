# Simulador de host del DEEPSDR 101.
#
#   make before   render de la UI actual, compilando el User/gfx.c del repo
#   make after    render del rediseno con el nucleo gfx2
#   make smoke    diagnostico de coste de bus y muestra de la fuente actual
#   make guard    comprueba que la banda reservada de arriba no deja pasar nada
#   make spec3b   render del espectro de la etapa 3b + comprobacion de anchos
#   make principal render de la pantalla principal completa
#   make bandas   render de la pantalla de bandas + comprobacion de la lista
#   make ajustes  render de ajustes y del detalle + comprobacion de anchos
#   make autoesc  autoescala del espectro: extremos frente a percentiles
#   make cw       decodificador de CW: barrido de velocidad y de senal-ruido
#   make modos    comprueba la tabla de modos de main.c
#   make bandas   comprueba los limites de banda contra su texto
#   make paletas  dibuja las 16 paletas con los mismos datos, para elegir
#   make comprueba todas las comprobaciones de geometria, sin renders
#   make armcheck compila gfx2 + assets para el GD32F450 y mide flash/RAM
#   make assets   regenera fuentes y colormap desde las TTF
#
# No hace falta la placa para nada de esto.

# Raiz del repositorio del firmware, de donde se lee la tabla de bandas.
FIRMWARE ?= ..

CC       = gcc
CFLAGS   = -O2 -Wall -I hostinc -I sim
VENDOR   = -I vendor
GFX2     = -I gfx2
# gfx2/idioma.c va en TODOS los enlaces, no en las reglas que lo necesitan.
#
# Por que: desde la traduccion (29/09/2026) medio firmware llama a tr(), y
# un fichero que hoy no lo llama puede llamarlo manana sin avisar. Ponerlo
# regla por regla significa que cada vez que alguien mete un tr() en un
# fichero ya copiado, el banco de ese fichero deja de enlazar - y el fallo
# no se parece en nada a su causa ("undefined reference to `tr'" en un
# banco del reloj). Cuesta unos kilobytes de binario de anfitrion que no
# se van a notar. Es una copia, asi que copiaschk la vigila.
LDLIBS   = gfx2/idioma.c -lm
ARMCC    = arm-none-eabi-gcc
ARMFLAGS = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
           -O2 -Wall -ffunction-sections -fdata-sections -I hostinc -I gfx2

.PHONY: all before after smoke guard spec3b principal bandas bandaschk ajustes cw autoesc modos persist csvlen comprueba armcheck assets png clean
all: before after

out/before: sim/before.c sim/lcd_sim.c vendor/gfx.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(VENDOR) -o $@ $^ $(LDLIBS)

out/after: sim/after.c sim/lcd_sim.c gfx2/gfx2.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/smoke: sim/smoke.c sim/lcd_sim.c vendor/gfx.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(VENDOR) -o $@ $^ $(LDLIBS)

out/guardtest: sim/guardtest.c sim/lcd_sim.c vendor/gfx.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(VENDOR) -o $@ $^ $(LDLIBS)

out/spec3b: sim/spec3b.c sim/lcd_sim.c vendor/gfx.c vendor/spectrum.c gfx2/gfx2.c gfx2/spec_chrome.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) $(VENDOR) -o $@ $^ $(LDLIBS)

#
# EL PUERTO AL MALAHIT DSP-1: LA MISMA PANTALLA A 480x320.
#
# Usa sim/principal.c tal cual - la maqueta de la pantalla principal - y
# cambia solo dos cosas: el tamano del panel simulado y la geometria, que
# viene de malahit/compat/malahit_layout.h. Ese fichero es el MISMO que
# usa la compilacion de la radio, asi que lo que sale en el PNG y lo que
# sale en el panel no pueden separarse.
#
# Y NO usa vendor/, que son los ficheros del firmware ORIGINAL y este
# repositorio no distribuye: tira de ../User/gfx.c y ../User/spectrum.c,
# que son los de este arbol y los que el puerto compila de verdad.
#
#
# -include DE LAS DOS CABECERAS DEL PANEL, Y POR QUE HACE FALTA.
#
# ../User/gfx.c pide "rm68120_exmc.h" entre comillas, y para un include
# entre comillas el compilador mira PRIMERO en el directorio del fichero
# que lo pide: gana ../User/rm68120_exmc.h, el de verdad, cuyo
# rm68120_dato() escribe en 0x60020000 -la ventana EXMC del GD32-. En un
# PC esa direccion no existe y el render se cae con SIGSEGV dentro de
# rm68120_dato().
#
# Eso es justo lo que vendor/ resuelve para los otros bancos: son COPIAS
# en una carpeta donde el fichero de verdad no esta, asi que hostinc/
# gana. Aqui no hay copia -se compila ../User/gfx.c tal cual- asi que
# hace falta otra via.
#
# Forzar la cabecera de hostinc ANTES: define ella misma la guarda
# RM68120_EXMC_H, y cuando gfx.c llega a su #include la encuentra ya
# definida y el fichero de verdad se queda en nada. Sin -D y sin tocar
# ningun fichero. Es el mismo mecanismo que usa la compilacion de la
# radio, al reves: alli se sustituye por una propia, aqui por la del
# simulador.
#
MALAHIT_LAYOUT = -I ../malahit/compat -I ../User -include malahit_layout.h \
                 -include hostinc/rm68120_exmc.h -include hostinc/lcd_dma.h \
                 -DLCD_W=480 -DLCD_H=320

#
# LAS TIPOGRAFIAS, A LA MITAD, SIN TOCAR UN SOLO FICHERO.
#
# ui_top.c y compania nombran las fuentes como identificadores sueltos
# -&font_ui_14b- asi que un -D las redirige todas de golpe. No hay que
# editar nada ni inventar una capa de indireccion.
#
# OJO: esto NO puede aplicarse a fonts_data.c, que es quien DEFINE esos
# simbolos. Si se le aplica, la definicion de font_ui_14b pasa a
# llamarse font_ui_11 y el enlace se cae con un simbolo duplicado. Por
# eso fonts_data.c se compila aparte, sin el remapeo.
#
# La mas pequena que hay es font_ui_11; de 15 px a 11 es un 73%, no un
# 50%. El lector de frecuencia si baja de verdad: font_num_44 son 46 px
# y font_num_20 son 21, o sea un 46%. Para bajar el texto de la interfaz
# a la mitad de verdad harian falta caras de 7-8 px, que no estan en el
# juego de assets y habria que generar con tools/gen_assets.py.
#
#
# DOS JUEGOS DE CARAS, Y POR ESO SE COMPILA ui_top.c APARTE.
#
# El remapeo es por UNIDAD DE TRADUCCION, asi que cada fichero puede
# llevar el suyo. ui_top.c es el unico que necesita la cara de 8 px -las
# chapas de estado y la pastilla del mando, que son seis en una fila de
# 480-; la regla del espectro y los botones de abajo se leen bien a 11 y
# encogerlos tambien seria empeorarlos sin motivo.
#
# ui_top.c usa font_ui_18b para el modo, la banda y MEM, y font_ui_14/14b
# para las chapas: por eso basta con remapear 14 y 14b a la de 8 y dejar
# 18b en 11. Las chapas encogen y los rotulos grandes no.
#
MALAHIT_FONTS_COMMON = -Dfont_ui_18b=font_ui_11 \
                       -Dfont_ui_14b=font_ui_11 \
                       -Dfont_ui_14=font_ui_11 \
                       -Dfont_num_20=font_ui_11

MALAHIT_FONTS_TOP = -include font_num_31.h -include font_ui_8.h \
                    -include font_ui_8b.h \
                    -Dfont_num_44=font_num_31 \
                    -Dfont_ui_18b=font_ui_11 \
                    -Dfont_ui_14b=font_ui_8b \
                    -Dfont_ui_14=font_ui_8 \
                    -Dfont_num_20=font_ui_11

out/malahit_top.o: ../User/ui_top.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) $(MALAHIT_LAYOUT) $(MALAHIT_FONTS_TOP) -c -o $@ $<

out/malahit_fonts.o: gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -c -o $@ $<

out/malahit: sim/principal.c sim/lcd_sim.c ../User/gfx.c ../User/spectrum.c \
             gfx2/gfx2.c gfx2/spec_chrome.c gfx2/ui_act.c \
             out/malahit_top.o out/malahit_fonts.o
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) $(MALAHIT_LAYOUT) $(MALAHIT_FONTS_COMMON) \
	      -o $@ $^ $(LDLIBS)

#
# LA PRUEBA DE QUE UN CAMBIO EN User/ NO TOCA AL DEEPSDR.
#
#   make ref800-guarda    antes de tocar nada
#   ...                   el cambio
#   make ref800-comprueba despues
#
# Renderiza la pantalla principal a 800x480 con los ficheros de User/ y
# compara el PPM BYTE A BYTE con el guardado. Es lo unico que convierte
# "esto no cambia nada arriba" en un hecho en vez de una esperanza, y es
# barato: dos ordenes y un diff binario.
#
# No usa vendor/ -que son los ficheros del firmware original y este
# repositorio no distribuye- sino ../User, que es lo que se esta
# tocando.
#
out/ref800: sim/principal.c sim/lcd_sim.c ../User/gfx.c ../User/spectrum.c \
            ../User/gfx2.c ../User/spec_chrome.c ../User/ui_top.c \
            ../User/ui_act.c ../User/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) -I ../User \
	  -include hostinc/rm68120_exmc.h -include hostinc/lcd_dma.h \
	  -o $@ $^ $(LDLIBS)

.PHONY: ref800-guarda ref800-comprueba
ref800-guarda: out/ref800
	@./out/ref800 >/dev/null && mv -f out/principal.ppm out/ref800.ppm
	@echo "  referencia guardada en out/ref800.ppm"

ref800-comprueba: out/ref800
	@test -f out/ref800.ppm || { echo "  no hay referencia: 'make ref800-guarda' primero"; exit 1; }
	@./out/ref800 >/dev/null && mv -f out/principal.ppm out/ref800_nuevo.ppm
	@cmp out/ref800.ppm out/ref800_nuevo.ppm \
	  && echo "  IDENTICO: el DEEPSDR no ha cambiado ni un pixel" \
	  || { echo "  *** DISTINTO: el cambio SI afecta al DEEPSDR ***"; exit 1; }

.PHONY: malahit
malahit: out/malahit
	./out/malahit
	@mv -f out/principal.ppm out/malahit.ppm 2>/dev/null || true
	@$(MAKE) png
	@echo "  -> out/malahit.ppm y out/malahit.png"

out/spec3blayout: sim/spec3b_layout.c sim/lcd_sim.c gfx2/gfx2.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/rejilla: sim/rejilla.c
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

out/snaptest: sim/snaptest.c gfx2/spec_snap.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/cwbw: sim/cwbw.c
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

out/digi: sim/digi.c sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_digi.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/kbd: sim/kbd.c sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_kbd.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/freqtap: sim/freqtap.c sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_top.c gfx2/fonts_data.c sim/bandas_tabla.h
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/paletachk: sim/paletachk.c sim/lcd_sim.c vendor/gfx.c vendor/spectrum.c gfx2/gfx2.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) $(VENDOR) -o $@ $^ $(LDLIBS)

out/paletas: sim/paletas.c sim/lcd_sim.c vendor/gfx.c vendor/spectrum.c gfx2/gfx2.c gfx2/spec_chrome.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) $(VENDOR) -o $@ $^ $(LDLIBS)

out/chromecost: sim/chromecost.c sim/lcd_sim.c vendor/gfx.c vendor/spectrum.c gfx2/gfx2.c gfx2/spec_chrome.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) $(VENDOR) -o $@ $^ $(LDLIBS)

out/principal: sim/principal.c sim/lcd_sim.c vendor/gfx.c vendor/spectrum.c gfx2/gfx2.c gfx2/spec_chrome.c gfx2/ui_top.c gfx2/ui_act.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) $(VENDOR) -o $@ $^ $(LDLIBS)

out/acttest: sim/acttest.c sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_act.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/layout: sim/layout.c sim/lcd_sim.c gfx2/gfx2.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

# Y DEPENDE DE main.c, no solo del generador - 30/09/2026.
#
# Le faltaba `$(FIRMWARE)/User/main.c` (las otras cinco tablas generadas si
# lo llevan). O sea que cambiar la tabla de bandas en main.c NO regeneraba
# esto, y out/bandas y out/freqtap seguian midiendo la lista vieja, en
# silencio. Se noto porque el .py llevaba meses sin tocarse: la foto que
# habia era de 46 bandas y la tabla tenia 49.
sim/bandas_tabla.h: tools/bandas_dump.py $(FIRMWARE)/User/main.c
	python3 tools/bandas_dump.py $(FIRMWARE)/User/main.c $@

out/bandas: sim/bandas2.c sim/bandas_tabla.h sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_cfg.c gfx2/ui_grid.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $(filter %.c,$^) $(LDLIBS)

out/ajustes: sim/ajustes.c sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_grid.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/detalle: sim/detalle.c sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_det.c gfx2/ui_grid.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/autoesc: sim/autoesc.c gfx2/spec_agc.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/cwtest: sim/cwtest.c gfx2/cw.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

out/config: sim/config.c sim/lcd_sim.c gfx2/gfx2.c gfx2/ui_cfg.c gfx2/ui_grid.c gfx2/fonts_data.c
	@mkdir -p out
	$(CC) $(CFLAGS) $(GFX2) -o $@ $^ $(LDLIBS)

before: out/before ; ./out/before && $(MAKE) png
after:  out/after  ; ./out/after  && $(MAKE) png
smoke:  out/smoke  ; ./out/smoke  && $(MAKE) png
guard:  out/guardtest ; ./out/guardtest
spec3b: out/spec3b out/spec3blayout ; ./out/spec3blayout && ./out/spec3b 0 && $(MAKE) png
ajustes: out/config out/detalle ; ./out/config 0 && ./out/detalle 0 && $(MAKE) png
bandas: out/bandas ; ./out/bandas 0 && $(MAKE) png
principal: out/principal out/layout out/acttest ; ./out/layout && ./out/acttest && ./out/principal && $(MAKE) png
cw: out/cwtest ; ./out/cwtest
autoesc: out/autoesc ; ./out/autoesc
modos: ; python3 tools/modos_check.py $(FIRMWARE)/User/main.c
bandaschk: ; python3 tools/bandas_check.py $(FIRMWARE)/User/main.c
comprueba: out/guardtest out/layout out/acttest out/spec3blayout out/bandas out/config out/cwtest out/autoesc out/paletachk out/freqtap out/kbd out/digi out/snaptest out/rejilla ; ./out/guardtest && ./out/layout && ./out/acttest && ./out/spec3blayout && ./out/bandas 0 && ./out/config 0 && ./out/cwtest && ./out/autoesc && ./out/paletachk && ./out/freqtap && ./out/kbd 0 && ./out/kbd 1 && ./out/digi && ./out/snaptest && ./out/rejilla && $(MAKE) modos && $(MAKE) bandaschk

png:
	@python3 -c "from PIL import Image; import glob,os; \
[Image.open(p).save(p[:-4]+'.png') for p in glob.glob('out/*.ppm')]"

assets:
	python3 tools/gen_assets.py

armcheck:
	@mkdir -p armcheck
	$(ARMCC) -c $(ARMFLAGS) armcheck/armmain.c -o armcheck/armmain.o
	$(ARMCC) -c $(ARMFLAGS) gfx2/gfx2.c        -o armcheck/gfx2.o
	$(ARMCC) -c $(ARMFLAGS) armcheck/stubs.c   -o armcheck/stubs.o
	arm-none-eabi-size -t armcheck/*.o

clean:
	rm -rf out armcheck/*.o

# Comprobaciones y reglas extra, en un fichero aparte (23/09/2026). El "-"
# de "-include" significa "si no esta, no pasa nada". Esta separado porque
# el puente con el que se escriben los ficheros de este proyecto no puede
# escribir ficheros llamados Makefile -son codigo que se ejecuta- pero si un
# .mk: asi anadir un banco nuevo no obliga a renombrar nada a mano cada vez.
#
# Un objetivo puede tener varias reglas mientras solo UNA lleve receta, asi
# que local.mk puede colgarle requisitos nuevos a "comprueba" sin tener que
# volver a escribir su receta entera.
-include local.mk
