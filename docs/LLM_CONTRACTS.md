# MayteraOS LLM Integration Contracts

## Overview

This document defines how LLM agents can interact with MayteraOS applications through the capability-based permission system. All LLM interactions are governed by temporal capability tokens that provide fine-grained access control with mandatory audit trails.

## Table of Contents

1. [Contract Architecture](#contract-architecture)
2. [Capability Token Format](#capability-token-format)
3. [Application Contracts](#application-contracts)
4. [IPC Protocol](#ipc-protocol)
5. [Security Model](#security-model)
6. [Examples](#examples)

---

## Contract Architecture

### Design Principles

1. **Least Privilege**: LLM agents receive only the minimum capabilities needed
2. **Temporal Bounds**: All capabilities have explicit expiration times
3. **Audit Trail**: Every capability use is logged for accountability
4. **User Consent**: Sensitive operations require explicit user approval
5. **Revocability**: Capabilities can be revoked at any time

### Capability Hierarchy

```
system.*                    - System-wide capabilities
├── system.settings.*       - Settings access
│   ├── system.settings.read
│   └── system.settings.write
├── system.audio.*          - Audio control
├── system.display.*        - Display settings
└── system.network.*        - Network configuration

app.*                       - Application-specific capabilities
├── app.terminal.*          - Terminal application
│   ├── app.terminal.execute
│   └── app.terminal.read_output
├── app.editor.*            - Text editor
├── app.files.*             - File browser
└── app.syslog.*            - System log viewer

fs.*                        - Filesystem capabilities
├── fs.read                 - Read files
├── fs.write                - Write files
├── fs.delete               - Delete files
└── fs.create               - Create files/directories

media.*                     - Media capabilities
└── media.playback          - Media playback control
```

---

## Capability Token Format

### Token Structure

```json
{
  "token_id": "cap_1234567890abcdef",
  "version": "1.0",
  "issued_at": 1706620800,
  "expires_at": 1706624400,
  "capabilities": [
    "app.terminal.execute",
    "fs.read"
  ],
  "constraints": {
    "max_uses": 10,
    "allowed_paths": ["/home/*"],
    "denied_commands": ["rm -rf", "shutdown"]
  },
  "audit_tag": "Code compilation task",
  "issuer": "maytera.capability.service",
  "signature": "base64_signature_here"
}
```

### Token Fields

| Field | Type | Description |
|-------|------|-------------|
| `token_id` | string | Unique identifier for the token |
| `version` | string | Contract version (currently "1.0") |
| `issued_at` | uint64 | Unix timestamp when token was issued |
| `expires_at` | uint64 | Unix timestamp when token expires |
| `capabilities` | array | List of granted capability strings |
| `constraints` | object | Additional restrictions on capability use |
| `max_uses` | int | Maximum number of times token can be used |
| `audit_tag` | string | Human-readable purpose for logging |
| `issuer` | string | Service that issued the token |
| `signature` | string | Cryptographic signature for verification |

---

## Application Contracts

### Terminal

The Terminal application provides command execution capabilities.

**Manifest**: `/apps/terminal/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `app.terminal.execute` | Execute shell commands | HIGH |
| `app.terminal.read_output` | Read command output | MEDIUM |
| `app.terminal.write_input` | Write to command stdin | MEDIUM |
| `app.terminal.history` | Access command history | LOW |

#### Methods

##### execute_command
Execute a shell command in the terminal.

```json
{
  "action": "execute",
  "app": "terminal",
  "method": "execute_command",
  "params": {
    "command": "ls -la /home",
    "timeout": 30000,
    "capture_output": true
  },
  "capability_token": "cap_..."
}
```

**Parameters:**
- `command` (string, required): The command to execute
- `timeout` (int, optional): Timeout in milliseconds (default: 30000)
- `capture_output` (bool, optional): Whether to return stdout/stderr

**Returns:**
```json
{
  "status": "success",
  "exit_code": 0,
  "stdout": "drwxr-xr-x 5 user user 4096 Jan 30 10:00 .\n...",
  "stderr": "",
  "duration_ms": 150
}
```

**Required Capability:** `app.terminal.execute`

##### get_cwd
Get current working directory.

```json
{
  "action": "query",
  "app": "terminal",
  "method": "get_cwd",
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "cwd": "/home/user"
}
```

**Required Capability:** `app.terminal.read_output`

---

### File Browser (Files)

The Files application provides filesystem navigation and management.

**Manifest**: `/apps/files/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `fs.read` | Read file contents | LOW |
| `fs.write` | Write/modify files | HIGH |
| `fs.delete` | Delete files | HIGH |
| `fs.create` | Create files/directories | MEDIUM |
| `app.files.navigate` | Navigate directories | LOW |

#### Methods

##### list_directory
List contents of a directory.

```json
{
  "action": "query",
  "app": "files",
  "method": "list_directory",
  "params": {
    "path": "/home/documents"
  },
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "path": "/home/documents",
  "entries": [
    {
      "name": "notes.txt",
      "type": "file",
      "size": 1024,
      "modified": 1706620800,
      "permissions": "rw-r--r--"
    },
    {
      "name": "projects",
      "type": "directory",
      "size": 4096,
      "modified": 1706620000,
      "permissions": "rwxr-xr-x"
    }
  ],
  "count": 2
}
```

**Required Capability:** `fs.read`

##### read_file
Read contents of a file.

```json
{
  "action": "query",
  "app": "files",
  "method": "read_file",
  "params": {
    "path": "/home/documents/notes.txt",
    "offset": 0,
    "length": 4096
  },
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "path": "/home/documents/notes.txt",
  "content": "File content here...",
  "size": 1024,
  "encoding": "utf-8"
}
```

**Required Capability:** `fs.read`

##### write_file
Write content to a file.

```json
{
  "action": "execute",
  "app": "files",
  "method": "write_file",
  "params": {
    "path": "/home/documents/new_file.txt",
    "content": "Hello, World\!",
    "mode": "overwrite"
  },
  "capability_token": "cap_..."
}
```

**Parameters:**
- `path` (string, required): File path
- `content` (string, required): Content to write
- `mode` (string, optional): "overwrite" or "append" (default: "overwrite")

**Returns:**
```json
{
  "status": "success",
  "bytes_written": 13
}
```

**Required Capability:** `fs.write`

##### delete_file
Delete a file or directory.

```json
{
  "action": "execute",
  "app": "files",
  "method": "delete_file",
  "params": {
    "path": "/home/documents/old_file.txt"
  },
  "capability_token": "cap_..."
}
```

**Required Capability:** `fs.delete`

---

### Settings

The Settings application provides system configuration access.

**Manifest**: `/apps/settings/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `system.settings.read` | Read settings values | LOW |
| `system.settings.write` | Modify settings | MEDIUM |
| `system.appearance.read` | Read appearance settings | LOW |
| `system.appearance.write` | Modify appearance | LOW |
| `system.audio.read` | Read audio settings | LOW |
| `system.audio.write` | Modify audio | LOW |
| `system.network.read` | Read network settings | LOW |
| `system.network.write` | Modify network | HIGH |

#### Methods

##### get_setting
Read a setting value.

```json
{
  "action": "query",
  "app": "settings",
  "method": "get_setting",
  "params": {
    "category": "appearance",
    "key": "theme"
  },
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "category": "appearance",
  "key": "theme",
  "value": "dark",
  "type": "string"
}
```

**Required Capability:** `system.settings.read` or category-specific read capability

##### set_setting
Change a setting value.

```json
{
  "action": "execute",
  "app": "settings",
  "method": "set_setting",
  "params": {
    "category": "appearance",
    "key": "theme",
    "value": "light"
  },
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "status": "success",
  "previous_value": "dark",
  "new_value": "light"
}
```

**Required Capability:** `system.settings.write` or category-specific write capability

#### Setting Categories

##### Appearance
- `theme` (string): "dark", "light", "classic", "ocean"
- `accent_color` (string): "blue", "green", "orange", "purple", "red"
- `font_size` (string): "small", "medium", "large"
- `animations_enabled` (bool)
- `transparency_enabled` (bool)

##### Display
- `brightness` (int): 0-100
- `resolution` (string): "1920x1080", "1280x720", etc.
- `refresh_rate` (int): 60, 75, 120, etc.
- `night_light` (bool)

##### Sound
- `master_volume` (int): 0-100
- `input_volume` (int): 0-100
- `output_device` (string)
- `input_device` (string)
- `sound_effects` (bool)

##### Network
- `dhcp_enabled` (bool)
- `wifi_enabled` (bool)
- `ip_address` (string)
- `gateway` (string)
- `dns_servers` (array)

##### Keyboard
- `layout` (string): "us", "uk", "de", "fr"
- `repeat_rate` (string): "slow", "normal", "fast"
- `repeat_delay` (string): "short", "normal", "long"

##### DateTime
- `timezone` (string)
- `use_24hour` (bool)
- `auto_time` (bool)

---

### Editor

The Editor application provides text editing capabilities.

**Manifest**: `/apps/editor/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `app.editor.read` | Read editor buffer | LOW |
| `app.editor.write` | Modify editor buffer | MEDIUM |
| `app.editor.file_open` | Open files in editor | MEDIUM |
| `app.editor.file_save` | Save files from editor | MEDIUM |

#### Methods

##### open_file
Open a file in the editor.

```json
{
  "action": "execute",
  "app": "editor",
  "method": "open_file",
  "params": {
    "path": "/home/documents/code.c"
  },
  "capability_token": "cap_..."
}
```

**Required Capability:** `app.editor.file_open`

##### get_buffer
Get current editor buffer contents.

```json
{
  "action": "query",
  "app": "editor",
  "method": "get_buffer",
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "content": "int main() {\n    return 0;\n}",
  "filename": "code.c",
  "modified": true,
  "line_count": 3,
  "cursor_line": 2,
  "cursor_col": 4
}
```

**Required Capability:** `app.editor.read`

##### set_buffer
Replace editor buffer contents.

```json
{
  "action": "execute",
  "app": "editor",
  "method": "set_buffer",
  "params": {
    "content": "// Modified content\nint main() { return 0; }"
  },
  "capability_token": "cap_..."
}
```

**Required Capability:** `app.editor.write`

##### save_file
Save current buffer to file.

```json
{
  "action": "execute",
  "app": "editor",
  "method": "save_file",
  "params": {
    "path": "/home/documents/code.c"
  },
  "capability_token": "cap_..."
}
```

**Required Capability:** `app.editor.file_save`

---

### Calculator

The Calculator application provides mathematical operations.

**Manifest**: `/apps/calc/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `app.calc.compute` | Perform calculations | LOW |
| `app.calc.read` | Read current value | LOW |

#### Methods

##### compute
Perform a calculation.

```json
{
  "action": "execute",
  "app": "calc",
  "method": "compute",
  "params": {
    "expression": "123 + 456 * 2"
  },
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "result": 1035,
  "expression": "123 + 456 * 2"
}
```

**Required Capability:** `app.calc.compute`

##### get_value
Get current calculator display value.

```json
{
  "action": "query",
  "app": "calc",
  "method": "get_value",
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "value": 1035,
  "display": "1035"
}
```

**Required Capability:** `app.calc.read`

---

### System Log (Syslog)

The Syslog application provides access to system logs.

**Manifest**: `/apps/syslog/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `app.syslog.read` | Read log entries | LOW |
| `app.syslog.filter` | Filter log entries | LOW |

#### Methods

##### get_logs
Retrieve log entries.

```json
{
  "action": "query",
  "app": "syslog",
  "method": "get_logs",
  "params": {
    "count": 100,
    "offset": 0,
    "severity": "all"
  },
  "capability_token": "cap_..."
}
```

**Parameters:**
- `count` (int, optional): Number of entries (default: 100)
- `offset` (int, optional): Start offset (default: 0)
- `severity` (string, optional): "all", "info", "warn", "error", "ok"

**Returns:**
```json
{
  "entries": [
    {
      "timestamp": 1706620800,
      "severity": "info",
      "message": "[INFO] MayteraOS System Log Viewer"
    },
    {
      "timestamp": 1706620801,
      "severity": "ok",
      "message": "[OK] Kernel initialized successfully"
    }
  ],
  "total": 150,
  "returned": 100
}
```

**Required Capability:** `app.syslog.read`

---

### Solitaire

The Solitaire game application.

**Manifest**: `/apps/solitaire/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `app.solitaire.play` | Interact with game | LOW |
| `app.solitaire.read` | Read game state | LOW |

#### Methods

##### get_game_state
Get current game state.

```json
{
  "action": "query",
  "app": "solitaire",
  "method": "get_game_state",
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "in_progress": true,
  "moves": 42,
  "time_elapsed": 180,
  "cards_remaining": 24
}
```

**Required Capability:** `app.solitaire.read`

##### new_game
Start a new game.

```json
{
  "action": "execute",
  "app": "solitaire",
  "method": "new_game",
  "capability_token": "cap_..."
}
```

**Required Capability:** `app.solitaire.play`

---

### Python Interpreter

The Python (MicroPython) application provides scripting capabilities.

**Manifest**: `/apps/python/manifest.json`

#### Capabilities

| Capability | Description | Risk Level |
|------------|-------------|------------|
| `app.python.execute` | Execute Python code | HIGH |
| `app.python.read_output` | Read execution output | MEDIUM |

#### Methods

##### execute_script
Execute Python code.

```json
{
  "action": "execute",
  "app": "python",
  "method": "execute_script",
  "params": {
    "code": "print(Hello from Python!)\nresult = 2 + 2\nprint(fResult: {result})",
    "timeout": 5000
  },
  "capability_token": "cap_..."
}
```

**Returns:**
```json
{
  "status": "success",
  "output": "Hello from Python\!\nResult: 4\n",
  "error": null,
  "duration_ms": 50
}
```

**Required Capability:** `app.python.execute`

##### execute_file
Execute a Python script file.

```json
{
  "action": "execute",
  "app": "python",
  "method": "execute_file",
  "params": {
    "path": "/home/scripts/hello.py"
  },
  "capability_token": "cap_..."
}
```

**Required Capabilities:** `app.python.execute`, `fs.read`

---

## IPC Protocol

### Message Format

All LLM-to-application communication uses the MayteraOS IPC system.

```json
{
  "header": {
    "msg_type": "llm_request",
    "version": "1.0",
    "request_id": "req_abc123",
    "timestamp": 1706620800
  },
  "body": {
    "action": "execute",
    "app": "terminal",
    "method": "execute_command",
    "params": {...},
    "capability_token": "cap_..."
  }
}
```

### Response Format

```json
{
  "header": {
    "msg_type": "llm_response",
    "version": "1.0",
    "request_id": "req_abc123",
    "timestamp": 1706620801
  },
  "body": {
    "status": "success",
    "result": {...}
  }
}
```

### Error Response

```json
{
  "header": {
    "msg_type": "llm_response",
    "version": "1.0",
    "request_id": "req_abc123",
    "timestamp": 1706620801
  },
  "body": {
    "status": "error",
    "error_code": "CAPABILITY_DENIED",
    "error_message": "Capability fs.write not granted in token",
    "details": {
      "required_capability": "fs.write",
      "token_capabilities": ["fs.read"]
    }
  }
}
```

### Error Codes

| Code | Description |
|------|-------------|
| `CAPABILITY_DENIED` | Token lacks required capability |
| `TOKEN_EXPIRED` | Capability token has expired |
| `TOKEN_EXHAUSTED` | Token max_uses exceeded |
| `INVALID_TOKEN` | Token signature verification failed |
| `INVALID_REQUEST` | Malformed request |
| `APP_NOT_FOUND` | Target application not running |
| `METHOD_NOT_FOUND` | Unknown method name |
| `EXECUTION_FAILED` | Method execution error |
| `TIMEOUT` | Operation timed out |

---

## Security Model

### Capability Request Flow

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   LLM Agent     │    │ Capability Svc  │    │      User       │
└────────┬────────┘    └────────┬────────┘    └────────┬────────┘
         │                      │                      │
         │ 1. Request capability│                      │
         │─────────────────────>│                      │
         │                      │ 2. Prompt for consent│
         │                      │─────────────────────>│
         │                      │                      │
         │                      │ 3. User approves     │
         │                      │<─────────────────────│
         │                      │                      │
         │ 4. Issue token       │                      │
         │<─────────────────────│                      │
         │                      │                      │
```

### Sensitive Operations

The following operations always require user consent:

1. **File deletion** (`fs.delete`)
2. **Command execution** (`app.terminal.execute`)
3. **Python code execution** (`app.python.execute`)
4. **Network configuration** (`system.network.write`)
5. **System settings changes** (`system.settings.write`)

### Audit Logging

All capability uses are logged:

```json
{
  "timestamp": 1706620800,
  "token_id": "cap_1234567890abcdef",
  "action": "execute",
  "app": "terminal",
  "method": "execute_command",
  "params_hash": "sha256:...",
  "result": "success",
  "audit_tag": "Code compilation task"
}
```

---

## Examples

### Example 1: Requesting Capability

LLM requests capability to execute terminal commands:

```json
{
  "action": "request_capability",
  "capabilities": ["app.terminal.execute"],
  "duration": 3600,
  "max_uses": 10,
  "reason": "Run build command for user project",
  "constraints": {
    "allowed_commands": ["make", "gcc", "ls"],
    "denied_patterns": ["rm -rf", "sudo"]
  }
}
```

### Example 2: Executing Command

After obtaining capability token:

```json
{
  "action": "execute",
  "app": "terminal",
  "method": "execute_command",
  "params": {
    "command": "make all",
    "timeout": 60000
  },
  "capability_token": "cap_1234567890abcdef"
}
```

### Example 3: Reading and Modifying Settings

```json
{
  "action": "query",
  "app": "settings",
  "method": "get_setting",
  "params": {
    "category": "appearance",
    "key": "theme"
  },
  "capability_token": "cap_..."
}

{
  "action": "execute",
  "app": "settings",
  "method": "set_setting",
  "params": {
    "category": "appearance",
    "key": "theme",
    "value": "dark"
  },
  "capability_token": "cap_..."
}
```

### Example 4: File Operations

```json
{
  "action": "query",
  "app": "files",
  "method": "list_directory",
  "params": {
    "path": "/home/user/projects"
  },
  "capability_token": "cap_..."
}

{
  "action": "query",
  "app": "files",
  "method": "read_file",
  "params": {
    "path": "/home/user/projects/config.json"
  },
  "capability_token": "cap_..."
}
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-01-30 | Initial release |

---

*Document maintained by P25 - Documentation Lead*
*MayteraOS v1.8.0*
