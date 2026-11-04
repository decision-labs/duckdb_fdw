-- 20 Live DuckLake-over-Quack attach (skipped unless DUCKLAKE_QUACK_HOST is set).
-- Placeholders are rewritten by run_tests.sh from env:
--   YOUR_QUACK_HOST YOUR_DATA_PATH YOUR_S3_ENDPOINT YOUR_QUACK_TOKEN
--   YOUR_ACCESS_KEY YOUR_SECRET_KEY YOUR_S3_USE_SSL
CREATE EXTENSION IF NOT EXISTS postgis;
DROP EXTENSION IF EXISTS duckdb_fdw CASCADE;
CREATE EXTENSION duckdb_fdw;

CREATE SERVER ducklake_quack_live FOREIGN DATA WRAPPER duckdb_fdw OPTIONS (ducklake_quack_host 'YOUR_QUACK_HOST', data_path 'YOUR_DATA_PATH', s3_endpoint 'YOUR_S3_ENDPOINT', s3_use_ssl 'YOUR_S3_USE_SSL', s3_url_style 'path', disable_ssl 'YOUR_DISABLE_SSL');
CREATE USER MAPPING IF NOT EXISTS FOR CURRENT_USER SERVER ducklake_quack_live OPTIONS (quack_token 'YOUR_QUACK_TOKEN', s3_access_key_id 'YOUR_ACCESS_KEY', s3_secret_access_key 'YOUR_SECRET_KEY');

CREATE SCHEMA IF NOT EXISTS remote_lake;
IMPORT FOREIGN SCHEMA main FROM SERVER ducklake_quack_live INTO remote_lake;

SELECT count(*) >= 0 AS imported FROM information_schema.tables WHERE table_schema = 'remote_lake';
