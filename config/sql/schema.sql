CREATE TABLE version(version INTEGER);
CREATE TABLE device_info(device_id TEXT, is_registered INTEGER NOT NULL DEFAULT 0 CHECK (is_registered IN (0,1)));
CREATE TABLE ecu_serials(serial TEXT UNIQUE, hardware_id TEXT NOT NULL, is_primary INTEGER NOT NULL CHECK (is_primary IN (0,1)));
CREATE TABLE misconfigured_ecus(serial TEXT UNIQUE, hardware_id TEXT NOT NULL, state INTEGER NOT NULL CHECK (state IN (0,1)));
CREATE TABLE installed_versions(hash TEXT UNIQUE, name TEXT NOT NULL, is_current INTEGER NOT NULL CHECK (is_current IN (0,1)) DEFAULT 0,
                                length INTEGER NOT NULL DEFAULT 0);
CREATE TABLE primary_keys(private TEXT, public TEXT);
CREATE TABLE tls_creds(ca_cert BLOB, ca_cert_format TEXT,
                       client_cert BLOB, client_cert_format TEXT,
                       client_pkey BLOB, client_pkey_format TEXT);
CREATE TABLE meta(meta BLOB NOT NULL, repo INTEGER NOT NULL, meta_type INTEGER NOT NULL, version INTEGER NOT NULL);
CREATE TABLE target_images(filename TEXT UNIQUE, image_data BLOB NOT NULL);
CREATE TABLE repo_types(repo INTEGER NOT NULL, repo_string TEXT NOT NULL);
CREATE TABLE meta_types(meta INTEGER NOT NULL, meta_string TEXT NOT NULL);
