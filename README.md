# Indicador de Sentimiento Económico

**Juan David Herrera Henao · Sistemas Operativos 2026-30**

El proyecto recibe encuestas de España, Francia e Italia y calcula un resultado por agente y otro general. Para explicar cómo funciona, sigo una encuesta desde el CSV hasta el reporte. En ese recorrido se ven dos temas del curso: la comunicación entre procesos y la coordinación entre hilos.

- [Organización del código](#organización-del-código)
- [Cómo viaja una encuesta](#cómo-viaja-una-encuesta)
- [Cálculo del ISE](#cálculo-del-ise)
- [Demostración en Linux](#demostración-en-linux)
- [Pruebas](#pruebas)
- [Conclusiones y recomendaciones](#conclusiones-y-recomendaciones)

## Organización del código

| Archivo | Qué hace |
| --- | --- |
| [Generador](src/generador.c) | Crea el CSV con las encuestas de un país. |
| [Agente](src/agente.c) | Valida el CSV, se conecta y envía las encuestas. |
| [Servidor](src/servidor.c) | Recibe los mensajes, reparte el trabajo y presenta los resultados. |
| [Funciones comunes](src/comun.c) | Manejan la carga del CSV, el envío de mensajes y el cálculo del reporte. |
| [Estructuras](include/ise.h) | Definen la encuesta y los mensajes que comparten los programas. |

## Cómo viaja una encuesta

```mermaid
flowchart LR
    G[Generador] --> C[CSV]
    C --> A[Agente]
    A --> F[FIFO]
    F --> R[Recolector]
    R --> B[Búfer por agente]
    B --> T[Trabajador]
    T --> I[Reporte ISE]
```

### Del CSV al servidor

El generador escribe en un archivo temporal y publica el CSV terminado con [rename](src/generador.c#L146). El generador y el agente se coordinan con el mismo archivo de bloqueo. La escritura toma un bloqueo exclusivo y la lectura toma uno compartido.

El agente [carga y valida el CSV](src/agente.c#L138). Primero anuncia la conexión, después [envía un mensaje LECTURA por encuesta](src/agente.c#L178) y al final manda FIN. Cada mensaje cabe en PIPE_BUF y se envía con una sola escritura para evitar que se intercale con el de otro agente. El receptor acumula bytes cuando una lectura devuelve una parte del mensaje.

### Del recolector al trabajador

El FIFO comunica al agente con el servidor. Dentro del servidor, el recolector encola cada lectura en el búfer del agente correspondiente. El trabajador retira el mensaje y actualiza la suma y la cantidad de encuestas del sector.

- **Libres** cuenta los espacios disponibles. El recolector [espera un espacio](src/servidor.c#L138) antes de insertar.
- **Ocupados** cuenta los registros disponibles. El trabajador [espera un registro](src/servidor.c#L161) y, cuando lo retira, devuelve un cupo.
- **Mutex** protege la copia del registro y el movimiento de los índices del búfer circular.

Con un búfer de uno, si ya hay un mensaje pendiente, el recolector tiene que esperar a que el trabajador lo retire. Esa espera puede retrasar la atención a otros agentes.

### Cómo termina

FIN avisa que el agente acabó de enviar y permite obtener su reporte parcial. Al vencer el plazo, el servidor deja de recibir, [encola SALIR](src/servidor.c#L499) detrás del trabajo pendiente y espera a los trabajadores con pthread_join. Después junta las estadísticas para el reporte general.

## Cálculo del ISE

En la [función de reporte](src/comun.c#L399) se divide la suma de cada sector por su cantidad de encuestas y se multiplica ese promedio por el peso. En el resultado general se reúnen primero las sumas y cantidades de los agentes por sector. Así cada encuesta cuenta una vez, aunque unos agentes envíen más datos que otros. Si falta algún sector, el resultado queda incompleto y no se emite recomendación.

| Sector | Peso | Promedio de la demo | Aporte |
| --- | ---: | ---: | ---: |
| Industria | 40 % | 100 | 40 |
| Servicios | 30 % | 0 | 0 |
| Consumidores | 20 % | 50 | 10 |
| Comercio | 5 % | 100 | 5 |
| Construcción | 5 % | 0 | 0 |
| **ISE** | **100 %** | | **55 %** |

Por encima de 50 se muestra **Vender**, por debajo se muestra **Comprar** y en 50 se muestra **Neutro**.

## Demostración en Linux

Compilo con GCC y Make. Para ejecutar las pruebas de integración también necesito Python 3.

```sh
make
make test
```

Para repetir la demostración, preparo en el directorio del proyecto un CSV con una encuesta por sector.

```sh
cat > encuestas_demo.csv <<'EOF'
ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento
1,0,ES,Industria,0,100.00
2,0,ES,Servicios,1,0.00
3,0,ES,Consumidores,2,50.00
4,0,ES,Comercio,3,100.00
5,0,ES,Construccion,4,0.00
EOF
```

En la primera terminal arranco el servidor con un búfer de un registro, 30 segundos y un cupo de agente.

```sh
./servidorISE -b 1 -p TORVALDS -t 30 -a 1
```

Cuando aparece ISE listo, paso a la segunda terminal en el mismo directorio.

```sh
ls -l TORVALDS
ps -T -C servidorISE
./agentePais -n encuestas_demo.csv -p TORVALDS
```

La letra p en los permisos de TORVALDS indica que es un FIFO. El nombre puede cambiar, siempre que los dos programas usen la misma ruta con -p. Con ps -T se ven el hilo principal, el trabajador, el recolector y el temporizador. Al enviar el CSV se espera ver cinco encuestas procesadas y **55 % · Vender**. El resumen general aparece al cerrar el servidor.

Para probar datos generados, uso el comando siguiente y luego paso encuestas_ES.csv al agente. El indicador cambia según los valores que salgan.

```sh
./generadorCSV -p ES -e 1
```

## Pruebas

| Caso | Qué comprobé |
| --- | --- |
| Cinco encuestas con resultado conocido | El servidor procesa las cinco y el índice coincide con el cálculo a mano. |
| Cuatro agentes y búfer de uno | El caso de integración procesa 56.325 encuestas y compara el resultado general con un cálculo independiente. |
| CSV inválido | El agente detecta datos incorrectos antes del envío. |
| Sector sin encuestas | El reporte indica que el ISE está incompleto. |
| Vencimiento del plazo | El servidor cierra la recepción y distingue los envíos incompletos. |

El código de las pruebas está en [tests/integracion.py](tests/integracion.py).

## Conclusiones y recomendaciones

La prueba de cinco filas me permitió seguir una encuesta desde el CSV y comprobar el resultado sin depender de valores aleatorios. La prueba de cuatro agentes sirvió para revisar el procesamiento concurrente. Con el búfer de uno se ve bien la relación entre los dos semáforos: cada mensaje que entra usa un espacio libre y cada mensaje que sale lo devuelve.

Para repetir la demostración, empezaría por el CSV de cinco filas y comprobaría tanto el conteo como el porcentaje. Luego cambiaría el tamaño del búfer con los mismos datos para verificar que el resultado se conserva. Para comparar rendimiento haría varias corridas con los mismos archivos y la misma cantidad de agentes. Mediría el tiempo total y cuánto dura la espera del recolector cuando una cola se llena, porque esa espera puede retrasar otros mensajes.
