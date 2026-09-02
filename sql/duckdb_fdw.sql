-- Basic Extension Setup
CREATE EXTENSION duckdb_fdw;

-- Create Server
CREATE SERVER duckdb_test FOREIGN DATA WRAPPER duckdb_fdw
OPTIONS (database '/tmp/duckdb_fdw_regress_main.db');

-- Admin helper privilege defaults
CREATE ROLE duckdb_fdw_unprivileged;
SELECT has_function_privilege('duckdb_fdw_unprivileged', 'duckdb_execute(name,text)', 'EXECUTE');
SELECT has_function_privilege('duckdb_fdw_unprivileged', 'duckdb_create_s3_secret(name,text,text,text,text)', 'EXECUTE');

-- Test version function
SELECT duckdb_fdw_version() IS NOT NULL;

-- Create a table in DuckDB via DDL function
SELECT duckdb_execute('duckdb_test', 'DROP TABLE IF EXISTS test_types');
SELECT duckdb_execute('duckdb_test', 'CREATE TABLE test_types (i INTEGER, j BIGINT, d DOUBLE, s VARCHAR)');
SELECT duckdb_execute('duckdb_test', 'INSERT INTO test_types VALUES (1, 100, 3.14, ''hello''), (2, 200, 6.28, ''world'')');

-- Map it to PostgreSQL
CREATE FOREIGN TABLE test_types (
    i INT4,
    j INT8,
    d FLOAT8,
    s TEXT
) SERVER duckdb_test OPTIONS (table 'test_types');

-- Basic Query
SELECT * FROM test_types ORDER BY i;

-- New Types Support Test (UUID, Decimal, Date, Timestamp)
SELECT duckdb_execute('duckdb_test', 'DROP TABLE IF EXISTS test_v2');
SELECT duckdb_execute('duckdb_test', 'CREATE TABLE test_v2 (
    u UUID, 
    d DECIMAL(18,3), 
    dt DATE, 
    ts TIMESTAMP,
    b BOOLEAN
)');

SELECT duckdb_execute('duckdb_test', 'INSERT INTO test_v2 VALUES (
    ''550e8400-e29b-41d4-a716-446655440000'', 
    12345.678, 
    ''2024-01-01'', 
    ''2024-01-01 12:34:56'',
    true
)');

CREATE FOREIGN TABLE test_v2 (
    u UUID,
    d NUMERIC(18,3),
    dt DATE,
    ts TIMESTAMP,
    b BOOL
) SERVER duckdb_test OPTIONS (table 'test_v2');

SELECT * FROM test_v2;

-- Appender API Test (INSERT)
INSERT INTO test_types (i, j, d, s) VALUES (3, 300, 9.42, 'appender');
SELECT * FROM test_types WHERE i = 3;

-- Batch Insert API Test (COPY)
CREATE TABLE local_data (i int, j bigint, d double precision, s text);
INSERT INTO local_data SELECT g, g*10, g*1.1, 'str' || g FROM generate_series(4, 10) g;
COPY test_types FROM PROGRAM 'cat' (FORMAT CSV); -- This doesn't work directly like this for FDW
-- Better: INSERT INTO ... SELECT
INSERT INTO test_types SELECT * FROM local_data;
SELECT count(*) FROM test_types;

-- Advanced Pushdown Test
EXPLAIN (VERBOSE, COSTS OFF) 
SELECT stddev(i), variance(j), log(d) FROM test_types GROUP BY s;

-- Filter pushdown test
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM test_types WHERE i = 1;
SELECT * FROM test_types WHERE i = 1;

-- Server option refresh should use a new DuckDB database after ALTER SERVER
CREATE SERVER duckdb_switch FOREIGN DATA WRAPPER duckdb_fdw
OPTIONS (database 'duckdb_switch_one.db');
SELECT duckdb_execute('duckdb_switch', 'DROP TABLE IF EXISTS switch_test');
SELECT duckdb_execute('duckdb_switch', 'CREATE TABLE switch_test (i INTEGER)');
SELECT duckdb_execute('duckdb_switch', 'INSERT INTO switch_test VALUES (1)');
ALTER SERVER duckdb_switch OPTIONS (SET database 'duckdb_switch_two.db');
SELECT duckdb_execute('duckdb_switch', 'DROP TABLE IF EXISTS switch_test');
SELECT duckdb_execute('duckdb_switch', 'CREATE TABLE switch_test (i INTEGER)');
SELECT duckdb_execute('duckdb_switch', 'INSERT INTO switch_test VALUES (2)');
CREATE FOREIGN TABLE switch_test_ft (
    i INT4
) SERVER duckdb_switch OPTIONS (table 'switch_test');
SELECT * FROM switch_test_ft;
DROP FOREIGN TABLE switch_test_ft;
DROP SERVER duckdb_switch CASCADE;

-- BLOB / GEOMETRY scan (no PostGIS required). GEOMETRY imports as bytea;
-- with PostGIS installed, IMPORT would yield geometry instead.
CREATE SERVER duckdb_geom FOREIGN DATA WRAPPER duckdb_fdw
OPTIONS (database '/tmp/duckdb_fdw_regress_geom.db');
SELECT duckdb_execute('duckdb_geom', 'DROP TABLE IF EXISTS test_blob');
SELECT duckdb_execute('duckdb_geom', 'CREATE TABLE test_blob (id INTEGER, b BLOB)');
SELECT duckdb_execute('duckdb_geom', 'INSERT INTO test_blob VALUES (1, from_hex(''DEADBEEF''))');
SELECT duckdb_execute('duckdb_geom', 'DROP TABLE IF EXISTS test_geom');
SELECT duckdb_execute('duckdb_geom', 'CREATE TABLE test_geom (id INTEGER, g GEOMETRY)');
SELECT duckdb_execute('duckdb_geom', 'INSERT INTO test_geom VALUES (1, ''POINT(1 2)'')');
DROP SCHEMA IF EXISTS duckdb_geom_imp CASCADE;
CREATE SCHEMA duckdb_geom_imp;
IMPORT FOREIGN SCHEMA main FROM SERVER duckdb_geom INTO duckdb_geom_imp;
SELECT c.relname, a.attname, format_type(a.atttypid, a.atttypmod)
  FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
  JOIN pg_attribute a ON a.attrelid = c.oid
  WHERE n.nspname = 'duckdb_geom_imp' AND a.attnum > 0 AND NOT a.attisdropped
  ORDER BY c.relname, a.attnum;
SELECT id, encode(b, 'hex') FROM duckdb_geom_imp.test_blob;
SELECT id, encode(g, 'hex') IS NOT NULL AS geom_no_crash FROM duckdb_geom_imp.test_geom;
DROP FOREIGN TABLE duckdb_geom_imp.test_blob;
DROP FOREIGN TABLE duckdb_geom_imp.test_geom;
DROP SCHEMA duckdb_geom_imp;
DROP SERVER duckdb_geom CASCADE;

-- Unsupported write paths remain explicit
UPDATE test_types SET s = 'changed' WHERE i = 1;
DELETE FROM test_types WHERE i = 1;

-- Cleanup
DROP FOREIGN TABLE test_types;
DROP ROLE duckdb_fdw_unprivileged;
DROP SERVER duckdb_test CASCADE;
DROP EXTENSION duckdb_fdw;
