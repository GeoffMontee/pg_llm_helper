# pg_llm_helper - PostgreSQL LLM Error Helper

A PostgreSQL extension that automatically captures errors and integrates with LLMs to provide intelligent troubleshooting assistance.

## Features

- Automatically captures all PostgreSQL errors (including syntax errors)
- Stores error history in shared memory (survives across sessions)
- Provides simple SQL functions to query error history
- Integrates seamlessly with pgai for LLM-powered error analysis
- Zero-configuration error tracking

## Requirements

- PostgreSQL 17 (may work with earlier versions with minor modifications)
- C compiler (gcc or clang)
- PostgreSQL development headers
- pgai extension (optional, for LLM integration)

## Installation

### 1. Build and Install the Extension

```bash
# Navigate to the extension directory
cd pg_llm_helper

# Build the extension
make

# Install (requires sudo/root)
sudo make install
```

### 2. Configure PostgreSQL

Add the extension to `shared_preload_libraries` in `postgresql.conf`:

```
shared_preload_libraries = 'pg_llm_helper'
```

### 3. Restart PostgreSQL

```bash
sudo systemctl restart postgresql
# or
sudo pg_ctl restart -D /path/to/data/directory
```

### 4. Create the Extension in Your Database

```sql
CREATE EXTENSION pg_llm_helper;
```

### 5. (Optional) Install pgai for LLM Integration

```sql
-- Install pgai extension
CREATE EXTENSION ai CASCADE;

-- Set your OpenAI API key
SELECT set_config('ai.openai_api_key', 'your-api-key-here', false);

-- Or set it permanently for your user
ALTER USER your_username SET ai.openai_api_key = 'your-api-key-here';
```

## Usage

### Basic Error Capture

Errors are captured automatically. Just run queries as normal:

```sql
-- This will trigger a syntax error
SELECT * FORM users WHERE id = 1;
-- ERROR:  syntax error at or near "FORM"
```

### View Last Error

```sql
SELECT * FROM get_last_error();
```

Returns:
- `backend_pid` - Process ID of the backend
- `query_text` - The query that caused the error
- `error_message` - The error message
- `sql_state` - SQL state code
- `error_level` - Error severity level
- `timestamp` - When the error occurred

### Get LLM Help (requires pgai)

```sql
SELECT llm_help_last_error();
```

This will send your last error to an LLM and return an explanation and suggested fix.

### View Error History

```sql
-- Get last 10 errors (across all sessions)
SELECT * FROM get_error_history(10);

-- Get last 50 errors
SELECT * FROM get_error_history(50);
```

### Clear Error History

```sql
SELECT clear_error_history();
```

## Example Workflow

```sql
-- 1. Make a mistake
SELECT * FORM users WHERE id = 1;
-- ERROR:  syntax error at or near "FORM"

-- 2. Get instant AI help
SELECT llm_help_last_error();
```

Example output:
```
The error occurred because "FORM" is not a valid SQL keyword. You meant to use "FROM".

Corrected query:
SELECT * FROM users WHERE id = 1;

Tip: This is a common typo. Most SQL editors with syntax highlighting 
would catch this before execution.
```

## Configuration

The extension uses the following defaults:
- Maximum errors stored: 100 (circular buffer)
- Maximum query length: 8192 characters
- Maximum error message length: 1024 characters

To modify these, edit the `#define` constants in `pg_llm_helper.c` and rebuild.

## Customizing LLM Integration

The `llm_help_last_error()` function uses pgai's OpenAI integration by default. You can modify it to use:

- **Anthropic Claude**: Replace with `ai.anthropic_generate()`
- **Other LLM providers**: Modify the function to call your preferred API
- **Custom prompts**: Edit the function to adjust the prompt sent to the LLM

Example with Claude:

```sql
CREATE OR REPLACE FUNCTION llm_help_last_error_claude()
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
    
    SELECT ai.anthropic_generate(
        'claude-sonnet-4-20250514',
        jsonb_build_array(
            jsonb_build_object(
                'role', 'user',
                'content', format(
                    E'PostgreSQL error: %s\n\nQuery: %s\n\nExplain and fix.',
                    err.error_message,
                    err.query_text
                )
            )
        )
    )->'content'->0->>'text'
    INTO llm_response;
    
    RETURN llm_response;
END;
$$;
```

## Troubleshooting

### Extension won't load

Make sure you:
1. Added `pg_llm_helper` to `shared_preload_libraries`
2. Restarted PostgreSQL (reload is not enough)
3. Have correct permissions on the shared library

### Shared memory error on startup

If you see:
```
FATAL: cannot request additional shared memory outside shmem_request_hook
```

This means you're using an older version of the extension. Make sure you have the latest version that properly uses `shmem_request_hook`.

### Can't create extension

Check that:
1. The `.so` file is in PostgreSQL's `lib` directory
2. The `.control` and `.sql` files are in the `extension` directory
3. You have CREATE privileges on the database

### LLM functions don't work

Ensure:
1. pgai is installed: `CREATE EXTENSION ai CASCADE;`
2. API key is set correctly
3. You have network access to the LLM API

## Architecture

The extension works by:
1. Hooking into PostgreSQL's `emit_log_hook` to intercept all error messages
2. Storing errors in a shared memory circular buffer
3. Providing SQL functions to query the buffer
4. Integrating with pgai (or custom LLM APIs) for analysis

Errors are captured at the logging layer, which means it catches:
- Syntax errors
- Runtime errors
- Constraint violations
- Permission errors
- All other PostgreSQL errors

## License

This extension is provided as-is for educational and development purposes.

## Contributing

Feel free to modify and extend this extension for your needs. Some ideas:
- Add support for filtering errors by type
- Implement automatic error pattern detection
- Add metrics and monitoring integration
- Create a background worker for batch LLM analysis
