# All 1M Dataset Runner

`scripts/run_all_1m.sh` is the one-command runner for the current GLIN
reproduction pipeline.

It covers these datasets by default:

```text
AW
LW
ROADS
PARKS
OSM_AU_POINTS
UNIF_S
UNIF_L
DIAG_S
DIAG_L
```

The runner performs:

```text
1. Build required C++ tools.
2. Convert OSM Australia binary points to WKT, if needed.
3. Generate UNIF/DIAG synthetic geo point WKT, if needed.
4. Generate JTS STRtree KNN query windows.
5. Run GLIN Contains.
6. Run GLIN-piecewise Intersects.
7. Run Boost R-tree Contains/Intersects.
8. Run GEOS Quadtree Contains/Intersects.
9. Summarize raw CSV files.
10. Draw figures.
```

## Full Run

```bash
scripts/run_all_1m.sh
```

Default output:

```text
queries/all_1m/
results/all_1m/
results/all_1m_summary.csv
figures/all_1m/
```

## Smoke Test

```bash
DATASETS="UNIF_S" LIMIT=1000 QUERY_COUNT=5 \
RESULT_DIR=results/all_1m_smoke \
QUERY_DIR=queries/all_1m_smoke \
FIGURE_DIR=figures/all_1m_smoke \
SUMMARY_CSV=results/all_1m_smoke_summary.csv \
OVERWRITE=1 \
scripts/run_all_1m.sh
```

## Dry Run

Print commands without running them:

```bash
DRY_RUN=1 DATASETS="AW" LIMIT=1000 QUERY_COUNT=3 \
scripts/run_all_1m.sh
```

## Common Parameters

```text
LIMIT=1000000          records loaded per dataset
QUERY_COUNT=100        query windows per selectivity
SEED=42                random seed
DATASETS="AW PARKS"    run only selected datasets
OVERWRITE=1            regenerate existing outputs
AUTO_BUILD=0           skip cmake build step
RUN_CONTAINS=0         skip Contains benchmarks
RUN_INTERSECTS=0       skip Intersects benchmarks
GENERATE_QUERIES=0     reuse existing query files
RUN_BENCHMARKS=0       only prepare data/query/summary/plot
SUMMARIZE=0            skip summary generation
PLOT=0                 skip plotting
```

## Important Note

For point datasets, very low-selectivity KNN query windows can be nearly
degenerate. `Contains` may report zero answers when the query polygon only
touches the point boundary. `Intersects` is usually the more natural
relationship for point query-window tests.
