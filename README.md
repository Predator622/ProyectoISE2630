# Indicador de Sentimiento Económico

Juan David Herrera Henao · Sistemas Operativos 2026-30

El servidor recibe encuestas de España, Francia e Italia y calcula el indicador a partir de los promedios de cinco sectores. El generador crea los CSV y cada agente envía sus encuestas por un FIFO. El recolector reparte las lecturas entre los búferes de los agentes y cada hilo procesa las de su agente.

## Compilar y probar en Linux

Usa GCC y Make para compilar. Las pruebas requieren Python 3.

```sh
make
make test
```

## Demostración

En la primera terminal, desde la carpeta del proyecto:

```sh
./generadorCSV -p ES -e 1
./servidorISE -b 2 -p TORVALDS -t 60 -a 1
```

En otra terminal, desde la misma carpeta:

```sh
./agentePais -n encuestas_ES.csv -p TORVALDS
```

Al terminar el agente, el servidor muestra su reporte parcial. Al vencer el plazo, publica el resumen general. El ISE pondera industria (40 %), servicios (30 %), consumidores (20 %), comercio (5 %) y construcción (5 %). Si falta un sector, el reporte indica que el cálculo está incompleto.

Si un búfer se llena, el recolector espera a que el consumidor libere un espacio. Cuando entra una lectura, el semáforo de registros ocupados avisa al consumidor. El mutex protege los índices del búfer. Las pruebas incluyen cuatro agentes concurrentes con un búfer de un registro, CSV inválidos y envíos que quedan incompletos al vencer el plazo.
