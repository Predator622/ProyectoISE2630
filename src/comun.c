/*******************************************************************************
 * Pontificia Universidad Javeriana
 * Estudiante: Juan David Herrera Henao
 * Materia: Sistemas Operativos 2026-30
 * Proyecto: Indicador de Sentimiento Economico
 * Fichero: comun.c
 * Tema: Validacion de datos, ficheros, comunicacion y calculo del ISE
 ******************************************************************************/

#include "ise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>

// Tablas comunes: el mismo orden se usa en el CSV, en los mensajes y en los calculos.
const char *const paises[PAISES] = {
    "ES", // ID 0: Espana.
    "FR", // ID 1: Francia.
    "IT", // ID 2: Italia.
};
const char *const sectores[SECTORES] = {
    "Industria",    // ID 0.
    "Servicios",    // ID 1.
    "Consumidores", // ID 2.
    "Comercio",     // ID 3.
    "Construccion", // ID 4.
};
const int maximos[PAISES][SECTORES] = {
    {1760, 1430, 2020, 1080, 290},   // Espana.
    {3850, 4240, 1670, 2860, 24020}, // Francia.
    {5440, 3100, 2000, 1560, 1000},  // Italia.
};
const int pesos[SECTORES] = {40, 30, 20, 5, 5};
volatile sig_atomic_t cancelado = 0;

// Muestra el error de una llamada al sistema y termina el programa.
_Noreturn void errorSistema(const char *contexto) {
    perror(contexto);
    exit(EXIT_FAILURE);
}

// Las funciones pthread devuelven el codigo de error, no lo dejan en errno.
void validarHilo(int codigo, const char *contexto) {
    if (codigo) {
        errno = codigo;
        errorSistema(contexto);
    }
}

// Convierte el texto y comprueba que sea un entero dentro del rango.
static int convertirEntero(const char *texto, long minimo, long maximo, long *resultado) {
    char *fin;
    errno = 0;
    long valorEntero = strtol(texto, &fin, 10);
    if (errno || fin == texto || *fin || valorEntero < minimo || valorEntero > maximo) {
        return -1;
    }
    *resultado = valorEntero;
    return 0;
}

// Valida un argumento numerico; termina si el dato no es aceptable.
long validarEntero(const char *texto, long minimo, long maximo, const char *nombre) {
    long valorEntero;
    if (convertirEntero(texto, minimo, maximo, &valorEntero) == -1) {
        fprintf(stderr, "%s invalido: %s (rango %ld..%ld)\n", nombre, texto, minimo, maximo);
        exit(EXIT_FAILURE);
    }
    return valorEntero;
}

// Busca el codigo del pais dentro del arreglo ES, FR e IT.
int buscarPais(const char *texto) {
    for (int i = 0; i < PAISES; i++) {
        if (!strcmp(texto, paises[i])) {
            return i;
        }
    }
    return -1;
}

// Consulta un reloj que no cambia cuando se ajusta la hora del equipo.
double obtenerTiempo(void) {
    struct timespec tiempoActual;
    if (clock_gettime(CLOCK_MONOTONIC, &tiempoActual) == -1) {
        errorSistema("clock_gettime");
    }
    return tiempoActual.tv_sec + tiempoActual.tv_nsec / 1e9;
}

// Espera lectura o escritura disponible hasta el tiempo limite.
int esperarDescriptor(int descriptor, short eventos, double limite) {
    for (;;) {
        if (cancelado) {
            errno = EINTR;
            return -1;
        }
        double restante = limite - obtenerTiempo();
        if (restante <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd consulta = {.fd = descriptor, .events = eventos};
        int milisegundos;
        if (restante > 0.1) {
            milisegundos = 100;
        } else {
            milisegundos = (int)(restante * 1000) + 1;
        }
        int resultado = poll(&consulta, 1, milisegundos);
        if (resultado < 0 && errno == EINTR) {
            continue;
        }
        if (resultado < 0) {
            return -1;
        }
        if (consulta.revents & eventos) {
            return 0;
        }
        if (consulta.revents & (POLLERR | POLLNVAL)) {
            errno = EPIPE;
            return -1;
        }
        if (consulta.revents & POLLHUP) {
            errno = EPIPE;
            return -1;
        }
    }
}

// Envia todo el mensaje en una escritura para conservar la atomicidad del FIFO.
int enviarMensaje(int descriptor, const void *datos, size_t cantidadBytes, double limite) {
    /* Escribe cada mensaje completo para evitar que los agentes mezclen sus bytes. */
    if (cantidadBytes > PIPE_BUF) {
        errno = EMSGSIZE;
        return -1;
    }
    for (;;) {
        if (cancelado) {
            errno = EINTR;
            return -1;
        }
        if (obtenerTiempo() >= limite) {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t resultado = write(descriptor, datos, cantidadBytes);
        if (resultado == (ssize_t)cantidadBytes) {
            return 0;
        }
        if (resultado < 0 && errno == EINTR) {
            continue;
        }
        if (resultado < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (esperarDescriptor(descriptor, POLLOUT, limite) == -1) {
                return -1;
            }
            continue;
        }
        if (resultado >= 0) {
            errno = EIO;
        }
        return -1;
    }
}

// Reune lecturas parciales hasta completar la cantidad de bytes solicitada.
int recibirMensaje(int descriptor, void *datos, size_t cantidadBytes, double limite) {
    size_t recibidos = 0;
    while (recibidos < cantidadBytes) {
        if (esperarDescriptor(descriptor, POLLIN, limite) == -1) {
            return -1;
        }
        ssize_t resultado = read(descriptor, (char *)datos + recibidos, cantidadBytes - recibidos);
        if (resultado > 0) {
            recibidos += (size_t)resultado;
        } else if (resultado < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        } else {
            if (!resultado) {
                errno = EPIPE;
            }
            return -1;
        }
    }
    return 0;
}

// Cierra un descriptor valido y avisa si el cierre falla.
void cerrarDescriptor(int descriptor) {
    if (descriptor >= 0 && close(descriptor) == -1) {
        perror("close");
    }
}

// Bloquea el archivo .lock que comparten el generador y el agente lector.
int bloquearArchivo(const char *ruta, short tipo) {
    char nombre[4096];
    if (snprintf(nombre, sizeof nombre, "%s.lock", ruta) >= (int)sizeof nombre) {
        errno = ENAMETOOLONG;
        errorSistema("ruta lock");
    }
    int descriptor = open(nombre, O_CREAT | O_RDWR, 0600);
    if (descriptor == -1) {
        errorSistema("open lock");
    }
    struct flock bloqueo = {.l_type = tipo, .l_whence = SEEK_SET};
    while (fcntl(descriptor, F_SETLKW, &bloqueo) == -1) {
        if (errno != EINTR || cancelado) {
            errorSistema("fcntl lock");
        }
    }
    /* El archivo .lock se conserva; cerrar el descriptor libera el bloqueo. */
    return descriptor;
}

// Indica el numero de linea que contiene un error en el CSV.
static _Noreturn void errorCSV(size_t linea, const char *detalle) {
    fprintf(stderr, "CSV linea %zu: %s\n", linea, detalle);
    exit(EXIT_FAILURE);
}

// Valida un campo entero del CSV y conserva su numero de linea para el error.
static long validarEnteroCSV(const char *texto, long minimo, long maximo, size_t linea,
                             const char *nombre) {
    long valorEntero;
    if (convertirEntero(texto, minimo, maximo, &valorEntero) == -1) {
        fprintf(stderr, "CSV linea %zu: %s invalido: %s (rango %ld..%ld)\n", linea, nombre, texto,
                minimo, maximo);
        exit(EXIT_FAILURE);
    }
    return valorEntero;
}

// Quita espacios al inicio y al final de un campo sin copiarlo.
static char *quitarEspacios(char *texto) {
    while (isspace((unsigned char)*texto)) {
        texto++;
    }
    char *fin = texto + strlen(texto);
    while (fin > texto && isspace((unsigned char)fin[-1])) {
        fin--;
        *fin = '\0';
    }
    return texto;
}

// Comprueba los seis campos, el pais, el sector, el sentimiento y sus limites.
static Encuesta validarRegistro(char *campos[6], size_t numeroLinea, int *pais,
                                int conteos[SECTORES]) {
    Encuesta encuesta = {0};
    encuesta.id = validarEnteroCSV(campos[0], 1, LONG_MAX, numeroLinea, "ID_Encuesta");
    encuesta.pais = buscarPais(campos[2]);
    if (encuesta.pais < 0) {
        errorCSV(numeroLinea, "pais desconocido");
    }
    if (validarEnteroCSV(campos[1], 0, 2, numeroLinea, "ID_Pais") != encuesta.pais) {
        errorCSV(numeroLinea, "ID_Pais debe ser ES=0, FR=1, IT=2");
    }
    if (*pais == -1) {
        *pais = encuesta.pais;
    }
    if (*pais != encuesta.pais) {
        errorCSV(numeroLinea, "paises mezclados");
    }
    encuesta.sector = (int)validarEnteroCSV(campos[4], 0, 4, numeroLinea, "ID_Sector");
    if (strcmp(campos[3], sectores[encuesta.sector])) {
        errorCSV(numeroLinea, "sector e ID no coinciden");
    }
    char *fin;
    errno = 0;
    double valor = strtod(campos[5], &fin);
    if (errno || fin == campos[5] || *fin || !isfinite(valor) || valor < 0 || valor > 100) {
        errorCSV(numeroLinea, "sentimiento fuera de 0..100");
    }
    encuesta.sentimiento = (int)lround(valor * 100);
    conteos[encuesta.sector]++;
    if (conteos[encuesta.sector] > maximos[encuesta.pais][encuesta.sector]) {
        errorCSV(numeroLinea, "se supera el maximo del sector");
    }
    return encuesta;
}

// Lee, valida y guarda las encuestas en un arreglo de memoria dinamica.
Encuesta *cargarEncuestas(const char *ruta, size_t *cantidad, int *pais) {
    int descriptorBloqueo = bloquearArchivo(ruta, F_RDLCK);
    FILE *archivoCSV = fopen(ruta, "r");
    if (!archivoCSV) {
        errorSistema("fopen CSV");
    }
    char *linea = NULL;
    size_t espacioLinea = 0;
    size_t numeroLinea = 0;
    size_t totalEncuestas = 0;
    size_t capacidad = 128;
    Encuesta *datos = malloc(capacidad * sizeof(Encuesta));
    if (!datos) {
        errorSistema("malloc encuestas");
    }
    int conteos[SECTORES] = {0};
    *pais = -1;
    ssize_t longitud;
    while ((longitud = getline(&linea, &espacioLinea, archivoCSV)) != -1) {
        numeroLinea++;
        if (cancelado) {
            errno = EINTR;
            errorSistema("lectura CSV interrumpida");
        }
        if (memchr(linea, '\0', (size_t)longitud)) {
            errorCSV(numeroLinea, "caracter NUL oculto");
        }
        if (longitud && linea[longitud - 1] == '\n') {
            longitud--;
            linea[longitud] = '\0';
        }
        if (longitud && linea[longitud - 1] == '\r') {
            longitud--;
            linea[longitud] = '\0';
        }
        if (strchr(linea, '\r')) {
            errorCSV(numeroLinea, "retorno de carro dentro de un registro");
        }
        if (numeroLinea == 1 && longitud >= 3 && !memcmp(linea, "\xef\xbb\xbf", 3)) {
            // Copia tambien el terminador: longitud - 3 bytes de texto y un byte NUL.
            memmove(linea, linea + 3, (size_t)longitud - 2);
            longitud -= 3;
        }
        if (numeroLinea == 1 &&
            !strcmp(linea, "ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento")) {
            continue;
        }
        char *campos[6];
        char *cursor = linea;
        for (int i = 0; i < 6; i++) {
            campos[i] = cursor;
            char *coma = strchr(cursor, ',');
            if (i < 5) {
                if (!coma) {
                    errorCSV(numeroLinea, "se requieren seis campos");
                }
                *coma = '\0';
                cursor = coma + 1;
            } else if (coma) {
                errorCSV(numeroLinea, "sobran campos");
            }
        }
        for (int i = 0; i < 6; i++) {
            campos[i] = quitarEspacios(campos[i]);
        }
        if (numeroLinea == 1 && !strcmp(campos[0], "ID_Encuesta") &&
            !strcmp(campos[1], "ID_Pais") && !strcmp(campos[2], "Pais") &&
            !strcmp(campos[3], "Sector") && !strcmp(campos[4], "ID_Sector") &&
            !strcmp(campos[5], "Sentimiento")) {
            continue;
        }
        Encuesta encuesta = validarRegistro(campos, numeroLinea, pais, conteos);
        if (totalEncuestas == capacidad) {
            // Duplica la capacidad del arreglo cuando se llena.
            capacidad *= 2;
            Encuesta *nuevo = realloc(datos, capacidad * sizeof(Encuesta));
            if (!nuevo) {
                errorSistema("realloc encuestas");
            }
            datos = nuevo;
        }
        datos[totalEncuestas] = encuesta;
        totalEncuestas++;
    }
    if (ferror(archivoCSV)) {
        errorSistema("leer CSV");
    }
    free(linea);
    if (fclose(archivoCSV) == EOF) {
        errorSistema("fclose CSV");
    }
    cerrarDescriptor(descriptorBloqueo);
    if (!totalEncuestas) {
        errorCSV(numeroLinea, "no contiene encuestas");
    }
    *cantidad = totalEncuestas;
    return datos;
}

// Actualiza el contador y la suma del sector de una encuesta.
void sumarEncuesta(Estadisticas *estadisticas, const Encuesta *encuesta) {
    estadisticas->cantidad[encuesta->sector]++;
    estadisticas->suma[encuesta->sector] += (uint64_t)encuesta->sentimiento;
}

// Imprime las medias por sector y calcula el ISE con los cinco pesos.
void impReporte(const char *titulo, const Estadisticas *estadisticas) {
    double ise = 0;
    int completo = 1;
    uint64_t total = 0;
    printf("\n=== %s ===\n", titulo);
    for (int i = 0; i < SECTORES; i++) {
        total += estadisticas->cantidad[i];
        if (!estadisticas->cantidad[i]) {
            completo = 0;
            printf("%-13s: SIN DATOS (peso %d%%)\n", sectores[i], pesos[i]);
        } else {
            // La suma guarda centesimas; dividir por 100 recupera el porcentaje.
            double media = (double)estadisticas->suma[i] / estadisticas->cantidad[i] / 100.0;
            double aporte = media * pesos[i] / 100.0;
            ise += aporte;
            printf("%-13s: %llu encuestas, media %.4f%%, peso %d%%, aporte %.4f puntos\n",
                   sectores[i], (unsigned long long)estadisticas->cantidad[i], media, pesos[i],
                   aporte);
        }
    }
    printf("Total procesado: %llu\n", (unsigned long long)total);
    if (!completo) {
        puts("ISE INCOMPLETO: faltan sectores; no se calcula la recomendacion.");
        return;
    }
    puts("Calculo: ISE = suma de (media del sector x peso / 100).");
    const char *decision;
    const char *color;
    const char *restaurarColor;

    // Se admite una tolerancia pequena por las operaciones con double.
    if (ise > 50.0 + 1e-9) {
        decision = "Vender";
        color = "\033[32m";
    } else if (ise < 50.0 - 1e-9) {
        decision = "Comprar";
        color = "\033[31m";
    } else {
        decision = "Neutro";
        color = "\033[33m";
    }

    // Al guardar la salida en un archivo no se escriben los codigos de color.
    restaurarColor = "\033[0m";
    if (!isatty(STDOUT_FILENO)) {
        color = "";
        restaurarColor = "";
    }
    printf("ISE = %.4f%% | %s%s%s\n", ise, color, decision, restaurarColor);
}
