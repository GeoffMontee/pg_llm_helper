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
- **pgvector extension** (required dependency)
- **pgai extension** (optional, for LLM integration)
- **Python 3.12 or earlier** (pgai is not yet compatible with Python 3.13)

## Installation

### 1. Install Prerequisites

#### pgvector (Required)

```bash
# For Debian/Ubuntu
apt-get update
apt-get install -y postgresql-17-pgvector

# Or build from source
git clone https://github.com/pgvector/pgvector.git
cd pgvector
make
sudo make install
```

#### pgai (Optional - for LLM-powered error analysis)

pgai provides SQL functions to call LLMs like OpenAI, Anthropic, etc. The `llm_help_last_error()` function requires pgai.

**Important:** pgai requires Python 3.12 or earlier. If you're using Debian 13 (Trixie) with Python 3.13, use Debian 12 (Bookworm) or the Timescale Docker image instead.

**Option A: Use Timescale Docker Image (Easiest)**

The Timescale image includes PostgreSQL 17, pgvector, and pgai pre-installed:

```bash
docker run -d \
  --name postgres-pgai \
  -e POSTGRES_PASSWORD=mypassword \
  -p 5432:5432 \
  timescale/timescaledb-ha:pg17

# Enable the ai extension
docker exec -it postgres-pgai psql -U postgres -c "CREATE EXTENSION ai CASCADE;"
```

**Option B: Install pgai from Source**

```bash
# Install dependencies (Debian/Ubuntu with Python 3.12 or earlier)
apt-get update
apt-get install -y \
    git \
    build-essential \
    postgresql-server-dev-17 \
    postgresql-plpython3-17 \
    python3 \
    python3-pip

# Clone pgai repository
cd /tmp
git clone https://github.com/timescale/pgai.git --branch extension-0.11.1
cd pgai

# Install the extension
python3 projects/extension/build.py install

# Install Python dependencies where plpython3u can find them
# (Adjust Python version as needed - check with: python3 --version)
pip3 install --target /usr/local/lib/python3.12/dist-packages \
    openai \
    anthropic \
    cohere

# Clean up
cd /
rm -rf /tmp/pgai

# Restart PostgreSQL
sudo systemctl restart postgresql
# or for Docker:
# docker restart your-container-name
```

**Important Note on Python Path:**

plpython3u (used by pgai) needs to find the Python packages. If you get "ModuleNotFoundError: No module named 'openai'" errors:

1. Check plpython3u's Python path:
   ```sql
   CREATE FUNCTION show_python_path()
   RETURNS text[]
   LANGUAGE plpython3u
   AS $$
   import sys
   return sys.path
   $$;
   
   SELECT show_python_path();
   ```

2. Install packages to one of those directories:
   ```bash
   # Example for Python 3.12
   pip3 install --target /usr/local/lib/python3.12/dist-packages \
       openai anthropic cohere
   ```

3. Or set PYTHONPATH when starting PostgreSQL (Docker):
   ```bash
   docker run -d \
     -e PYTHONPATH=/usr/local/lib/pgai/0.11.1:/usr/local/lib/python3.12/dist-packages \
     ...
   ```

### 2. Build and Install pg_llm_helper Extension

### 2. Build and Install pg_llm_helper Extension

```bash
# Navigate to the extension directory
cd pg_llm_helper

# Build the extension
make

# Install (requires sudo/root)
sudo make install
```

### 3. Configure PostgreSQL

Add the extension to `shared_preload_libraries` in `postgresql.conf`:

```
shared_preload_libraries = 'pg_llm_helper'
```

### 4. Restart PostgreSQL

```bash
sudo systemctl restart postgresql
# or
sudo pg_ctl restart -D /path/to/data/directory
# or for Docker:
docker restart your-container-name
```

### 5. Create Extensions in Your Database

```sql
-- Create pgvector (required)
CREATE EXTENSION IF NOT EXISTS vector;

-- Create pg_llm_helper
CREATE EXTENSION pg_llm_helper;

-- Create pgai (optional, for LLM features)
CREATE EXTENSION IF NOT EXISTS ai CASCADE;
```

### 6. Configure API Keys (If Using pgai)

```sql
-- Set your OpenAI API key
ALTER USER postgres SET ai.openai_api_key = 'sk-your-api-key-here';

-- Or for Anthropic Claude
ALTER USER postgres SET ai.anthropic_api_key = 'sk-ant-your-key-here';

-- Or set for current session only
SELECT set_config('ai.openai_api_key', 'sk-your-key-here', false);
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

### Shared memory error on startup

If you see:
```
FATAL: cannot request additional shared memory outside shmem_request_hook
```

This means you're using an older version of the extension. Make sure you have the latest version that properly uses `shmem_request_hook`.

### Python module not found errors

If you see errors like:
```
ModuleNotFoundError: No module named 'openai'
```

This means plpython3u cannot find the Python packages. Solutions:

1. **Check Python path:**
   ```sql
   CREATE FUNCTION show_python_path()
   RETURNS text[]
   LANGUAGE plpython3u
   AS $$
   import sys
   return sys.path
   $$;
   
   SELECT show_python_path();
   ```

2. **Install packages to the correct location:**
   ```bash
   # Find Python version
   python3 --version
   
   # Install to dist-packages (adjust version as needed)
   pip3 install --target /usr/local/lib/python3.12/dist-packages \
       openai anthropic cohere
   
   # Verify installation
   ls /usr/local/lib/python3.12/dist-packages/openai
   
   # Restart PostgreSQL
   docker restart your-container-name
   ```

3. **Set PYTHONPATH (Docker):**
   ```bash
   docker run -d \
     -e PYTHONPATH=/usr/local/lib/pgai/0.11.1:/usr/local/lib/python3.12/dist-packages \
     ...
   ```

### Python 3.13 compatibility

pgai does not currently support Python 3.13. If you're using:
- **Debian 13 (Trixie)**: Use Debian 12 (Bookworm) image instead
- **Docker**: Use `timescale/timescaledb-ha:pg17` which includes everything pre-configured
- **From scratch**: Install Python 3.12 instead of 3.13

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

## Complete Docker Setup Example

Here's a complete Dockerfile that includes PostgreSQL 17, pgvector, pgai, and pg_llm_helper:

```dockerfile
FROM postgres:17-bookworm

# Install dependencies
RUN apt-get update && apt-get install -y \
    postgresql-17-pgvector \
    postgresql-plpython3-17 \
    postgresql-server-dev-17 \
    python3 \
    python3-pip \
    git \
    build-essential \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install pgai extension
RUN cd /tmp && \
    git clone https://github.com/timescale/pgai.git --branch extension-0.11.1 && \
    cd pgai && \
    python3 projects/extension/build.py install && \
    cd / && \
    rm -rf /tmp/pgai

# Install Python packages for pgai
RUN pip3 install --break-system-packages --target /usr/local/lib/python3.11/dist-packages \
    openai \
    anthropic \
    cohere

# Copy pg_llm_helper extension files
COPY pg_llm_helper/pg_llm_helper.c /tmp/
COPY pg_llm_helper/pg_llm_helper--1.0.sql /tmp/
COPY pg_llm_helper/pg_llm_helper.control /tmp/
COPY pg_llm_helper/Makefile /tmp/

# Build and install pg_llm_helper
RUN cd /tmp && \
    make && \
    make install && \
    rm -rf /tmp/*

# Configure shared_preload_libraries
RUN echo "shared_preload_libraries = 'pg_llm_helper'" >> /usr/share/postgresql/postgresql.conf.sample
```

Build and run:

```bash
docker build -t postgres-ai .

docker run -d \
  --name postgres-ai \
  -e POSTGRES_PASSWORD=mypassword \
  -e POSTGRES_DB=mydb \
  -p 5432:5432 \
  -v pgdata:/var/lib/postgresql/data \
  postgres-ai
```

Initialize:

```bash
docker exec -it postgres-ai psql -U postgres -d mydb << EOF
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION pg_llm_helper;
CREATE EXTENSION ai CASCADE;
ALTER USER postgres SET ai.openai_api_key = 'your-api-key-here';
EOF
```

## Quick Start with Timescale Image

Alternatively, use the Timescale image (easiest option):

```bash
# Start Timescale container (includes pgvector and pgai)
docker run -d \
  --name postgres-complete \
  -e POSTGRES_PASSWORD=mypassword \
  -p 5432:5432 \
  -v pgdata:/var/lib/postgresql/data \
  timescale/timescaledb-ha:pg17

# Copy and install pg_llm_helper
docker cp pg_llm_helper.tar.gz postgres-complete:/tmp/
docker exec -it postgres-complete bash -c "
  cd /tmp && \
  tar -xzf pg_llm_helper.tar.gz && \
  cd pg_llm_helper && \
  make && make install && \
  echo \"shared_preload_libraries = 'pg_llm_helper'\" >> /var/lib/postgresql/data/postgresql.conf
"

# Restart and initialize
docker restart postgres-complete

docker exec -it postgres-complete psql -U postgres << EOF
CREATE EXTENSION pg_llm_helper;
CREATE EXTENSION ai CASCADE;
ALTER USER postgres SET ai.openai_api_key = 'your-api-key-here';
EOF
```

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
