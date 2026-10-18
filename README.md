# Grafo de configuraciones: Python, C++ y benchmark

Implementación de la construcción de la sección 3 de **Kaoru Aguilera Katayama,
«P=NP by proving that verifying in polylogarithmic time allows you to prove in
polynomial time» (1 de octubre de 2026)**, a partir del PDF proporcionado.

## Qué se implementó

El motor `explore` recibe un verificador determinista que lee un certificado
binario de izquierda a derecha. En cada posición prueba 0 y 1 y fusiona las
ejecuciones con la misma memoria. Conserva un predecesor por configuración para
recuperar un certificado aceptado. La posición del certificado es la capa del
grafo; no se fusionan estados de capas diferentes.

Una transición agrupa el trabajo determinista entre dos lecturas del certificado.
El verificador concreto debe suministrar esas transiciones y un predicado de
aceptación. Cualquier información que afecte al futuro debe formar parte de su
estado. `None` en Python, o `false` en C++, elimina una rama rechazada.

El motor explora **todas las configuraciones alcanzables de cada capa**, hasta
terminar el certificado o agotar las ramas. No prueba todas las asignaciones
completas por separado. Las fusiones se hacen por igualdad exacta del estado;
las tablas hash resuelven las colisiones comparando también el contenido.

El PDF proporciona esta construcción general, pero no las transiciones de un
verificador de SAT que cumpla simultáneamente las hipótesis de tiempo
polilogarítmico, espacio logarítmico y lectura de una sola pasada. Por ello, el
paquete separa el motor del paper y dos verificadores concretos añadidos aquí:

- **`cnf`**: verificador exacto de una asignación SAT, con memoria explícita de
  cláusulas pendientes. Permite ejecutar archivos DIMACS reales.
- **`mod`**: control de fusión de estados; busca un certificado binario de longitud
  `q` cuyo valor tenga residuo `target` módulo `M`.

El adaptador CNF no se presenta como el verificador holográfico que el PDF
presupone. Su ejecución no establece una cota polinómica para SAT general.

## Ejecutar Python

Requiere Python 3.10 o posterior, sin paquetes externos.

```bash
python3 config_graph.py cnf examples/sat.cnf --output solution_python.json
python3 config_graph.py cnf examples/unsat.cnf
python3 config_graph.py mod 64 36 63 --trace
```

## Ejecutar C++

Requiere un compilador C++17 y únicamente la biblioteca estándar. No requiere
Boost. Las máscaras de más de 64 bits usan tantos bloques de 64 bits como
necesiten; no se truncan cláusulas ni índices.

```bash
g++ -O3 -DNDEBUG -std=c++17 config_graph.cpp -o config_graph_cpp
./config_graph_cpp cnf examples/sat.cnf --output solution_cpp.json
./config_graph_cpp cnf examples/unsat.cnf
./config_graph_cpp mod 64 36 63 --trace
```

En Windows, compilar con un compilador compatible con C++17 y ejecutar el `.exe`
resultante. Los números de memoria del benchmark se midieron en Linux.

## Entradas y resultados

DIMACS requiere una cabecera `p cnf NUM_VARIABLES NUM_CLAUSES`; cada cláusula
termina en 0. Se admiten comentarios `c`, cláusulas en varias líneas, cláusulas
vacías, variables declaradas sin uso, literales repetidos y tautologías. Se
rechazan índices fuera del rango declarado, cabeceras duplicadas y cuentas
incorrectas. El archivo es texto UTF-8/ASCII sin BOM. Extensiones no estándar
como terminadores `%` o comentarios al final de una línea no se admiten.

Ambos programas imprimen JSON. `--output archivo.json` también lo guarda.

- `status`: `SAT`, `UNSAT` o `UNKNOWN`; en el control modular, `ACCEPT`/`REJECT`.
- `witness`: lista de 0/1 en orden `x1, x2, ..., xn`, o `null` cuando no hay
  certificado disponible. Cada resultado SAT con certificado se comprueba
  contra **todas las cláusulas originales** antes de devolverse.
- `configurations`: nodos distintos creados, sumando todas las capas y el inicial.
- `peak_frontier`: mayor número de estados en una capa completada.
- `attempted_edges`, `valid_edges`, `merged_edges`: transiciones intentadas,
  admitidas y que llegan a una configuración ya existente, respectivamente.
- `state_bits_bound`: espacios de cláusula reservados para la memoria del
  verificador CNF, o bits del residuo modular. No es la RAM de todo el programa.
- `graph_seconds`: búsqueda y reconstrucción del certificado.
- `prepare_seconds`: compilación del verificador CNF.
- `total_seconds`: lectura, preparación, búsqueda y comprobación del certificado,
  sin arranque del proceso, importaciones de Python ni escritura del JSON.
- `peak_rss_kib`: máximo de memoria residente del proceso; incluye intérprete,
  entrada y estructuras auxiliares. No es memoria adicional neta del algoritmo.
  En Linux se usa `VmHWM` del espacio de direcciones actual, evitando el suelo
  de memoria que `ru_maxrss` puede heredar del lanzador antes de `exec`.
- `--trace`: agrega el ancho de todas las capas.

**No hay límite predeterminado de variables, estados o tiempo.** Los recursos
físicos siguen limitando la ejecución. Opcionalmente:

```bash
python3 config_graph.py cnf instancia.cnf --timeout 60
./config_graph_cpp cnf instancia.cnf --max-frontier 1000000
```

Esos límites producen `UNKNOWN`, nunca `UNSAT`. `--timeout` se aplica a la
búsqueda del grafo, no a la lectura ni a la preparación. `--decision-only` omite
los predecesores y el certificado, conservando la decisión exacta si termina.
El benchmark entregado mantiene la recuperación del certificado activada.

## Adaptador SAT: estado y transición

Se procesan las variables en el orden numérico del DIMACS. Cada cláusula ocupa
un bit desde su primera hasta su última variable. El bit indica si algún literal
ya leído satisface la cláusula. Al leer la última variable de esa cláusula:

1. Si sigue sin estar satisfecha, se rechaza esa rama.
2. Si está satisfecha, se borra su bit y se libera el espacio.

Los espacios se reutilizan para cláusulas que comienzan después. Se eliminan
tautologías y literales repetidos; no se usa aprendizaje de cláusulas, búsqueda
DPLL, propagación global de unidades ni reordenación heurística en los solvers
medidos. La asignación completa es un certificado de longitud exactamente `n`,
por lo que no hace falta enumerar otras longitudes. El auxiliar
`explore_lengths` muestra esa enumeración para otros verificadores.

Si `w` es el número de espacios de cláusula necesarios, hay a lo sumo `2^w`
memorias posibles por capa. La búsqueda tiene como cota `O(n * 2^w)` nodos, con
un coste adicional por operar, copiar y comparar máscaras. **`w` puede crecer
con la entrada**; este verificador no garantiza memoria `O(log N)` para un CNF
de tamaño `N`. Tampoco hace solamente un número polilogarítmico de lecturas:
lee una vez cada variable de la asignación. Su valor experimental es hacer
ejecutable y medible la construcción por configuraciones.

Los predecesores son metadatos de la búsqueda, no parte de la memoria del
verificador que determina las fusiones. Se conserva un único representante de
los prefijos que llegan al mismo estado; sus continuaciones aceptables son
idénticas, de modo que ese representante basta para recuperar un testigo.

## Control modular

La transición es `r' = (2*r + bit) mod M`. Hay como máximo `M` residuos por
capa y `1 + q*M` configuraciones en total. Se prueba `q = ceil(log2(M))^2` en el
benchmark. Esto permite observar el beneficio de fusionar estados: el número
de certificados posibles es `2^q`, pero los estados alcanzables se saturan.

La cota es polinómica **en M**, no en la longitud binaria de M. El comando
`mod M q target` usa parámetros compactos; no debe citarse por sí solo como
un verificador que cumple todas las hipótesis del paper respecto al tamaño de
esa entrada compacta. Es un control de estados acotados. Con una descripción
explícita de un autómata de M estados, el tamaño de entrada y la cota se
interpretarían de otra manera.

## Reproducir validación y benchmark

```bash
python3 validate.py
python3 benchmark.py --cpu 0
```

`--cpu 0` fija afinidad en Linux. Omitirlo si el sistema no lo admite o usar un
CPU permitido. Parámetros predeterminados: 3 repeticiones, 8 semillas por tamaño
aleatorio, tamaños de 12 a 36 variables, y un watchdog externo de 30 segundos
por ejecución. Un watchdog agotado se registra como `TIMEOUT`, no como UNSAT.
Las instancias se regeneran de forma determinista.

La validación compara con tablas de verdad y un DPLL independiente; también
contrasta los contadores, los certificados y todas las capas entre lenguajes.
El DPLL de `validate.py` es una referencia de corrección, no se usa dentro del
motor ni se presenta como un solver CDCL competitivo.

Familias medidas:

- `random3sat`: tres variables distintas por cláusula, signos equiprobables,
  `m = round(4.26*n)`, semillas publicadas en el código. Respuestas de referencia
  calculadas con el DPLL independiente.
- `chain_sat` / `chain_unsat`: igualdades entre variables consecutivas; el caso
  UNSAT añade valores incompatibles a los extremos. Hasta 10 000 variables.
- `separated_equalities`: `x_i = y_i`, leyendo primero todos los x y después
  todos los y. Fuerza `2^h` estados a mitad de la lectura, con `h` parejas.
- `interleaved_equalities`: la misma relación lógica, renombrada para leer
  `x1,y1,x2,y2,...`; basta un máximo de dos estados. Mide el efecto del orden.
- `modular`: control de M residuos descrito arriba.

Python y C++ se ejecutan secuencialmente, en procesos nuevos, alternando cuál
se mide primero. Hay una ejecución pequeña de calentamiento por lenguaje.
Se registran por separado los tiempos internos y el tiempo de proceso completo.
Los CSV contienen **todas** las repeticiones, no solamente el mejor tiempo.
Las medianas de las familias aleatorias agrupan semillas y repeticiones.

Archivos:

- `BENCHMARK.md`: resultados medidos y alcance de las conclusiones.
- `results/runs.csv`, `results/raw.jsonl`: observaciones completas.
- `results/summary.csv`, `results/summary.json`: agregados por familia y tamaño.
- `results/environment.json`: CPU, versiones, parámetros y hashes del código.
- `results/manifest.json`: instancias, respuestas de referencia y SHA-256.
- `instances/`: todos los CNF utilizados; `validation.json`: comprobaciones.
- `plot_results.py`: genera las figuras; requiere Matplotlib únicamente para
  esa operación opcional. Los solvers y el benchmark no lo requieren.

## Relación con la referencia holográfica

Se verificó la fuente citada por el PDF: Babai, Fortnow, Levin y Szegedy,
*Checking Computations in Polylogarithmic Time*, STOC 1991,
DOI [10.1145/103418.103428](https://doi.org/10.1145/103418.103428).
La [versión alojada por Fortnow](https://lance.fortnow.com/papers/files/check.pdf),
sección 1.2, describe un verificador probabilístico con acceso aleatorio a la
prueba, la entrada codificada y los bits aleatorios. La
[explicación del coautor Leonid Levin](https://www.cs.bu.edu/faculty/lnd/research/er-r.htm)
también enumera estos requisitos.

Por tanto, esa referencia no suministra directamente el verificador
determinista de una pasada que necesita la búsqueda existencial del PDF.
Encontrar una ejecución aceptante al escoger también las monedas no equivale
a comprobar la condición probabilística de aceptación de una misma prueba.
Esta es una diferencia entre modelos; los tiempos del benchmark no resuelven
esa diferencia ni constituyen una prueba de P=NP o de P≠NP.

El resultado implementado y medido es la búsqueda por configuraciones, con
verificadores explícitos y respuestas contrastadas.
