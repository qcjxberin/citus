/* citus--6.1-13--6.1-14.sql */

SET search_path = 'pg_catalog';

CREATE FUNCTION worker_apply_sequence_command(text)
    RETURNS VOID
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', $$worker_apply_sequence_command$$;
COMMENT ON FUNCTION worker_apply_sequence_command(text)
    IS 'create a sequence which products globally unique values';
    
RESET search_path;
