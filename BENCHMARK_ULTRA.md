# POLYLOG ULTRA: comparación con el C++ anterior

**C++17 en un archivo.** ULTRA conserva el grafo para componentes estrechos y añade propagación, componentes independientes, 2-SAT por SCC y CDCL. Las aceleraciones corresponden a este solver híbrido.

- Validación: 2,004 ejecuciones correctas sobre 962 fórmulas y pruebas adicionales de parser/timeout.
- AddressSanitizer + UndefinedBehaviorSanitizer: 182 ejecuciones adicionales superadas con aserciones activadas. LeakSanitizer se desactivó porque el entorno impide su inspección de procesos; no se afirma haber verificado fugas con LSan.
- Benchmark: 67 instancias, tres repeticiones por motor; 402 ejecuciones.
- ULTRA: 201/201 respuestas correctas; ningún UNKNOWN ni timeout.
- Anterior: 183/201 resueltas; 18 UNKNOWN por presupuesto explícito de frontera en los casos añadidos; ninguna respuesta definitiva incorrecta.

## Condiciones de medición

Fecha UTC: 2026-10-18T05:01:32.512885+00:00. CPU: AMD EPYC 9V74 80-Core Processor. Afinidad: [0]. Compilador: g++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0. Ambos compilados con `-O3 -DNDEBUG -std=c++17`.

Un proceso por vez, procesos nuevos, orden alternado y una ejecución pequeña de calentamiento por motor. Medianas de tiempo de pared interno, que incluyen lectura, preparación, búsqueda y verificación del certificado; excluyen arranque y serialización. Los CSV incluyen también el tiempo completo del proceso y la memoria residente.

Las 54 instancias de la entrega anterior se ejecutaron sin presupuesto de frontera en el motor antiguo. Las 13 nuevas usan explícitamente `--max-frontier 4096` en ese motor. ULTRA mantiene sus parámetros predeterminados: máximo de 4096 estados de nueva frontera y 262144 nodos por intento de grafo; al agotarlos sigue resolviendo por CDCL. El watchdog externo es de 15 segundos por proceso.

## Comparaciones con resultado completo en ambos motores

Para 3-SAT aleatorio: ocho semillas por tamaño y tres repeticiones (24 observaciones por celda). Las demás filas: un caso y tres repeticiones. La aceleración es el cociente de medianas; no representa una garantía para cualquier fórmula.

| Caso | Variables | C++ anterior (ms) | ULTRA (ms) | Cociente anterior/ULTRA |
|---|---:|---:|---:|---:|
| 3-SAT aleatorio | 12 | 0.1177 | 0.0934 | 1.26x |
| 3-SAT aleatorio | 18 | 0.4362 | 0.1180 | 3.70x |
| 3-SAT aleatorio | 24 | 3.0999 | 0.1588 | 19.52x |
| 3-SAT aleatorio | 30 | 19.7941 | 0.1773 | 111.66x |
| 3-SAT aleatorio | 36 | 409.8742 | 0.2406 | 1,703.45x |
| Igualdades separadas | 36 | 86.5329 | 0.0671 | 1,289.63x |
| Cadena SAT | 10,000 | 10.8619 | 6.7805 | 1.60x |
| Cadena UNSAT | 10,000 | 10.1422 | 3.6883 | 2.75x |
| Paridades contradictorias | 28 | 2.1192 | 0.0961 | 22.06x |
| 3-SAT en banda | 1,000 | 1.3232 | 1.6390 | 0.81x |

**Regresión incluida:** el caso de 3-SAT en banda de 1000 variables ya tenía una frontera de cuatro estados. Aquí ULTRA añade trabajo de preparación y ordenación y resulta más lento. El paquete conserva todos los resultados.

## Casos añadidos donde el motor anterior agotó su presupuesto

No se calcula un factor de aceleración cuando el motor anterior no obtuvo una respuesta definitiva. Las instancias plantadas tienen una asignación conocida por construcción; cada testigo devuelto se vuelve a comprobar contra el CNF original.

| Caso | Variables | Respuesta | ULTRA total (ms) | Motor usado |
|---|---:|---|---:|---|
| 3-SAT plantado | 100 | SAT | 0.4594 | CDCL |
| 3-SAT plantado | 300 | SAT | 25.4961 | CDCL |
| 3-SAT plantado | 500 | SAT | 10,913.8538 | CDCL |
| Igualdades separadas | 200 | SAT | 0.1587 | componentes + 2-SAT |
| Igualdades separadas | 10,000 | SAT | 5.4191 | componentes + 2-SAT |
| 7 palomas, 6 huecos | 42 | UNSAT | 3.8494 | CDCL |

La instancia plantada de 500 variables sigue siendo costosa: alrededor de 10.9 segundos en esta corrida. La ausencia de una frontera masiva no implica que desaparezca toda búsqueda difícil.

## Estados y memoria

Para las 18 igualdades separadas (36 variables), el solver anterior alcanza 262144 estados en una capa. ULTRA identifica 18 componentes de dos variables y los resuelve por 2-SAT; no materializa esa frontera. Un contador de frontera de 0 indica otro motor, no ausencia de trabajo.

| Caso | RAM anterior (MiB) | RAM ULTRA (MiB) |
|---|---:|---:|
| 3-SAT aleatorio, 36 variables | 31.68 | 1.91 |
| Igualdades separadas, 36 variables | 23.36 | 1.85 |
| Cadena SAT, 10000 variables | 6.59 | 6.82 |

La memoria es `VmHWM` de Linux para el espacio de direcciones actual, con runtime y entrada incluidos. Los valores de la familia aleatoria son medianas agrupadas, no la RAM de una fórmula seleccionada.

## Evidencia de corrección

La validación usa todas las combinaciones de cláusulas no tautológicas de dos variables; fórmulas aleatorias contrastadas por tablas de verdad y un DPLL independiente; 2-SAT; cláusulas vacías y tautologías; fórmulas de palomas y paridades de resultado conocido. Se fuerza además el cambio de grafo a CDCL.

Durante la validación normal se observaron 19 cambios por presupuesto, 643 reinicios y 303 compactaciones de la base de cláusulas. Los tests con sanitizadores cubrieron también esas rutas.

Esto es evidencia experimental, no una demostración formal de la implementación. SAT incluye certificado verificado; UNSAT no incluye DRAT/LRAT. No se hizo una comparación con los solvers actuales de SAT Competition y no se afirma superar a CaDiCaL, Kissat o MiniSat.

## Reproducir

```bash
g++ -O3 -DNDEBUG -std=c++17 solver_ultra.cpp -o solver_ultra
python3 validate_ultra.py
python3 compare_ultra.py --cpu 0
```

Sanitizadores (Linux; comprobación adicional):

```bash
g++ -O1 -g -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer solver_ultra.cpp -o solver_ultra_sanitized
ASAN_OPTIONS=detect_leaks=0 python3 validate_ultra.py --quick --binary ./solver_ultra_sanitized --report validation_sanitized.json
```

`results/raw.jsonl` y `results/runs.csv` contienen cada ejecución; `results/summary.*` los agregados; `results/environment.json` el entorno y hashes; `cases.json` los casos y respuestas; `instances/` los CNF y `baseline/` el C++ original.
