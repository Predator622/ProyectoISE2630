/*******************************************************************************
 * Pontificia Universidad Javeriana
 * Estudiante: Juan David Herrera Henao
 * Materia: Sistemas Operativos 2026-30
 * Proyecto: Indicador de Sentimiento Economico
 * Fichero: generador.c
 * Tema: Generacion de encuestas por pais y sector
 ******************************************************************************/

#include "ise.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <signal.h>

static char temporal[80];
static int temporalCreado;
// Elimina los recursos temporales propios al terminar el proceso.
static void limpiarRecursos(void) {
    if (temporalCreado && unlink(temporal) == -1 && errno != ENOENT) {
        perror("unlink CSV temporal");
    }
}

// El manejador solo marca la interrupcion; el flujo principal hace la limpieza.
static void marcarInterrupcion(int numero) {
    (void)numero;
    cancelado = 1;
}

// Genera un sentimiento en centesimas: desde 0.00 hasta 100.00 por ciento.
static int generarSentimiento(void) {
    unsigned azar;
    unsigned rango = (unsigned)RAND_MAX + 1u;
    unsigned limiteAceptado = rango - rango % 10001u;

    // Descarta el sobrante para que todos los valores de 0 a 10000 tengan igual probabilidad.
    do {
        azar = (unsigned)rand();
    } while (azar >= limiteAceptado);
    return (int)(azar % 10001u);
}

// Funcion de ingreso: recibe y valida las opciones de la linea de comandos.
static int funcIngreso(int argc, char *argv[], int *pais, int *porcentaje) {
    int opcion;
    // Ingreso y validacion de los argumentos de ejecucion.
    while ((opcion = getopt(argc, argv, "p:e:")) != -1) {
        switch (opcion) {
            case 'p':
                *pais = buscarPais(optarg);
                break;
            case 'e':
                *porcentaje = (int)validarEntero(optarg, 1, 100, "porcentaje");
                break;
            default:
                return -1;
        }
    }
    if (*pais < 0 || *porcentaje < 0 || optind != argc) {
        fprintf(stderr, "Uso: %s -p ES|FR|IT -e 1..100\n", argv[0]);
        return -1;
    }
    return 0;
}

// Funcion principal: ingreso, preparacion, trabajo y liberacion de recursos.
int main(int argc, char *argv[]) {
    int pais = -1;
    int porcentaje = -1;
    if (funcIngreso(argc, argv, &pais, &porcentaje) == -1) {
        return EXIT_FAILURE;
    }

    struct sigaction accion = {.sa_handler = marcarInterrupcion};
    if (sigemptyset(&accion.sa_mask) == -1 || sigaction(SIGINT, &accion, NULL) == -1 ||
        sigaction(SIGTERM, &accion, NULL) == -1) {
        errorSistema("sigaction generador");
    }
    if (atexit(limpiarRecursos) != 0) {
        errno = ENOMEM;
        errorSistema("atexit generador");
    }
    // Preparacion del nombre del CSV y del archivo temporal.
    char ruta[64];
    snprintf(ruta, sizeof ruta, "encuestas_%s.csv", paises[pais]);
    snprintf(temporal, sizeof temporal, "%s.tmp.XXXXXX", ruta);
    // Bloqueo exclusivo: ningun lector cooperante ve un CSV a medio escribir.
    int descriptorBloqueo = bloquearArchivo(ruta, F_WRLCK);
    int descriptorCSV = mkstemp(temporal);
    if (descriptorCSV < 0) {
        errorSistema("mkstemp");
    }
    temporalCreado = 1;
    FILE *archivoCSV = fdopen(descriptorCSV, "w");
    if (!archivoCSV) {
        errorSistema("fdopen");
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    int correcto =
        fprintf(archivoCSV, "ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento\n") >= 0;
    long idEncuesta = 0;
    printf("Generando %s para %s con %d%%\n", ruta, paises[pais], porcentaje);
    // Generacion de las encuestas de cada uno de los cinco sectores.
    for (int sector = 0; sector < SECTORES; sector++) {
        // Se redondea la cantidad al entero mas cercano.
        int cantidad = (maximos[pais][sector] * porcentaje + 50) / 100;
        printf("%-13s: %d/%d\n", sectores[sector], cantidad, maximos[pais][sector]);
        for (int i = 0; i < cantidad; i++) {
            if (cancelado) {
                errno = EINTR;
                errorSistema("generacion interrumpida");
            }
            int sentimiento = generarSentimiento();
            idEncuesta++;
            if (fprintf(archivoCSV, "%ld,%d,%s,%s,%d,%d.%02d\n", idEncuesta, pais, paises[pais],
                        sectores[sector], sector, sentimiento / 100, sentimiento % 100) < 0) {
                correcto = 0;
            }
        }
    }
    // Vaciar los buffers y sincronizar el archivo antes de reemplazar el CSV.
    if (fflush(archivoCSV) == EOF) {
        correcto = 0;
    }
    if (correcto && fsync(descriptorCSV) == -1) {
        correcto = 0;
    }
    if (fclose(archivoCSV) == EOF) {
        correcto = 0;
    }
    if (!correcto) {
        errno = EIO;
        errorSistema("escribir CSV");
    }
    if (cancelado) {
        errno = EINTR;
        errorSistema("generacion interrumpida");
    }
    // rename publica el archivo terminado mientras el bloqueo sigue tomado.
    if (rename(temporal, ruta) == -1) {
        errorSistema("rename CSV");
    }
    temporalCreado = 0;
    cerrarDescriptor(descriptorBloqueo);
    printf("Fichero generado: %ld encuestas.\n", idEncuesta);
    return EXIT_SUCCESS;
}
