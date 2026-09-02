#ifndef DUCKDB_FDW_GEOM_H
#define DUCKDB_FDW_GEOM_H

/*
 * Soft PostGIS mapping for DuckDB GEOMETRY / BLOB.
 *
 * Fork-local: keep Geobase / PostGIS logic here so rebasing onto
 * alitrack/upstream is a few call-site hunks, not a duckdb_fdw.c rewrite.
 *
 * No liblwgeom. No REQUIRES postgis. Catalog lookup + fmgr only.
 */

#include "postgres.h"
#include "duckdb.h"

/*
 * PostgreSQL type name for DuckDB GEOMETRY at IMPORT time.
 * "geometry" if PostGIS geometry exists (extensions, then search_path),
 * otherwise "bytea".
 */
extern const char *duckdb_fdw_geom_pg_type_name(void);

/*
 * Convert a DuckDB blob (ISO WKB for GEOMETRY, raw bytes for BLOB)
 * to a PostgreSQL Datum of pgtype.
 *
 * bytea  -> raw varlena
 * text   -> \x hex
 * geometry (PostGIS) -> ST_GeomFromWKB(bytea); SRID left 0
 *
 * Does not call geometry_in on hex (that often fails).
 */
extern Datum duckdb_fdw_blob_to_pg_datum(duckdb_blob blob, Oid pgtype);

/*
 * Scan hook: if the DuckDB column is GEOMETRY or BLOB, read it with
 * duckdb_value_blob() (never varchar) and convert. Returns true when
 * handled (including SQL NULL, in which case *out is 0).
 */
extern bool duckdb_fdw_convert_blob_column(duckdb_result *res, int col_idx,
										   uint64_t row, Oid pgtype,
										   Datum *out);

#endif
