#include "postgres.h"
#include "duckdb_fdw_geom.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*
 * Cached PostGIS geometry type OID. This fork requires PostGIS
 * (duckdb_fdw.control requires = 'postgis'). Lookup still retries a
 * miss so a same-session CREATE EXTENSION postgis is visible. Do not
 * fall back to bytea. Do not link liblwgeom.
 */
static Oid	duckdb_fdw_geometry_oid = InvalidOid;
static bool duckdb_fdw_geometry_looked_up = false;

static Oid	duckdb_fdw_st_geomfromwkb_oid = InvalidOid;
static bool duckdb_fdw_st_geomfromwkb_looked_up = false;

static const char *
duckdb_fdw_postgis_errhint(void)
{
	return "Install PostGIS, then run CREATE EXTENSION postgis WITH SCHEMA extensions; (Geobase) or CREATE EXTENSION postgis;";
}

static void
duckdb_fdw_ereport_geometry_missing(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("PostGIS is required for DuckDB GEOMETRY columns"),
			 errdetail("The geometry type was not found in schema \"extensions\" or search_path."),
			 errhint("%s", duckdb_fdw_postgis_errhint())));
}

static void
duckdb_fdw_ereport_st_geomfromwkb_missing(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot convert DuckDB GEOMETRY to PostGIS geometry"),
			 errdetail("function ST_GeomFromWKB(bytea) was not found."),
			 errhint("%s", duckdb_fdw_postgis_errhint())));
}

static Oid
duckdb_fdw_lookup_type(const char *nspname, const char *typname)
{
	TypeName   *tn;
	List	   *names;

	if (nspname != NULL)
		names = list_make2(makeString(pstrdup(nspname)),
						   makeString(pstrdup(typname)));
	else
		names = list_make1(makeString(pstrdup(typname)));

	tn = makeTypeNameFromNameList(names);
	return LookupTypeNameOid(NULL, tn, true);
}

static Oid
duckdb_fdw_lookup_geometry_oid(void)
{
	Oid			oid;

	/*
	 * Geobase installs PostGIS in the extensions schema, which may or may
	 * not be on search_path depending on the role. Try that first.
	 */
	oid = duckdb_fdw_lookup_type("extensions", "geometry");
	if (OidIsValid(oid))
		return oid;

	/* public / search_path */
	return duckdb_fdw_lookup_type(NULL, "geometry");
}

static Oid
duckdb_fdw_geometry_type_oid(bool retry_miss)
{
	if (!duckdb_fdw_geometry_looked_up)
	{
		duckdb_fdw_geometry_oid = duckdb_fdw_lookup_geometry_oid();
		duckdb_fdw_geometry_looked_up = true;
		return duckdb_fdw_geometry_oid;
	}

	if (!OidIsValid(duckdb_fdw_geometry_oid) && retry_miss)
		duckdb_fdw_geometry_oid = duckdb_fdw_lookup_geometry_oid();

	return duckdb_fdw_geometry_oid;
}

static bool
duckdb_fdw_oid_is_geometry(Oid pgtype)
{
	HeapTuple	tup;
	Form_pg_type typform;
	bool		is_geom;

	if (!OidIsValid(pgtype))
		return false;

	if (OidIsValid(duckdb_fdw_geometry_oid) && pgtype == duckdb_fdw_geometry_oid)
		return true;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(pgtype));
	if (!HeapTupleIsValid(tup))
		return false;

	typform = (Form_pg_type) GETSTRUCT(tup);
	is_geom = (strcmp(NameStr(typform->typname), "geometry") == 0);
	ReleaseSysCache(tup);

	if (is_geom)
		duckdb_fdw_geometry_oid = pgtype;

	return is_geom;
}

static Oid
duckdb_fdw_lookup_func(const char *nspname, const char *funcname,
					   int nargs, const Oid *argtypes)
{
	List	   *names;

	if (nspname != NULL)
		names = list_make2(makeString(pstrdup(nspname)),
						   makeString(pstrdup(funcname)));
	else
		names = list_make1(makeString(pstrdup(funcname)));

	return LookupFuncName(names, nargs, argtypes, true);
}

static Oid
duckdb_fdw_lookup_st_geomfromwkb(void)
{
	Oid			argtypes[1];
	Oid			oid;

	argtypes[0] = BYTEAOID;

	oid = duckdb_fdw_lookup_func("extensions", "st_geomfromwkb", 1, argtypes);
	if (OidIsValid(oid))
		return oid;

	return duckdb_fdw_lookup_func(NULL, "st_geomfromwkb", 1, argtypes);
}

static Oid
duckdb_fdw_st_geomfromwkb(void)
{
	if (!duckdb_fdw_st_geomfromwkb_looked_up)
	{
		duckdb_fdw_st_geomfromwkb_oid = duckdb_fdw_lookup_st_geomfromwkb();
		duckdb_fdw_st_geomfromwkb_looked_up = true;
		return duckdb_fdw_st_geomfromwkb_oid;
	}

	if (!OidIsValid(duckdb_fdw_st_geomfromwkb_oid))
		duckdb_fdw_st_geomfromwkb_oid = duckdb_fdw_lookup_st_geomfromwkb();

	return duckdb_fdw_st_geomfromwkb_oid;
}

static void
duckdb_fdw_require_postgis(void)
{
	if (!OidIsValid(duckdb_fdw_geometry_type_oid(true)))
		duckdb_fdw_ereport_geometry_missing();

	if (!OidIsValid(duckdb_fdw_st_geomfromwkb()))
		duckdb_fdw_ereport_st_geomfromwkb_missing();
}

const char *
duckdb_fdw_geom_pg_type_name(void)
{
	duckdb_fdw_require_postgis();

	/*
	 * Qualify when PostGIS lives in Geobase's extensions schema so IMPORT
	 * works even if that schema is not on search_path.
	 */
	if (OidIsValid(duckdb_fdw_lookup_type("extensions", "geometry")))
		return "extensions.geometry";
	return "geometry";
}

static Datum
duckdb_fdw_blob_as_bytea(duckdb_blob blob)
{
	bytea	   *result;

	result = (bytea *) palloc(blob.size + VARHDRSZ);
	SET_VARSIZE(result, blob.size + VARHDRSZ);
	if (blob.size > 0 && blob.data != NULL)
		memcpy(VARDATA(result), blob.data, blob.size);
	return PointerGetDatum(result);
}

static Datum
duckdb_fdw_blob_as_hex_text(duckdb_blob blob)
{
	StringInfoData hex;
	Datum		res;
	idx_t		i;

	initStringInfo(&hex);
	appendStringInfoString(&hex, "\\x");
	for (i = 0; i < blob.size; i++)
		appendStringInfo(&hex, "%02x",
						 ((const unsigned char *) blob.data)[i]);
	res = PointerGetDatum(cstring_to_text(hex.data));
	pfree(hex.data);
	return res;
}

Datum
duckdb_fdw_blob_to_pg_datum(duckdb_blob blob, Oid pgtype)
{
	Datum		bytea_datum;

	if (pgtype == BYTEAOID)
		return duckdb_fdw_blob_as_bytea(blob);

	if (pgtype == TEXTOID || pgtype == VARCHAROID || pgtype == BPCHAROID)
		return duckdb_fdw_blob_as_hex_text(blob);

	if (duckdb_fdw_oid_is_geometry(pgtype))
	{
		Oid			func;

		/*
		 * ISO WKB → PostGIS via ST_GeomFromWKB(bytea). Do not use
		 * geometry_in on \x hex; that input often fails.
		 *
		 * SRID is left 0: duckdb_geometry_type_get_crs() is type-level
		 * (not per-value) and CRS strings are not reliably EPSG codes.
		 * Parsing them on the scan path is not cheap or robust enough.
		 * Callers can ST_SetSRID() locally when they know the CRS.
		 */
		func = duckdb_fdw_st_geomfromwkb();
		if (!OidIsValid(func))
			duckdb_fdw_ereport_st_geomfromwkb_missing();

		bytea_datum = duckdb_fdw_blob_as_bytea(blob);
		return OidFunctionCall1(func, bytea_datum);
	}

	ereport(ERROR,
			(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
			 errmsg("cannot convert DuckDB BLOB to type %s",
					format_type_be(pgtype)),
			 errhint("Map BLOB columns as bytea or text.")));
	return (Datum) 0;
}

bool
duckdb_fdw_convert_blob_column(duckdb_result *res, int col_idx,
							   uint64_t row, Oid pgtype, Datum *out)
{
	duckdb_type duck_type;
	duckdb_blob blob;

	Assert(res != NULL);
	Assert(out != NULL);

	duck_type = duckdb_column_type(res, (idx_t) col_idx);
	if (duck_type != DUCKDB_TYPE_GEOMETRY && duck_type != DUCKDB_TYPE_BLOB)
		return false;

	if (duck_type == DUCKDB_TYPE_GEOMETRY && !duckdb_fdw_oid_is_geometry(pgtype))
	{
		duckdb_fdw_require_postgis();
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("DuckDB GEOMETRY columns must map to PostGIS geometry"),
				 errhint("Re-IMPORT the foreign table, or declare the column as geometry.")));
	}

	if (duckdb_value_is_null(res, col_idx, row))
	{
		*out = (Datum) 0;
		return true;
	}

	blob = duckdb_value_blob(res, (idx_t) col_idx, row);
	/*
	 * The deprecated C API returns data=NULL when the value cannot be
	 * converted to blob (some native GEOMETRY values). Emit an empty
	 * blob rather than a null pointer Datum. DuckLake / spatial WKB
	 * typically has a real buffer.
	 */
	if (blob.data == NULL)
	{
		duckdb_blob empty;

		empty.data = NULL;
		empty.size = 0;
		*out = duckdb_fdw_blob_to_pg_datum(empty, pgtype);
		return true;
	}

	*out = duckdb_fdw_blob_to_pg_datum(blob, pgtype);
	duckdb_free(blob.data);
	return true;
}
