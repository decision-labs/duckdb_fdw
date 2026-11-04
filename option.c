/*-------------------------------------------------------------------------
 * 
 * DuckDB Foreign Data Wrapper for PostgreSQL
 * 
 * IDENTIFICATION
 *        option.c
 * 
 *------------------------------------------------------------------------*/

#include "postgres.h"
#include "duckdb_fdw.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "catalog/pg_attribute.h"

/*
 * Option context structure
 */
struct DuckDBFdwOption
{
	const char *defname;
	Oid			optcontext;
};

/*
 * Valid options
 */
static struct DuckDBFdwOption valid_options[] =
{
	/* Connection options */
	{"database", ForeignServerRelationId},
    
    /* S3 / Cloud Credentials */
    {"s3_region", ForeignServerRelationId},
    {"s3_access_key_id", ForeignServerRelationId},
    {"s3_secret_access_key", ForeignServerRelationId},
    {"s3_endpoint", ForeignServerRelationId},
    {"s3_endpoint_type", ForeignServerRelationId}, /* e.g. 's3_tables' */
    {"s3_use_ssl", ForeignServerRelationId},
    /* path: https://endpoint/bucket/key (RustFS/MinIO).
     * vhost: https://bucket.endpoint/key (AWS default). */
    {"s3_url_style", ForeignServerRelationId}, /* 'path' or 'vhost' */

    /* S3 credentials can also be set per-user via USER MAPPING (preferred
     * for security — pg_foreign_server options are visible to all users
     * with SELECT on pg_foreign_server, which is public by default.) */
    {"s3_access_key_id", UserMappingRelationId},
    {"s3_secret_access_key", UserMappingRelationId},

    /* MotherDuck token */
    {"motherduck_token", ForeignServerRelationId},
    {"motherduck_token", UserMappingRelationId},
    
    /* Catalogs */
    {"attach_catalogs", ForeignServerRelationId}, /* format: 'name=uri;type iceberg' */

    /* Quack client-server mode (ATTACH 'quack:host' AS remote) */
    {"quack_host", ForeignServerRelationId}, /* host:port of the Quack server */
    {"quack_token", UserMappingRelationId},  /* Quack auth token (secure: user mapping) */
    {"quack_token", ForeignServerRelationId}, /* fallback: server option */
    {"disable_ssl", ForeignServerRelationId}, /* boolean: ATTACH ... (DISABLE_SSL true|false) */
    {"ssl_fingerprint", ForeignServerRelationId}, /* ATTACH ... (SSL_FINGERPRINT '...') */

    /* DuckLake catalog over Quack (ATTACH 'ducklake:quack:host' AS alias) */
    {"ducklake_quack_host", ForeignServerRelationId}, /* host[:port], opaque */
    {"data_path", ForeignServerRelationId}, /* e.g. s3://bucket */
    {"ducklake_alias", ForeignServerRelationId}, /* ATTACH alias, default my_ducklake */

    /* Extensions */
    {"extensions", ForeignServerRelationId}, /* e.g., 'httpfs,spatial,iceberg' */

    /* Arbitrary DuckDB SQL run once on every new connection (LOAD, SET, ...) */
    {"init_sql", ForeignServerRelationId},

	/* Table options */
	{"table", ForeignTableRelationId},
    {"read_parquet", ForeignTableRelationId}, /* Path to parquet file */

	/* B3: per-column DuckDB-side name used by the write path (and the
	 * deparser); without accepting it here the validator rejected the option */
	{"column_name", ForeignTableRelationId},
	/* column-level options are validated against pg_attribute */
	{"column_name", AttributeRelationId},
	
    /* Execution options */
	{"use_remote_estimate", ForeignServerRelationId},
	{"force_readonly", ForeignServerRelationId},
	
	{NULL, InvalidOid}
};

/*
 * Check if the option is valid
 */
static bool
duckdb_is_valid_option(const char *option, Oid context)
{
	struct DuckDBFdwOption *opt;
	for (opt = valid_options; opt->defname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->defname, option) == 0)
			return true;
	}
	return false;
}

/*
 * FDW Option Validator
 */
PG_FUNCTION_INFO_V1(duckdb_fdw_validator);
Datum
duckdb_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *lc;

	bool		has_quack_host = false;
	bool		has_ducklake_host = false;
	bool		has_data_path = false;
	char	   *url_style = NULL;
	char	   *ducklake_alias = NULL;
	char	   *ducklake_host = NULL;
	char	   *data_path = NULL;

	foreach(lc, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		if (!duckdb_is_valid_option(def->defname, catalog))
		{
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname)));
		}

		/* value checks for options whose value is spliced into DuckDB SQL */
		if (strcmp(def->defname, "disable_ssl") == 0)
			(void) defGetBoolean(def);
		else if (strcmp(def->defname, "ssl_fingerprint") == 0)
		{
			char	   *fp = defGetString(def);

			if (fp[0] == '\0' || strspn(fp, "0123456789abcdefABCDEF:") != strlen(fp))
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("invalid value for option \"ssl_fingerprint\": \"%s\"", fp),
						 errhint("Use the SHA-256 fingerprint of the server certificate as hex digits, optionally separated by colons.")));
		}
		else if (strcmp(def->defname, "s3_url_style") == 0)
		{
			url_style = defGetString(def);

			if (strcmp(url_style, "path") != 0 && strcmp(url_style, "vhost") != 0)
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("invalid value for option \"s3_url_style\": \"%s\"", url_style),
						 errhint("Valid values are \"path\" and \"vhost\".")));
		}

		if (catalog != ForeignServerRelationId)
			continue;
		if (strcmp(def->defname, "quack_host") == 0)
			has_quack_host = true;
		else if (strcmp(def->defname, "ducklake_quack_host") == 0)
		{
			has_ducklake_host = true;
			ducklake_host = defGetString(def);
		}
		else if (strcmp(def->defname, "data_path") == 0)
		{
			has_data_path = true;
			data_path = defGetString(def);
		}
		else if (strcmp(def->defname, "ducklake_alias") == 0)
			ducklake_alias = defGetString(def);
	}

	if (catalog == ForeignServerRelationId)
	{
		if (has_quack_host && has_ducklake_host)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("cannot combine quack_host and ducklake_quack_host"),
					 errhint("quack_host ATTACHes a remote DuckDB; ducklake_quack_host ATTACHes a DuckLake catalog over Quack.")));
		if (has_ducklake_host && !has_data_path)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("ducklake_quack_host requires data_path"),
					 errhint("Set data_path to the DuckLake object-store location, e.g. s3://bucket.")));
		if (ducklake_alias && !duckdb_fdw_is_valid_identifier(ducklake_alias))
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid ducklake_alias \"%s\"", ducklake_alias)));
		if (ducklake_host && !duckdb_fdw_is_safe_sql_fragment(ducklake_host))
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("ducklake_quack_host contains an unsafe SQL fragment")));
		if (data_path && !duckdb_fdw_is_safe_sql_fragment(data_path))
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("data_path contains an unsafe SQL fragment")));
	}
	PG_RETURN_VOID();
}

/*
 * Extract FDW options into a struct
 */
duckdb_opt *
duckdb_get_options(Oid foreignoid)
{
	ForeignTable *f_table = NULL;
	ForeignServer *f_server = NULL;
	List	   *options;
	ListCell   *lc;
	duckdb_opt *opt;

	opt = (duckdb_opt *) palloc0(sizeof(duckdb_opt));

	PG_TRY();
	{
		f_table = GetForeignTable(foreignoid);
		f_server = GetForeignServer(f_table->serverid);
	}
	PG_CATCH();
	{
		FlushErrorState();
		f_table = NULL;
		f_server = GetForeignServer(foreignoid);
	}
	PG_END_TRY();

	options = NIL;
	if (f_table)
		options = list_concat(options, f_table->options);
	options = list_concat(options, f_server->options);

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		if (strcmp(def->defname, "database") == 0)
			opt->svr_database = defGetString(def);
		else if (strcmp(def->defname, "table") == 0)
			opt->svr_table = defGetString(def);
		else if (strcmp(def->defname, "use_remote_estimate") == 0)
			opt->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(def->defname, "force_readonly") == 0)
			opt->force_readonly = defGetBoolean(def);
	}

	/* If table name is not specified, use Postgres relation name */
	if (!opt->svr_table && f_table)
		opt->svr_table = get_rel_name(foreignoid);

	return opt;
}
