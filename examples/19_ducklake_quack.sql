-- 19 DuckLake-over-Quack option validation (no live lake)
DROP EXTENSION IF EXISTS duckdb_fdw CASCADE;
CREATE EXTENSION duckdb_fdw;

-- data_path is required with ducklake_quack_host
DO $$
BEGIN
  EXECUTE 'CREATE SERVER lake_missing_path FOREIGN DATA WRAPPER duckdb_fdw OPTIONS (ducklake_quack_host ''localhost:9494'')';
  RAISE EXCEPTION 'expected data_path required';
EXCEPTION
  WHEN others THEN
    IF position('data_path' in SQLERRM) = 0 THEN
      RAISE;
    END IF;
END $$;

-- cannot combine quack_host and ducklake_quack_host
DO $$
BEGIN
  EXECUTE 'CREATE SERVER lake_both FOREIGN DATA WRAPPER duckdb_fdw OPTIONS (quack_host ''localhost:9494'', ducklake_quack_host ''localhost:9494'', data_path ''s3://b'')';
  RAISE EXCEPTION 'expected combine reject';
EXCEPTION
  WHEN others THEN
    IF position('quack_host' in SQLERRM) = 0 THEN
      RAISE;
    END IF;
END $$;

-- s3_url_style must be path or vhost
DO $$
BEGIN
  EXECUTE 'CREATE SERVER lake_style FOREIGN DATA WRAPPER duckdb_fdw OPTIONS (ducklake_quack_host ''localhost:9494'', data_path ''s3://b'', s3_url_style ''weird'')';
  RAISE EXCEPTION 'expected url style reject';
EXCEPTION
  WHEN others THEN
    IF position('s3_url_style' in SQLERRM) = 0 THEN
      RAISE;
    END IF;
END $$;

-- Valid definition only (no query — attach needs a live Quack + S3)
CREATE SERVER ducklake_quack_srv FOREIGN DATA WRAPPER duckdb_fdw OPTIONS (ducklake_quack_host 'localhost:9494', data_path 's3://kroobay', s3_endpoint 'localhost:9000', s3_use_ssl 'false', s3_url_style 'path');
CREATE USER MAPPING IF NOT EXISTS FOR CURRENT_USER SERVER ducklake_quack_srv OPTIONS (quack_token 'placeholder', s3_access_key_id 'placeholder', s3_secret_access_key 'placeholder');

DROP SERVER ducklake_quack_srv CASCADE;
DROP EXTENSION duckdb_fdw CASCADE;
