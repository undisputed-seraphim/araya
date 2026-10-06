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

## Runtime commands

| Command | Effect |
|---|---|
| `/config show` | print the resolved configuration and provenance |
| `/config reload` | re-read the config files and re-reconcile the tree |
| `/reload <id>` | retire and remount one component with its resolved config |
| `/log level <lvl>` | change the log level for this process |

Configuration is read from files only: there is no write-back. Edit the JSON
and run `/config reload` (or restart).
