#!/usr/bin/env python3
"""
Extrae k_band_presets[] de User/main.c a una cabecera que consume el
simulador.

Existe para que el render y la placa no puedan enseñar listas distintas:
la tabla de verdad esta en el firmware y aqui solo se lee.
"""
import re, sys, pathlib

src = pathlib.Path(sys.argv[1]).read_text(encoding='utf-8')
m = re.search(r'static const band_preset_t k_band_presets\[\] = \{(.*?)\n\};', src, re.S)
if not m:
    sys.exit('no encuentro k_band_presets[]')

# Los numeros de familia se LEEN de main.c, no se copian aqui: son el orden
# en que salen en la columna, y el dia que se reordenen este script tiene que
# reordenarse con ellos o el render enseña una cosa y la placa otra.
fam = {n: int(v) for n, v in
       re.findall(r'#define\s+(BAND_FAM_\w+)\s+(\d+)U', src)
       if n != 'BAND_FAM_COUNT'}
modo = {'DEMOD_MODE_AM': 'AM', 'DEMOD_MODE_SAM': 'SAM', 'DEMOD_MODE_USB': 'USB',
        'DEMOD_MODE_LSB': 'LSB', 'DEMOD_MODE_NFM': 'NFM', 'DEMOD_MODE_WFM': 'WFM'}

# EL PASO DE CADA BANDA, COMO INDICE - 21/11/2026, para el port del
# Malahit, que pone el paso de la banda al elegirla como hace main.c.
# BAND_STEP_<sufijo> es la posicion de <sufijo> en TUNE_STEPS_LISTA, asi
# que se lee el orden de la lista y no se copia aqui.
lista = re.search(r'#define TUNE_STEPS_LISTA\(X\)(.*?)\n\s*\n', src, re.S)
if not lista:
    sys.exit('no encuentro TUNE_STEPS_LISTA')
paso = {'BAND_STEP_' + suf: i for i, suf in
        enumerate(re.findall(r'X\(\s*(\w+)\s*,', lista.group(1)))}
# Y LA LISTA ENTERA: hercios, rotulo y para que sirve, en los dos idiomas.
# El port la usa como su tabla de pasos, en vez de escribir otra.
pasos = re.findall(r'X\(\s*\w+\s*,\s*(\d+)UL\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)',
                   lista.group(1))
if len(pasos) != len(paso):
    sys.exit('TUNE_STEPS_LISTA: %d pasos con nombre y %d enteros' % (len(paso), len(pasos)))

filas = []
for ln in m.group(1).split('\n'):
    ln = re.sub(r'/\*.*?\*/', '', ln).strip()
    if not ln.startswith('{'):
        continue
    # LA FILA ENTERA, TAL COMO ES HOY - 30/09/2026.
    #
    # Esta expresion pedia UN solo valor "UL" y la tabla lleva TRES desde que
    # cada banda guarda principio, final y centro. No fallaba porque make
    # solo regenera esto cuando cambia el .py, y el .py no se habia tocado:
    # sim/bandas_tabla.h llevaba meses siendo una foto vieja que nadie
    # comparaba con nada. Al tocar el fichero por la traduccion salto.
    #
    # Y el nombre es un T("es","en") desde el 29/09/2026.
    f = re.match(r'\{\s*(?:T\s*\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)|"([^"]*)")'
                 r'\s*,\s*"([^"]*)"\s*,\s*(\d+)UL\s*,\s*(\d+)UL\s*,\s*(\d+)UL\s*,'
                 r'\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}', ln)
    if not f:
        sys.exit('fila que no entiendo: ' + ln)
    n_es, n_en, n_1, rango = f.group(1), f.group(2), f.group(3), f.group(4)
    nombre = n_es if n_es is not None else n_1
    # LOS DOS NOMBRES. Casi todas las bandas se llaman igual en los dos
    # idiomas ("40 m" es "40 m"), pero no todas: "OL" es "LW". La tabla
    # guardaba uno solo y el banco de anchuras medía, por tanto, medio
    # problema. Ahora van los dos y el banco los mide los dos.
    filas.append((nombre, n_en if n_en is not None else nombre, rango,
                  int(f.group(7)), modo[f.group(8)], fam[f.group(10)],
                  int(f.group(5)), int(f.group(6)), paso[f.group(9)]))

# LAS FAMILIAS, EN LOS DOS IDIOMAS - 30/09/2026.
#
# Desde la traduccion son T("Aficionados","Amateur"), y sacar todas las
# comillas daba SEIS nombres para TRES familias. El banco los listaba como
# seis familias y repartia las bandas por indices que ya no correspondian:
# decia "Radiodifusion tiene 19 bandas" con una rejilla de 16 celdas, o sea
# inventaba un desbordamiento. Un banco que se equivoca en la direccion de
# "hay un fallo" es tan malo como uno que calla.
fn = re.search(r'k_band_familias\[BAND_FAM_COUNT\] = \{(.*?)\};', src, re.S)
familias, familias_en = [], []
if fn:
    for m_ in re.finditer(r'T\s*\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)|"([^"]*)"',
                          fn.group(1)):
        a_, b_, c_ = m_.groups()
        familias.append(a_ if a_ is not None else c_)
        familias_en.append(b_ if b_ is not None else (c_ or ''))

out = ['/* GENERADO por tools/bandas_dump.py desde User/main.c - no editar. */',
       '#ifndef BANDAS_TABLA_H', '#define BANDAS_TABLA_H', '',
       '/* lo, hi y paso, al FINAL para no mover nada de lo que ya habia: los',
       ' * bordes de la banda (para saber en cual cae una frecuencia, la mas',
       ' * estrecha gana, como banda_indice() de main.c) y el indice de su paso',
       ' * en TUNE_STEPS_LISTA. Los usa el port del Malahit. */',
       'typedef struct { const char *nombre, *nombre_en, *rango, *modo; unsigned long hz; int familia; unsigned long lo, hi; int paso; } banda_t;', '',
       'static const banda_t banda[] = {']
for n, n_en, r, hz, md, fm, lo, hi, ps in filas:
    out.append('    { "%s", "%s", "%s", "%s", %dUL, %d, %dUL, %dUL, %d },'
               % (n, n_en, r, md, hz, fm, lo, hi, ps))
out.append('};')
out.append('#define N_BANDAS ((int)(sizeof banda / sizeof banda[0]))')
out.append('')
out.append('/* TUNE_STEPS_LISTA de main.c, entera. paso_tabla[banda[k].paso] es el')
out.append(' * paso de la banda k. Sin usar en quien solo quiere las bandas: unused. */')
out.append('typedef struct { unsigned long hz; const char *rotulo, *desc, *desc_en; } paso_t;')
out.append('__attribute__((unused)) static const paso_t paso_tabla[] = {')
for hz, et, d, d_en in pasos:
    out.append('    { %sUL, "%s", "%s", "%s" },' % (hz, et, d, d_en))
out.append('};')
out.append('#define N_PASOS ((int)(sizeof paso_tabla / sizeof paso_tabla[0]))')
out.append('')
out.append('static const char *const familia_nombre[] = { %s };'
           % ', '.join('"%s"' % f for f in familias))
out.append('static const char *const familia_nombre_en[] = { %s };'
           % ', '.join('"%s"' % f for f in familias_en))
out.append('')
out.append('#endif')
pathlib.Path(sys.argv[2]).write_text('\n'.join(out) + '\n', encoding='utf-8')
print('%d bandas, %d familias' % (len(filas), len(familias)))
