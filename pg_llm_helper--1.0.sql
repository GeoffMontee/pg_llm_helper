-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_llm_helper" to load this file. \quit

CREATE FUNCTION get_last_error()
RETURNS TABLE (
    backend_pid int,
    query_text text,
    error_message text,
    sql_state text,
    error_level int,
    timestamp timestamptz
)
AS 'MODULE_PATHNAME', 'get_last_error'
LANGUAGE C STRICT;

CREATE FUNCTION get_error_history(limit int DEFAULT 10)
RETURNS TABLE (
    backend_pid int,
    query_text text,
    error_message text,
    sql_state text,
    error_level int,
    timestamp timestamptz
)
AS 'MODULE_PATHNAME', 'get_error_history'
LANGUAGE C STRICT;

CREATE FUNCTION clear_error_history()
RETURNS void
AS 'MODULE_PATHNAME', 'clear_error_history'
LANGUAGE C STRICT;

-- Convenience function to get LLM help on last error
CREATE FUNCTION llm_help_last_error()
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
    err record;
    llm_response text;
BEGIN
    SELECT * INTO err FROM get_last_error();
    
    IF err IS NULL THEN
        RETURN 'No recent errors found for this session.';
    END IF;
    
    -- This assumes pgai is installed
    -- You can replace with any LLM integration you prefer
    SELECT ai.openai_chat_complete(
        'gpt-4o-mini',
        jsonb_build_array(
            jsonb_build_object(
                'role', 'system',
                'content', 'You are a PostgreSQL expert. Provide concise error explanations and fixes.'
            ),
            jsonb_build_object(
                'role', 'user',
                'content', format(
                    E'PostgreSQL Error (SQL State: %s):\n\nQuery:\n%s\n\nError:\n%s\n\nExplain and suggest a fix.',
                    err.sql_state,
                    err.query_text,
                    err.error_message
                )
            )
        )
    )->'choices'->0->'message'->>'content'
    INTO llm_response;
    
    RETURN llm_response;
END;
$$;
