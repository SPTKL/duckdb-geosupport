-- Uses only the image's default entrypoint: no INSTALL, LOAD, or release setup.
SELECT CASE WHEN (SELECT count(*) FROM gs_installed_releases())=1
    THEN true ELSE error('Expected one preinstalled release') END;
CREATE TEMP TABLE image_results AS
    SELECT i, gs_address('125','Worth St','Mn') AS geo FROM range(4097) r(i);
SELECT CASE WHEN count(*)=4097 AND bool_and(geo.bbl='1001680032')
    AND count(DISTINCT geo.bundle_id)=1
    THEN true ELSE error('Image geocoding failed') END FROM image_results;
SELECT CASE WHEN (gs_bbl('1001680032')).bbl='1001680032'
    THEN true ELSE error('Image BBL lookup failed') END;
SELECT CASE WHEN (gs_address('999999','NOT A REAL STREET','Mn')).grc NOT IN ('00','01')
    THEN true ELSE error('Image lost reject codes') END;
