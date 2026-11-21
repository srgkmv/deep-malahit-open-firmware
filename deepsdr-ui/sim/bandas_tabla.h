/* GENERADO por tools/bandas_dump.py desde User/main.c - no editar. */
#ifndef BANDAS_TABLA_H
#define BANDAS_TABLA_H

/* lo, hi y paso, al FINAL para no mover nada de lo que ya habia: los
 * bordes de la banda (para saber en cual cae una frecuencia, la mas
 * estrecha gana, como banda_indice() de main.c) y el indice de su paso
 * en TUNE_STEPS_LISTA. Los usa el port del Malahit. */
typedef struct { const char *nombre, *nombre_en, *rango, *modo; unsigned long hz; int familia; unsigned long lo, hi; int paso; } banda_t;

static const banda_t banda[] = {
    { "2200 m", "2200 m", "135,7-137,8 kHz", "USB", 136500UL, 0, 135700UL, 137800UL, 1 },
    { "630 m", "630 m", "472-479 kHz", "USB", 475500UL, 0, 472000UL, 479000UL, 1 },
    { "160 m", "160 m", "1.810-2.000 kHz", "LSB", 1840000UL, 0, 1810000UL, 2000000UL, 3 },
    { "80 m", "80 m", "3.500-3.800 kHz", "LSB", 3750000UL, 0, 3500000UL, 3800000UL, 3 },
    { "60 m", "60 m", "5.351-5.367 kHz", "USB", 5357000UL, 0, 5351000UL, 5367000UL, 3 },
    { "40 m", "40 m", "7.000-7.200 kHz", "LSB", 7150000UL, 0, 7000000UL, 7200000UL, 3 },
    { "30 m", "30 m", "10.100-10.150 kHz", "USB", 10120000UL, 0, 10100000UL, 10150000UL, 3 },
    { "20 m", "20 m", "14.000-14.350 kHz", "USB", 14250000UL, 0, 14000000UL, 14350000UL, 3 },
    { "17 m", "17 m", "18.068-18.168 kHz", "USB", 18130000UL, 0, 18068000UL, 18168000UL, 3 },
    { "15 m", "15 m", "21.000-21.450 kHz", "USB", 21250000UL, 0, 21000000UL, 21450000UL, 3 },
    { "12 m", "12 m", "24.890-24.990 kHz", "USB", 24950000UL, 0, 24890000UL, 24990000UL, 3 },
    { "10 m", "10 m", "28,0-29,7 MHz", "USB", 28400000UL, 0, 28000000UL, 29700000UL, 3 },
    { "6 m", "6 m", "50-52 MHz", "USB", 50150000UL, 0, 50000000UL, 52000000UL, 3 },
    { "2 m", "2 m", "144-146 MHz", "NFM", 145500000UL, 0, 144000000UL, 146000000UL, 7 },
    { "OL", "LW", "148-284 kHz", "AM", 198000UL, 1, 148000UL, 284000UL, 3 },
    { "OM", "MW", "526-1.606 kHz", "AM", 1000000UL, 1, 526000UL, 1606000UL, 3 },
    { "120 m", "120 m", "2.300-2.495 kHz", "AM", 2400000UL, 1, 2300000UL, 2495000UL, 4 },
    { "90 m", "90 m", "3.200-3.400 kHz", "AM", 3300000UL, 1, 3200000UL, 3400000UL, 4 },
    { "75 m", "75 m", "3.900-4.000 kHz", "AM", 3950000UL, 1, 3900000UL, 4000000UL, 4 },
    { "60 m", "60 m", "4.750-5.060 kHz", "AM", 4900000UL, 1, 4750000UL, 5060000UL, 4 },
    { "49 m", "49 m", "5.800-6.200 kHz", "AM", 6000000UL, 1, 5800000UL, 6200000UL, 4 },
    { "41 m", "41 m", "7.200-7.450 kHz", "AM", 7300000UL, 1, 7200000UL, 7450000UL, 4 },
    { "31 m", "31 m", "9.400-9.900 kHz", "AM", 9600000UL, 1, 9400000UL, 9900000UL, 4 },
    { "25 m", "25 m", "11.600-12.100 kHz", "AM", 11800000UL, 1, 11600000UL, 12100000UL, 4 },
    { "22 m", "22 m", "13.570-13.870 kHz", "AM", 13700000UL, 1, 13570000UL, 13870000UL, 4 },
    { "19 m", "19 m", "15.100-15.830 kHz", "AM", 15400000UL, 1, 15100000UL, 15830000UL, 4 },
    { "16 m", "16 m", "17.480-17.900 kHz", "AM", 17650000UL, 1, 17480000UL, 17900000UL, 4 },
    { "15 m", "15 m", "18.900-19.020 kHz", "AM", 18950000UL, 1, 18900000UL, 19020000UL, 4 },
    { "13 m", "13 m", "21.450-21.850 kHz", "AM", 21600000UL, 1, 21450000UL, 21850000UL, 4 },
    { "11 m", "11 m", "25.670-26.100 kHz", "AM", 25800000UL, 1, 25670000UL, 26100000UL, 4 },
    { "NDB", "NDB", "190-535 kHz", "USB", 350000UL, 2, 190000UL, 535000UL, 3 },
    { "NAVTEX", "NAVTEX", "518 kHz", "USB", 518000UL, 2, 518000UL, 518000UL, 1 },
    { "NAVTEX-N", "NAVTEX-N", "490 kHz", "USB", 490000UL, 2, 490000UL, 490000UL, 1 },
    { "NAVTEX-HF", "NAVTEX-HF", "4.209,5 kHz", "USB", 4209500UL, 2, 4209500UL, 4209500UL, 1 },
    { "FAX 4", "FAX 4", "DWD 3.853,1 kHz", "USB", 3853100UL, 2, 3853100UL, 3853100UL, 1 },
    { "FAX 8", "FAX 8", "DWD 7.878,1 kHz", "USB", 7878100UL, 2, 7878100UL, 7878100UL, 1 },
    { "FAX 14", "FAX 14", "DWD 13.880,6 kHz", "USB", 13880600UL, 2, 13880600UL, 13880600UL, 1 },
    { "SSTV 20", "SSTV 20", "14.230 kHz", "USB", 14230000UL, 2, 14230000UL, 14230000UL, 1 },
    { "SSTV 40", "SSTV 40", "7.165 kHz", "USB", 7165000UL, 2, 7165000UL, 7165000UL, 1 },
    { "APRS", "APRS", "144,800 MHz", "NFM", 144800000UL, 3, 144800000UL, 144800000UL, 4 },
    { "APRS 70", "APRS 70", "432,500 MHz", "NFM", 432500000UL, 3, 432500000UL, 432500000UL, 4 },
    { "Horaria", "Time", "2,5 / 5 / 10 MHz", "AM", 10000000UL, 2, 0UL, 0UL, 3 },
    { "CB", "CB", "26,965-27,405 MHz", "AM", 27185000UL, 2, 26965000UL, 27405000UL, 6 },
    { "FM OIRT", "FM OIRT", "65,8-74 MHz", "WFM", 66300000UL, 3, 65800000UL, 74000000UL, 6 },
    { "FM", "FM", "CCIR 87,5-108 MHz", "WFM", 100000000UL, 3, 87500000UL, 108000000UL, 9 },
    { "Aérea", "Air", "108-137 MHz", "AM", 118000000UL, 3, 108000000UL, 137000000UL, 8 },
    { "Meteo", "Weather", "137-138 MHz", "WFM", 137500000UL, 3, 137000000UL, 138000000UL, 7 },
    { "Marina", "Marine", "156-162 MHz", "NFM", 156800000UL, 3, 156000000UL, 162000000UL, 7 },
    { "VHF alta", "VHF high", "162-174 MHz", "NFM", 165000000UL, 3, 162000000UL, 174000000UL, 7 },
};
#define N_BANDAS ((int)(sizeof banda / sizeof banda[0]))

/* TUNE_STEPS_LISTA de main.c, entera. paso_tabla[banda[k].paso] es el
 * paso de la banda k. Sin usar en quien solo quiere las bandas: unused. */
typedef struct { unsigned long hz; const char *rotulo, *desc, *desc_en; } paso_t;
__attribute__((unused)) static const paso_t paso_tabla[] = {
    { 10UL, "10 Hz", "Batido y deriva", "Beat and drift" },
    { 100UL, "100 Hz", "Ajuste fino", "Fine tuning" },
    { 500UL, "500 Hz", "Buscar en SSB", "Searching in SSB" },
    { 1000UL, "1 kHz", "SSB", "SSB" },
    { 5000UL, "5 kHz", "Onda corta", "Shortwave" },
    { 9000UL, "9 kHz", "Onda media", "Mediumwave" },
    { 10000UL, "10 kHz", "CB y OM americana", "CB and MW in the Americas" },
    { 12500UL, "12,5 kHz", "Marina y 2 m", "Marine and 2 m" },
    { 25000UL, "25 kHz", "Aeronáutica", "Aeronautical" },
    { 100000UL, "100 kHz", "FM comercial", "Broadcast FM" },
    { 1000000UL, "1 MHz", "Saltos grandes", "Big jumps" },
};
#define N_PASOS ((int)(sizeof paso_tabla / sizeof paso_tabla[0]))

static const char *const familia_nombre[] = { "Aficionados", "Radiodifusión", "Utilidades", "VHF/UHF" };
static const char *const familia_nombre_en[] = { "Amateur", "Broadcast", "Utility", "VHF/UHF" };

#endif
