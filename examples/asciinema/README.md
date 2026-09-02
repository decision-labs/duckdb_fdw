# Live DuckLake-over-Quack demo

Asciinema of:

1. **DuckDB CLI** attaching `ducklake:quack:localhost:1028` (secrets via `getenv`, not typed)
2. **Postgres** `CREATE SERVER` / `USER MAPPING` / `IMPORT FOREIGN SCHEMA`
3. **CRUD** on `remote_lake.fdw_demo`: INSERT + SELECT through the FDW; UPDATE/DELETE via `duckdb_execute` (native FDW UPDATE/DELETE are not implemented)

Play locally:

```bash
asciinema play examples/asciinema/ducklake-quack-live.cast
```

Online player (anonymous upload, may expire): https://asciinema.org/a/BF8xMlpOH6ogR7He
