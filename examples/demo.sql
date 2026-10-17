-- First LOAD the extension with an absolute path (required by hardened macOS CLI builds).
SELECT release, engine_version FROM gs_installed_releases();
SET VARIABLE geosupport_release = (SELECT bundle_id FROM gs_load('26c'));
CREATE TEMP TABLE demo AS
SELECT gs_address('125', 'Worth St', 'Mn') AS geo;
SELECT geo.bbl, geo.bin, geo.latitude, geo.longitude, geo.release, geo.bundle_id
FROM demo;
SELECT
    (gs_address('125', 'Worth St', 'Mn', release := '26b')).release AS prior,
    (gs_address('125', 'Worth St', 'Mn', release := '26c')).release AS current;
