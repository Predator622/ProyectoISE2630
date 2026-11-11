/*******************************************************************************
 * Pontificia Universidad Javeriana
 * Estudiante: Juan David Herrera Henao
 * Materia: Sistemas Operativos 2026-30
 * Proyecto: Indicador de Sentimiento Economico
 * Fichero: ise.h
 * Tema: Interfaz de estructuras y funciones compartidas
 ******************************************************************************/

#ifndef ISE_H
#define ISE_H
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <signal.h>
#include <limits.h>

#define SECTORES 5
#define PAISES 3
#define MAX_AGENTES 4
#define RUTA_RESPUESTA 160
#define FIRMA 0x49534531u
extern const char *const paises[PAISES];
extern const char *const sectores[SECTORES];
extern const int maximos[PAISES][SECTORES];
extern const int pesos[SECTORES];
extern volatile sig_atomic_t cancelado;
typedef struct datosEncuesta {
    long id;
    int pais;
    int sector;
    int sentimiento; // Sentimiento en centesimas: 50% = 5000.
} Encuesta;
// CONEXION registra el agente; LECTURA transporta una encuesta; FIN termina su envio.
// SALIR solo se usa dentro del servidor para cerrar sus trabajadores.
enum Tipo { CONEXION = 1, LECTURA = 2, FIN = 3, SALIR = 4 };
typedef struct mensajeISE {
    uint32_t firma; // Identifica el protocolo ISE; no es una firma criptografica.
    int tipo;
    pid_t pid; // PID del proceso agente, usado para localizar su buffer.
    int pais;
    size_t total; // Cantidad de encuestas anunciada al conectar.
    Encuesta encuesta;
    char respuesta[RUTA_RESPUESTA]; // Ruta del FIFO privado para las confirmaciones.
} Mensaje;
typedef struct respuestaISE {
    int estado; // Cero indica exito; otro valor indica rechazo o envio incompleto.
    char texto[120];
} Respuesta;
// Cada estructura enviada cabe en la escritura atomica minima garantizada por POSIX.
_Static_assert(sizeof(Mensaje) <= _POSIX_PIPE_BUF, "Mensaje debe caber en escritura atomica POSIX");
_Static_assert(sizeof(Respuesta) <= _POSIX_PIPE_BUF,
               "Respuesta debe caber en escritura atomica POSIX");
typedef struct datosEstadisticas {
    uint64_t cantidad[SECTORES]; // Numero de encuestas de cada sector.
    uint64_t suma[SECTORES];     // Suma de sentimientos en centesimas.
} Estadisticas;

// Muestra el error de una llamada al sistema y termina el programa.
_Noreturn void errorSistema(const char *contexto);
// Las funciones pthread devuelven el codigo de error, no lo dejan en errno.
void validarHilo(int codigo, const char *contexto);
// Valida un argumento numerico; termina si el dato no es aceptable.
long validarEntero(const char *texto, long minimo, long maximo, const char *nombre);
// Busca el codigo del pais dentro del arreglo ES, FR e IT.
int buscarPais(const char *texto);
// Consulta un reloj que no cambia cuando se ajusta la hora del equipo.
double obtenerTiempo(void);
// Espera lectura o escritura disponible hasta el tiempo limite.
int esperarDescriptor(int descriptor, short eventos, double limite);
// Envia todo el mensaje en una escritura para conservar la atomicidad del FIFO.
int enviarMensaje(int descriptor, const void *datos, size_t cantidadBytes, double limite);
// Reune lecturas parciales hasta completar la cantidad de bytes solicitada.
int recibirMensaje(int descriptor, void *datos, size_t cantidadBytes, double limite);
// Bloquea el archivo .lock que comparten el generador y el agente lector.
int bloquearArchivo(const char *ruta, short tipo);
// Cierra un descriptor valido y avisa si el cierre falla.
void cerrarDescriptor(int descriptor);
// Lee, valida y guarda las encuestas en un arreglo de memoria dinamica.
Encuesta *cargarEncuestas(const char *ruta, size_t *cantidad, int *pais);
// Actualiza el contador y la suma del sector de una encuesta.
void sumarEncuesta(Estadisticas *estadisticas, const Encuesta *encuesta);
// Imprime las medias por sector y calcula el ISE con los cinco pesos.
void impReporte(const char *titulo, const Estadisticas *estadisticas);
#endif
