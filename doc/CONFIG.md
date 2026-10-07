# Configuration

Araya is configured from four places, from lowest to highest precedence:

1. **Built-in defaults** compiled into each plugin.
2. **Home config** — `$XDG_CONFIG_HOME/araya/config.json` (default
   `~/.config/araya/config.json`): machine-level preferences that apply to
   every project.
3. **Project config** — `./araya.json` in the working directory: settings that
   belong to one checkout.
4. **Explicit overlays** — each `--config <path>` (repeatable) and
   `$ARAYA_CONFIG`.
5. **Environment variables** — `ARAYA_*` (see below).
6. **Command-line flags** — `--log-level`, `--log-file`, `--llm-config`.

Layers merge **per key**: a higher layer overrides only the keys it names and
leaves the rest untouched. A missing file is not an error; the layer simply
contributes nothing. Within `components`, each id merges its keys; within
`log`/`state`/`llm`, each field is overridden individually.

Everything is JSON. All plugin values are ultimately strings; JSON numbers and
booleans are stringified (`true`/`false`, decimal) on the way in, and arrays or
objects are serialized compactly (useful for a plugin that takes inline JSON).

## File locations

| Path | Role |
|---|---|
| `$XDG_CONFIG_HOME/araya/config.json` | home config (all projects) |
| `./araya.json` | project config (current working directory) |
| `--config <path>` / `$ARAYA_CONFIG` | extra overlay(s), applied above the above |
| `$XDG_STATE_HOME/araya/sessions/` | persisted sessions (default state) |
| `$XDG_STATE_HOME/araya/attachments/` | attachment store |
| `$XDG_STATE_HOME/araya/logs/araya.log` | rotating log |

`$XDG_CONFIG_HOME` defaults to `~/.config`, `$XDG_STATE_HOME` to
`~/.local/state`; when neither the environment nor `$HOME` is available the
state base falls back to the working directory. `ARAYA_STATE_DIR` overrides the
state base. Paths in the config may use `~`; relative paths resolve against the
state base, absolute paths are used verbatim.

## File shape

```jsonc
{
  "log":   { "file": "logs/araya.log", "level": "info" },
  "state": { "sessions": "sessions", "attachments": "attachments" },
  "llm":   { "config_file": "~/llm.json" },   // or an inline provider object
  "mcp":   { "servers": { "github": { "type": "local", "command": ["github-mcp-server", "stdio"] } } },
  "lsp":   { "servers": { "typescript": { "command": "typescript-language-server", "args": ["--stdio"], "extensionToLanguage": { ".ts": "typescript" } } } },
  "disabled": { "tool-web": true },
  "components": {
    "system-prompt": { "persona_prefix": "You are a coding agent." },
    "shell":         { "timeout_ms": "30000" }
  }
}
```

- `log.file`, `log.level` — the rotating log path and level
  (`error|warn|info|debug`).
- `state.sessions`, `state.attachments` — roots for the persistence and
  attachment backends.
- `llm` — either `config_file` (a path to the OpenAI-compatible provider JSON,
  same as `--llm-config`) or `openai` (the provider object inline). The
  `--llm-config` flag and `$ARAYA_LLM_CONFIG` still win.
- `mcp` — external Model Context Protocol servers (see [MCP servers](#mcp-servers)).
  Absent, no MCP client is mounted.
- `lsp` — local language servers for the `lsp` tool (see [Language servers](#language-servers)).
  Absent, no LSP stack is mounted.
- `disabled` — a per-component enable switch, merged per id: `true` drops the
  component, `false` re-enables one a lower layer dropped. Kept separate from
  `components` because some plugins take a `disabled` knob of their own.
- `components` — one block per component id. Keys are that plugin's config
  knobs (see below).
- A component id must name a component this build knows; an unknown id is an
  error.

## Environment

| Variable | Effect |
|---|---|
| `ARAYA_CONFIG` | an extra config overlay path |
| `ARAYA_STATE_DIR` | overrides the state base directory |
| `ARAYA_LOG_FILE` | overrides `log.file` |
| `ARAYA_LOG_LEVEL` | overrides `log.level` |
| `ARAYA_LLM_CONFIG` | overrides `llm` with a provider config file |
| `ARAYA_SYSTEM_PROMPT` | seeds `system-prompt.persona_prefix` |
| `ARAYA_TUI_ASCII` | ASCII glyph tier in the TUI |

## Flags

`araya [--config <path>] [--log-level <lvl>] [--log-file <path>] [--print-config] <run|tui> …`

| Flag | Effect |
|---|---|
| `--config <path>` | extra config overlay (repeatable, applied in order) |
| `--log-level <lvl>` | `error|warn|info|debug` |
| `--log-file <path>` | rotating log path |
| `--print-config` | print the resolved configuration (values + provenance + every accepted key) and exit |

`run` additionally accepts `--llm-config <path>`; `tui` accepts
`-s|--session <id>`.

## Plugin knobs

Every plugin declares the keys it accepts, their types, defaults, and whether
they are required. The app validates each `components` block against that
declaration: a malformed value for a known key refuses boot (the same
`config_error` the plugin would raise); an unknown key logs a warning and boots
anyway, so an overlay survives a plugin-set change.

`araya --print-config` prints the full, always-current reference (accepted
keys, types, defaults, and where the effective value came from). The plugin
source is the origin of truth; the declaration sits beside the plugin's
descriptor.

For example, `tool-web` takes `search` and `fetch` booleans (both default
`true`) that register the model-facing `web_search` and `web_fetch` tools.
Registration is independent of whether a search endpoint is configured: with
none, `web_search` is still visible and fails with a structured error at call
time. Disable one with `"components": { "tool-web": { "search": false } }`.

## MCP servers

The `mcp` block configures external Model Context Protocol servers. Each named
server is a `local` (stdio child process) or `remote` (streamable HTTP)
connection; its tools register into the agent as
`mcp__<server>__<toolName>`, and the server's `instructions` join the system
prompt. The block is mounted as one `mcp-client` component; when it is absent,
no MCP client runs. Configured servers also enable the shared resource tools
(`list_mcp_resources`, `list_mcp_resource_templates`, `read_mcp_resource`) and
the `MCP resource servers` prompt section.

```jsonc
"mcp": {
  "servers": {
    "github": {
      "type": "local",
      "command": ["github-mcp-server", "stdio"],
      "environment": { "GITHUB_TOKEN": "${GITHUB_TOKEN}" },
      "enabled": true
    }
  },
  "requestTimeoutMs": 60000,
  "maxInstructionBytes": 32768,
  "reconnect": { "enabled": true, "initialDelayMs": 500, "maxDelayMs": 30000, "maxAttempts": 10 }
}
```

| Field | Default | Meaning |
|---|---|---|
| `servers` | `{}` | map of server name → server object; the name is the tool namespace (`[A-Za-z0-9_-]{1,32}`) |
| `type` | `local` | `local` (stdio) or `remote` (streamable HTTP) |
| `command` | — | local: program + arguments (argv); a bare name is resolved against `PATH` |
| `environment` | `{}` | local: extra child env vars, merged over the ambient environment after credential-shaped and `ARAYA_*` names are scrubbed |
| `cwd` | — | local: child working directory |
| `url` / `headers` | — | remote: endpoint URL and extra request headers |
| `enabled` | `true` | a `false` server is not connected |
| `timeoutMs` | `requestTimeoutMs` | per-request timeout for this server |
| `maxInstructionBytes` | `maxInstructionBytes` | reject a connection whose instructions exceed this |
| `failOnStartupError` | `false` | reject `mcp-client` activation when the initial connection fails |
| `reconnect` | shared | `{enabled, initialDelayMs, maxDelayMs, maxAttempts}`; delays double up to the ceiling |

`${NAME}` references in strings expand from the process environment.

## Language servers

The `lsp` block configures local language servers for the model-facing `lsp`
tool (definitions, references, implementations, hover). Each named server maps
file extensions to LSP language ids and is pooled one process per workspace.
The block mounts the `lsp` seam, the `lsp-stdio` provider, and the `tool-lsp`
tool together; when absent, no LSP stack runs.

```jsonc
"lsp": {
  "servers": {
    "typescript": {
      "command": "typescript-language-server",
      "args": ["--stdio"],
      "extensionToLanguage": { ".ts": "typescript", ".tsx": "typescriptreact" },
      "env": { "NODE_OPTIONS": "--max-old-space-size=4096" }
    }
  }
}
```

| Field | Default | Meaning |
|---|---|---|
| `command` | required | Executable to spawn — absolute, or resolved on `PATH` at launch |
| `args` | `[]` | Arguments passed to the executable |
| `extensionToLanguage` | required | lowercase leading-dot extension → LSP language id |
| `env` | `{}` | Extra child env vars, merged over the ambient environment after credential-shaped and `ARAYA_*` names are scrubbed |
| `initializationOptions` | `null` | Static `initialize` options forwarded to the server |
| `configuration` | `null` | Static answer to every `workspace/configuration` item |
| `maxMessageBytes` / `maxStderrBytes` / `maxDocumentBytes` | 16000000 / 1000000 / 4000000 | framing, stderr-tail, and source-size caps |
| `shutdownTimeoutMs` / `killGraceMs` | 5000 / 2000 | graceful-shutdown and SIGTERM→SIGKILL budgets |
| `requestTimeoutMs` | 30000 | per-request timeout (Araya addition; prevents a wedged server from hanging a query) |

Positions from the model are one-based line and character (UTF-16); the tool
converts them to the protocol's zero-based coordinates. `findReferences`
always includes the declaration. `${NAME}` references expand from the process
environment.

## Runtime commands

| Command | Effect |
|---|---|
| `/config show` | print the resolved configuration and provenance |
| `/config reload` | re-read the config files and re-reconcile the tree |
| `/reload <id>` | retire and remount one component with its resolved config |
| `/log level <lvl>` | change the log level for this process |

Configuration is read from files only: there is no write-back. Edit the JSON
and run `/config reload` (or restart).
