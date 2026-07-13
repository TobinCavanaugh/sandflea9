# WASM Program Permissions Specification

## Overview

Every WASM program in sandfleaOS runs with an explicit set of capabilities defined in a sidecar permissions file. This file controls which kernel functions the program can call, which files it can access, and what resource limits apply.

No permissions file = program cannot run. This is fail-closed security.

---

## 1. File Location & Naming

Permissions files mirror the program's path under a `/perms/` root, with the program's content hash appended.

```
Program:    /user/programs/prog.wasm
Hash:       0x7c0a1b3f...
Perms:      /perms/user/programs/prog.wasm:0x7c0a1b3f...
```

The `:` is treated as a literal filename character (valid in ext2). The hash is the **first 32 hex characters** (128 bits) of the SHA-256 digest of the raw `.wasm` file bytes. 128 bits provides strong preimage resistance (2^128 work factor) while keeping filenames under ext2's 255-byte limit.

### Why path + hash?

- **Path**: human-readable, easy to locate and edit
- **Hash**: prevents a renamed/replaced program from inheriting another's permissions
- **Combined**: you can have different permission sets for different versions of the same program

### Security: The "Squatter" Attack (and Why It's Prevented)

A concern: what if a WASM program is deleted, but its permissions file remains? Could an attacker create a malicious WASM at the same path and inherit the old permissions?

**No.** The 128-bit hash in the filename prevents this:

1. The perm file is named `/perms/user/programs/oldapp.wasm:0xaaaa...`
2. User deletes `/user/programs/oldapp.wasm` but forgets to delete the perm file
3. Attacker creates `/user/programs/oldapp.wasm` with malicious code
4. Loader computes SHA-256 of the attacker's WASM → `0xbbbb...`
5. `0xbbbb...` ≠ `0xaaaa...` in the perm filename → **rejected**

The attacker would need to craft a WASM file whose SHA-256 prefix matches `0xaaaa...` — a 128-bit preimage attack requiring ~2^128 operations. This is cryptographically infeasible.

**The one exception**: the wildcard hash `0x0000...` used during development (§2). In production builds (`PERM_ENFORCE_STRICT`), the wildcard is rejected entirely, closing this gap.

**Information leak**: orphaned perm files DO reveal what programs were previously installed and what permissions they had. Use `permclean` (§11) to remove them.

### Perm Files Do NOT Auto-Move

Moving or renaming a WASM program does **not** automatically move its permissions file. This is intentional:

- VFS operations (`mv`, `rename`) stay simple and predictable
- A program's permissions are tied to its *location*, not its identity
- Moving a program to a new path is a deliberate act that should require deliberate permission setup
- If `mv` auto-moved perm files, moving a program into `/system/` could accidentally grant system-level permissions

To move a program: copy the WASM, create a new perm file at the destination, delete the originals.

---

## 2. Hash Computation

```
hash = SHA-256(raw_wasm_bytes)[0:16] → formatted as "0x" + 32 hex chars
```

Computed once at "install time" (when the program is placed in the filesystem). The loader computes the hash of the loaded WASM bytes at runtime and verifies it matches the filename — if it doesn't, the program is rejected.

### Threat Model

The hash protects against a **preimage attack**: an attacker who can write to `/user/programs/` but NOT to `/perms/` would need to craft a malicious WASM file whose SHA-256 prefix matches a trusted program's hash. With 128 bits, this requires ~2^128 operations — well beyond any feasible attack.

A birthday attack (finding two files with the same hash) doesn't apply here because the attacker must match a *specific* existing hash, not just any collision.

### Development Workflow (Wildcard Hash)

During development, recompiling a WASM program changes its hash on every build. To avoid updating the permissions filename constantly:

1. **Create the permissions file once** with the wildcard hash:
   ```
   /perms/user/programs/myapp.wasm:0x00000000000000000000000000000000
   ```
2. **Set `"hash"` to the wildcard** in the JSON:
   ```jsonc
   { "hash": "0x00000000000000000000000000000000", ... }
   ```
3. **Develop and iterate** — the wildcard matches any hash, so recompiles don't break anything. The kernel prints a warning to serial: `PERM: wildcard hash for myapp.wasm`.
4. **Before releasing**, run `hash myapp.wasm` to get the real hash, then:
   - Rename the permissions file to include the real hash
   - Update the `"hash"` field in the JSON

In production builds (`PERM_ENFORCE_STRICT`), the wildcard hash is rejected — every program must have a real hash.

---

## 3. Format (JSONC)

The file uses JSON with comments (JSONC / "JSON5-lite"):
- `//` line comments and `/* */` block comments
- Trailing commas allowed
- Standard JSON otherwise

### Schema

```jsonc
{
    // Required: must match the hash in the filename (or wildcard 0x0000...)
    "hash": "0x7c0a1b3f2e5d8a91",

    // Optional: SemVer version (required for libraries, optional for programs)
    "version": "1.2.0",

    // Required: the capabilities this program is granted
    "capabilities": {
        // Built-in capability groups. Each expands to a set of individual imports.
        "groups": [
            "wasi_small",    // fd_read, fd_write, fd_close, fd_seek, fd_tell
                             // (deliberately excludes fd_open — see "files" section)
            "logging"        // console.log, console.error
        ],

        // Optional: enable/disable specific imports beyond what groups provide.
        // "+" adds an import, "-" removes one (even if a group provides it).
        "imports": {
            "+fd_open": true,     // explicitly grant fd_open (requires "files" entries)
            "-fd_write": false    // explicitly deny fd_write even though wasi_small provides it
        }
    },

    // Required if fd_open is granted: which files can be accessed
    "files": [
        { "path": "user/data/scores.txt",   "perms": "r"  },
        { "path": "user/data/saves/*",      "perms": "rw" },
        { "path": "user/config/*.cfg",      "perms": "r"  }
    ],

    // Optional: resource limits (sane defaults if omitted)
    "limits": {
        "memory_pages": 256,        // max WASM linear memory (64KB pages)
        "max_file_handles": 16,     // max simultaneous open fds
        "max_execution_ticks": 0    // 0 = unlimited (preempted by scheduler)
    },

    // Optional: network permissions (future)
    "network": {
        "allow_outbound": false,
        "allowed_hosts": []          // e.g. ["api.example.com:443"]
    },

    // Optional: shared library dependencies
    "libraries": {
        "net": {                              // WASM import namespace
            "path": "system/libs/netlib.wasm",
            "version": "1.*",                 // version range (or "hash" for exact pin)
            "ceiling": {                       // optional: narrow lib's capabilities
                "groups": ["network"],
                "imports": { "-net_listen": false }
            }
        }
    }
}
```

---

## 4. Capability Model

### 4.1 Capability Groups

Groups are shorthand bundles of individual imports. When a group is listed, all its member imports are linked — unless explicitly removed in `imports`.

| Group | Members (WASM imports linked) |
|-------|------------------------------|
| `wasi_small` | `fd_read`, `fd_write`, `fd_close`, `fd_seek`, `fd_tell`, `fd_size` |
| `wasi_full` | everything in `wasi_small` + `fd_open`, `fd_create`, `fd_unlink`, `fd_readdir` |
| `logging` | `console_log`, `console_error` (maps to serial/screen output) |
| `display` | `display_get_info`, `display_draw` (framebuffer access) |
| `keyboard` | `keyboard_poll`, `keyboard_wait` (raw input) |
| `network` | `net_connect`, `net_send`, `net_recv`, `net_close` (future) |
| `system` | `time_now`, `time_sleep`, `random_get` |
| `all` | every import the kernel supports — **use sparingly** |

### 4.2 Individual Import Overrides

The `imports` object in the config allows fine-tuning:

- `"+import_name": true` — link this import even if no group provides it
- `"-import_name": false` — deny this import even if a group provides it
- If an import is requested by the WASM module but neither granted nor denied, **the program fails to load** with a clear error message

### 4.3 How Enforcement Works (Link-Time)

When loading a WASM module:

1. Parse the permissions file → `perm_config_t`
2. Parse the WASM module → discover its `(import ...)` declarations
3. For each import the module requests:
   - Check `perm_config.imports` for an explicit `+` or `-`
   - If no explicit rule, check if any granted group provides it
   - If not granted → **reject the module** with `"import 'net_connect' not permitted"`
4. Only link the granted imports via `m3_LinkRawFunction()`

This means a malicious program can't even *attempt* to call an unpermitted function — the import simply doesn't exist in its namespace.

### 4.4 The Three Import Tiers

Not all WASM imports are equal. The permissions system classifies every import into one of three tiers:

| Tier | What it covers | Controlled by | Profile intersection? |
|------|---------------|---------------|----------------------|
| **Tier 1: Implicit (LibC)** | `memset`, `memcpy`, `printf`, `_exit`, `_abort`, `clock_ms`, etc. — linked via `m3_LinkLibC()` | Always linked automatically | No — infrastructure, not a capability |
| **Tier 2: Capability groups** | `fd_read`, `fd_write`, `fd_open`, `display_draw`, `keyboard_poll`, `net_connect`, etc. | Groups + overrides in perm file | Yes — intersected with profile ceiling |
| **Tier 3: Custom imports** | Program-specific host functions like Doom's `ui.drawFrame`, `loading.readWads`, `console.onErrorMessage` | `+import` in perm file only | No — bypasses profile. The perm file alone controls them |

**Why three tiers?**

LibC functions are language runtime — memset and printf don't cross security boundaries. Capability groups are the dangerous syscalls. Custom imports are program-specific contracts (like Doom's rendering pipeline) that don't fit generic groups and shouldn't be blocked by profiles.

The distinction prevents the Doom problem: Doom imports `ui.drawFrame` which isn't in any group. Under a pure-group model, Doom couldn't run under `common` because the import wouldn't survive the profile intersection. With Tier 3, custom imports are controlled only by the perm file — the profile can't accidentally block them.

**How LibC linking works in practice:**

`m3_LinkLibC()` is always called after capability-based linking. It uses `SuppressLookupFailure` internally, so if a module doesn't import `_exit`, it's silently skipped. No harm in calling it unconditionally.

```c
// Loader pseudocode
link_imports(module, effective_caps):
    // Tier 2: capability-controlled imports
    for each import in module.imports:
        if import is in effective_caps:
            m3_LinkRawFunction(module, ...)
        elif import is in Tier 3 (custom):
            // Checked against perm file only, not profile
            if perm_file_allows(import):
                m3_LinkRawFunction(module, ...)
            else:
                FAIL("import not permitted")

    // Tier 1: always linked as infrastructure
    m3_LinkLibC(module);
```

---

## 5. Permission Profiles ("Scenes")

### 5.1 Concept

A **permission profile** (also called a "scene") is a **ceiling** on what capabilities a process can exercise. It's stored on the process and inherited by child processes.

The permissions file (§3–4) declares what a program **requests**. The profile determines how much of that request is actually **granted**:

```
effective = permissions_file.capabilities ∩ profile.capabilities
```

This is a two-layer security model:
- **Layer 1 (static):** The permissions file bounds what a program can ever do — even under `super`, it can't exceed what it declares
- **Layer 2 (dynamic):** The profile bounds what the current session allows — a program requesting `network` under a `common` profile won't get it

### 5.2 The `sp` Command

```
sp                 → prints current profile
sp <profile>       → switch to <profile>
```

- **Escalating** (e.g., `common` → `super`): prompts for a password. The password hash is compiled into the kernel or stored in `/system/passwd`.
- **De-escalating** (e.g., `super` → `common`): no password required. You can always drop privileges freely.
- **Invalid profile**: prints available profiles and does nothing.

`sp` changes the **current shell process's** profile. It does not affect already-running programs — only programs launched *after* the `sp` command inherit the new profile.

### 5.3 Built-in Profiles

Profiles are defined in `/system/profiles.jsonc` (with compiled-in fallbacks):

```jsonc
{
    "profiles": {
        "restricted": {
            "desc": "Guest/untrusted — minimal surface",
            "groups": []
            // effectively: program gets NOTHING beyond its own perm file.
            // if perm file says ["logging"], that's all you get.
        },
        "common": {
            "desc": "Default terminal session",
            "groups": ["wasi_small", "logging", "system"]
        },
        "power": {
            "desc": "Developer / power user",
            "groups": ["wasi_full", "keyboard", "logging", "system"]
        },
        "network": {
            "desc": "Network-facing programs",
            "groups": ["wasi_small", "network", "logging", "system"]
        },
        "super": {
            "desc": "Administrator — all userspace capabilities",
            "groups": ["all"]
        },
        "hardware": {
            "desc": "Kernel development — includes raw hardware access",
            "groups": ["all", "pci", "dma", "io_ports"]
        }
    },

    // The profile a new shell starts with
    "default": "common"
}
```

### 5.4 How the Intersection Works

At WASM load time, the kernel computes the **effective capability set**:

```
Step 1: Expand perm file groups → set of imports
Step 2: Apply perm file +import / -import overrides
Step 3: Expand profile groups → set of imports (the ceiling)
Step 4: Effective = Step2 ∩ Step3
Step 5: Link only imports in Effective
```

Key property: **the profile can only remove capabilities, never add them.** If a program's permissions file doesn't request `network`, no profile can grant it. This prevents a compromised `super` session from giving network access to a program that was never designed for it.

#### Worked Example

Program: `editor.wasm`
```jsonc
// Permissions file requests:
"groups": ["wasi_full", "display", "keyboard"],
"imports": { "-fd_unlink": false }
```

| Profile | Profile Groups | Effective Result |
|---------|---------------|------------------|
| `common` | wasi_small, logging, system | **wasi_small only** — display and keyboard denied |
| `power` | wasi_full, keyboard, logging, system | **wasi_full + keyboard** — display denied (not in power) |
| `super` | all | **wasi_full + display + keyboard – fd_unlink** — everything requested |

Notice: even under `super`, `fd_unlink` is still denied because the **perm file** explicitly removed it. The profile can't override `-` rules.

### 5.5 Storage & Inheritance

```c
// kern_sched.h — new field on kern_process_t
typedef struct kern_process {
    i32 pid;
    u64 cr3;
    // ...
    u8 profile_id;  // index into the profile table (0 = restricted, 4 = super, etc.)
} kern_process_t;
```

- `process_create()` copies the parent's `profile_id`
- The kernel process (PID 0) starts at `super` (it needs full access to boot)
- The initial shell process starts at `default` from profiles.jsonc (usually `common`)
- WASM child processes inherit the shell's profile at spawn time

### 5.6 Password & Authentication

The kernel stores a single SHA-256 password hash for profile escalation:

```c
// Compiled-in default (overridable via /system/passwd)
static const u8 super_password_hash[32] = { /* SHA-256 of default password */ };
```

When `sp super` is invoked:
1. Kernel prompts "Password:" on the terminal (echo disabled)
2. User types password, kernel hashes it with SHA-256
3. Compare against stored hash
4. If match → set `current_process->profile_id = PROFILE_SUPER`
5. If no match → "Access denied", profile unchanged

Future: per-user accounts with individual password hashes in `/system/passwd`.

---

## 6. File Access Control (Runtime)

File permissions are enforced at **runtime** inside the `wasm_fd_open` host function, because the path is only known at call time.

### 6.1 Path Matching

Paths in the permissions file are relative to the filesystem root and use glob-style patterns:

| Pattern | Matches |
|---------|---------|
| `user/data/scores.txt` | Exact match only |
| `user/data/*` | Any file directly in `user/data/` |
| `user/data/**` | Any file recursively under `user/data/` |
| `*.cfg` | Any `.cfg` file in root |
| `user/config/*.cfg` | `.cfg` files directly in `user/config/` |

### 6.2 Permission Levels

- `"r"` — read-only (`fd_open` with O_RDONLY flag passed through)
- `"rw"` — read and write (`fd_open` with O_RDWR)
- Program requests write but only has `"r"` → `fd_open` returns `-EPERM`

### 6.3 Path Canonicalization (Critical)

Before any permission check, paths MUST be canonicalized to prevent traversal attacks:

```
canonicalize(path):
    1. If path starts with "/", treat as absolute from filesystem root
    2. If path is relative, prepend the program's root (if set) or "/"
    3. Split on "/", process each component:
       - "." → skip
       - ".." → pop last component (but never go above root)
       - anything else → push onto stack
    4. Rejoin with "/"
    5. Result is the canonical absolute path
```

This means `../../etc/shadow` resolves to `/etc/shadow`, which won't match any permission glob for a program restricted to `user/programs/myapp/*`. Without canonicalization, a program could bypass path-based restrictions.

### 6.4 Runtime Check Flow

```
wasm_fd_open(path, flags):
    1. Validate WASM memory bounds (existing)
    2. Canonicalize path (resolve . and ..) → absolute canonical path
    3. Iterate perm_config.files[], match canonical path against patterns
    4. If no match → return -EPERM
    5. If match has "r" but flags include O_WRONLY/O_RDWR → return -EPERM
    6. Call fs_open(canonical_path) as usual → return fd
```

### 6.5 Special FDs

File descriptors 0, 1, 2 (stdin, stdout, stderr) are always available and don't require `fd_open`. They map to:
- `0` (stdin): terminal input queue
- `1` (stdout): terminal output
- `2` (stderr): serial output + terminal output

---

## 7. Shared Libraries (DLLs)

### 7.1 Concept

Libraries are WASM files that provide imports to programs. They use the **exact same perm file infrastructure** as programs — same format, same hash verification, same `/perms/` mirroring.

```
Library:     /system/libs/netlib.wasm
Hash:        0x1234abcd...
Perm:        /perms/system/libs/netlib.wasm:0x1234abcd...
```

The only difference: libraries don't have an exported `_start` entry point. They're loaded as dependencies, not executed directly.

### 7.2 Library Install Locations

| Location | Purpose | Permissions |
|----------|---------|-------------|
| `/system/libs/` | System libraries (networking, UI toolkit, crypto) | Read-only to normal users; updated via `sp hardware` |
| `/user/libs/` | User-installed or application-specific libraries | Read-write for the owning user |

### 7.3 Library Perm File

Libraries declare a `version` field (SemVer) and their own capabilities:

```jsonc
// /perms/system/libs/netlib.wasm:0x1234...
{
    "hash": "0x1234abcd...",
    "version": "1.2.0",
    "capabilities": {
        "groups": ["network", "system"]
    }
}
```

### 7.4 Program References Libraries

A program declares its dependencies in a `libraries` block keyed by WASM import namespace:

```jsonc
// /perms/user/programs/myapp.wasm:0xaaaa...
{
    "hash": "0xaaaa...",
    "capabilities": {
        "groups": ["wasi_small", "logging"]
    },
    "files": [
        { "path": "user/data/myapp/*", "perms": "rw" }
    ],
    "libraries": {
        "net": {
            "path": "system/libs/netlib.wasm",
            "version": "1.*",                // version range
            "ceiling": {
                "groups": ["network"]         // narrow: only allow network, not system
            }
        },
        "crypto": {
            "path": "system/libs/cryptlib.wasm",
            "hash": "0x5678efgh..."          // exact hash pin (no version check needed)
        }
    }
}
```

### 7.5 Version Ranges vs. Hash Pinning

Programs specify dependencies using **either** `version` (range) or `hash` (exact pin):

| Field | Behavior | Use case |
|-------|----------|----------|
| `hash: "0xabcd..."` | Exact binary. Hash must match. Strongest guarantee. | Production, security-critical deps |
| `version: "1.2.0"` | Exact SemVer. Any build of 1.2.0 accepted. | Pinned version, allows rebuilds |
| `version: "1.2.*"` | Any patch within 1.2.x | Bugfix releases |
| `version: "1.*"` | Any minor/patch within 1.x | Compatible upgrades |
| `version: ">=1.2, <2.0"` | Range expression | Precise compatibility window |
| `version: "*"` | Any version at all | Dev mode only |

**Version range syntax** (subset of npm/cargo, simple to parse):

| Pattern | Meaning |
|---------|---------|
| `1.2.3` | Exact version |
| `1.2.*` | Patch wildcard — matches 1.2.0 through 1.2.999 |
| `1.*` | Minor wildcard — matches 1.0.0 through 1.999.999 |
| `*` | Any version (dev/wildcard — rejected in strict mode) |
| `>=1.2` | 1.2.0 or higher |
| `>=1.2, <2.0` | Range. Comma-separated constraints are ANDed |

### 7.6 The Ceiling (Narrowing Library Permissions)

The `ceiling` block is orthogonal to versioning. It limits what the library can do, regardless of what its own perm file declares:

```
lib_effective = lib_perm ∩ parent_ceiling ∩ process_profile
```

This is the same intersection model as profiles (§5). The ceiling can only remove, never add.

**When is a ceiling required?**
- `hash` pin: ceiling is **optional** (you verified the exact binary)
- `version` range: ceiling is **strongly recommended** (the library could change beneath you)
- If no ceiling and using a version range: the program trusts the library's full perm file. This is acceptable for first-party or well-known system libraries.

### 7.7 Transitive Dependency Resolution

Libraries can depend on other libraries. The loader resolves the full tree at load time:

```
resolve_deps(module, parent_ceiling, profile, visited):
    1. If module in visited → FAIL (circular dependency)
    2. Add module to visited
    3. Load module's perm file
    4. effective = module_perm ∩ parent_ceiling ∩ profile
    5. For each lib in module.libraries:
       a. Load lib's perm file (hash + version check)
       b. child_ceiling = lib.ceiling || effective  // child can't exceed parent
       c. resolve_deps(lib, child_ceiling, profile, visited)
       d. Merge lib's effective imports into total
    6. Return effective ∪ merged_lib_imports
```

Rules:
- **Circular dependencies**: detected and rejected. Minimal cycle detection with a visited stack.
- **Cascading ceilings**: a library's max capability is the intersection of all ceilings above it in the tree.
- **File access**: each library uses its own `files` array. If a parent distrusts a library's file I/O entirely, it omits `wasi_full` or sets `"-fd_open": false` in the ceiling.
- **`-` overrides are absolute**: if any module in the tree has `-fd_write: false`, fd_write is denied regardless of what other modules request.

### 7.8 Swap Attack Prevention

Same protection as programs (§1): the hash in the perm filename prevents an attacker from swapping a library binary. The loader always verifies hash first, then checks version. An attacker who replaces `netlib.wasm` with a malicious version fails the hash check. The only exception is the wildcard hash `0x0000...` which is dev-only.

### 7.9 Complete Example: Program + Two Libraries

```
myapp.wasm
├── netlib.wasm (v1.2.3) ─── needs [network, system]
└── uikit.wasm  (v2.0.0) ─── needs [display, keyboard]
                             └── cryptlib.wasm (v1.0.0) ─── needs [system]
```

**netlib perm file:**
```jsonc
{ "hash": "0xn...", "version": "1.2.3", "capabilities": { "groups": ["network", "system"] } }
```

**uikit perm file:**
```jsonc
{
    "hash": "0xu...", "version": "2.0.0",
    "capabilities": { "groups": ["display", "keyboard"] },
    "libraries": {
        "crypto": { "path": "system/libs/cryptlib.wasm", "hash": "0xc..." }
    }
}
```

**myapp perm file:**
```jsonc
{
    "hash": "0xm...",
    "capabilities": { "groups": ["wasi_small", "logging"] },
    "files": [ { "path": "user/data/myapp/*", "perms": "rw" } ],
    "libraries": {
        "net": {
            "path": "system/libs/netlib.wasm",
            "version": "1.*",
            "ceiling": { "groups": ["network"] }    // narrow: network only, block system
        },
        "ui": {
            "path": "system/libs/uikit.wasm",
            "version": ">=2.0, <3.0"
            // no ceiling — trust uikit's full perm file
        }
    }
}
```

**Effective permissions** (under `common` profile: wasi_small, logging, system):

| Source | Own effective |
|--------|--------------|
| myapp | wasi_small, logging (system not requested) |
| netlib via ceiling | network ∩ common = network |
| uikit | display, keyboard ∩ common = (none — not in common profile) |
| uikit→cryptlib via uikit | system ∩ common = system |
| **TOTAL** | wasi_small, logging, network, system |

Note: uikit's `display` and `keyboard` are silently dropped because `common` profile doesn't include them. To use uikit, the user would need `sp power` first.

---

---

## 8. Future: WASM Virtual Filesystems

A natural extension of the permissions system is giving each WASM program its own **private filesystem view** — the program sees a virtual root like `/data.txt`, while the kernel maps it to `/user/programs/myapp/data.txt` on the real filesystem.

### Proposed Design (Not Yet Implemented)

Add an optional `"root"` field to the permissions file:

```jsonc
{
    "root": "user/programs/myapp/",
    "files": [
        { "path": "data/*",      "perms": "rw" },  // really user/programs/myapp/data/*
        { "path": "config.json", "perms": "r"  }   // really user/programs/myapp/config.json
    ]
}
```

The program opens `/config.json` → kernel prepends the root → real path is `/user/programs/myapp/config.json`.

### Benefits

- **Simpler program code**: programs use short, predictable paths
- **Stronger isolation**: the program literally cannot see files outside its root (even with `..` traversal — canonicalization handles this)
- **Easy cleanup**: deleting `/user/programs/myapp/` removes all the program's data
- **Portable permissions**: the same perm file works regardless of where the program is installed

### Tradeoffs

- Programs that genuinely need access to multiple disjoint paths (e.g., a system tool) need multiple `files` entries — the `"root"` just provides a convenient default prefix
- The kernel needs to translate paths in both directions (open → prepend root, readdir → strip root from results)

### Relationship to Path Canonicalization

Path canonicalization (§6.3) already prevents `../` attacks without a virtual root. The VFS is a *convenience* feature, not a *security* feature. It can be added after the core permissions system is working.

---

## 9. Resource Limits

| Limit | Default | Description |
|-------|---------|-------------|
| `memory_pages` | 64 | Max WASM linear memory in 64KB pages (default 4MB) |
| `max_file_handles` | 8 | Max simultaneous open file descriptors |
| `max_execution_ticks` | 0 | 0 = unlimited (scheduler handles preemption naturally) |

Enforcement:
- **memory_pages**: checked at `m3_NewRuntime()` — the runtime's stack size and memory limit are set from this
- **max_file_handles**: checked in `wasm_fd_open()` — if the process's `fd_table` is full, return `-EMFILE`
- **max_execution_ticks**: if non-zero, the scheduler can use this as a hint for priority/deadline

---

## 10. Complete Examples

### Program: `/user/programs/editor.wasm`
### Hash: `0xa1b2c3d4e5f60718`

### `/perms/user/programs/editor.wasm:0xa1b2c3d4e5f60718a1b2c3d4e5f60718`

```jsonc
{
    "hash": "0xa1b2c3d4e5f60718a1b2c3d4e5f60718",

    "capabilities": {
        "groups": [
            "wasi_full",     // full file I/O
            "display",       // draw to screen
            "keyboard",      // read keyboard
            "logging",       // debug output
            "system"         // time, random
        ],
        "imports": {
            "-fd_unlink": false   // editor can't delete files
        }
    },

    "files": [
        { "path": "user/documents/**",    "perms": "rw" },
        { "path": "user/config/editor/*", "perms": "rw" },
        { "path": "system/fonts/*",       "perms": "r"  }
    ],

    "limits": {
        "memory_pages": 512,     // 32MB for large documents
        "max_file_handles": 32
    }
}
```

### A more restricted example — `/perms/user/programs/hello.wasm:0xfeedface12345678feedface12345678`

```jsonc
{
    "hash": "0xfeedface12345678feedface12345678",
    "capabilities": {
        "groups": ["logging"]
        // no file I/O, no display, no keyboard
    },
    // no "files" section needed — fd_open isn't granted
    "limits": {
        "memory_pages": 16     // 1MB is plenty for "hello world"
    }
}
```

### Doom — `/perms/user/programs/doom.wasm:0x...`

```jsonc
{
    "hash": "0x...",

    "capabilities": {
        "groups": [
            "wasi_small",   // for savegame I/O (fd_read, fd_write, fd_close)
            "display",      // framebuffer
            "keyboard",     // input
            "system"        // time
        ],
        "imports": {
            "+fd_open": true  // needed to open DOOM1.WAD and savegame files
        }
    },

    "files": [
        { "path": "DOOM1.WAD",          "perms": "r"  },
        { "path": "DOOM2.WAD",          "perms": "r"  },
        { "path": "DOOM.WAD",           "perms": "r"  },
        { "path": "user/saves/doom/*",  "perms": "rw" }
    ],

    "limits": {
        "memory_pages": 1024    // Doom needs ~64MB
    }
}
```

---

## 11. Cleanup & Maintenance

### 10.1 The `permclean` Command

Orphaned permissions files (where the WASM program no longer exists) accumulate over time. The `permclean` command handles garbage collection:

```
permclean              → list all orphaned perm files (dry run)
permclean --purge      → delete orphaned perm files
permclean --all        → list ALL perm files with status (OK / ORPHAN / WILDCARD)
```

Implementation:
1. Scan `/perms/` recursively
2. For each perm file, extract the WASM path (strip the `:0x...` suffix)
3. Check if the WASM file exists at that path
4. If not → flag as orphaned

### 10.2 When to Run

- **Manual**: after uninstalling programs, run `permclean --purge`
- **Boot-time**: a kernel task can run a quick scan and log orphan count to serial
- **Periodic**: future — a background task that runs weekly

### 10.3 What `permclean` Does NOT Do

- Does NOT auto-delete without `--purge` (always dry-run first)
- Does NOT touch perm files whose WASM still exists
- Does NOT validate hash correctness (that's the loader's job)

### 10.4 Deleting a Program (Recommended Workflow)

```
rm /user/programs/oldapp.wasm
permclean --purge       # removes /perms/user/programs/oldapp.wasm:0x...
```

---

## 12. Implementation Roadmap

### Phase 1: Core Infrastructure
1. Add `perm_config_t` struct and minimal JSONC parser
2. Implement `perm_load(path, hash)` → reads and parses the sidecar file
3. Implement `perm_check_import(perm, import_name)` → returns bool
4. Implement `perm_check_file_access(perm, path, flags)` → returns bool
5. Add `profile_id` field to `kern_process_t`, define profile table
6. Implement `perm_profile_from_name(name)` → profile_id lookup
7. Implement `perm_get_effective(perm, profile_id)` → intersection logic

### Phase 2: Integration
8. Refactor `wasm_test()` into a generic `wasm_load_and_run(path)` that:
   - Computes hash, loads perm file, loads profile from current process
   - Computes effective capabilities via intersection
   - Links only permitted imports (replaces hardcoded `m3_LinkRawFunction`)
9. Add runtime file access checks in `wasm_fd_open()`
10. Add resource limit enforcement

### Phase 3: The `sp` Command
11. Implement `sp` as a shell built-in or kernel command:
    - `sp` alone prints current profile name
    - `sp <name>` validates the profile, prompts for password if escalating
    - Updates `current_process->profile_id`
12. Implement password prompt with echo disabled
13. Add SHA-256 hashing (needed for both hash computation and password)

### Phase 4: Tooling
14. `hash` shell command to compute and display a WASM file's hash
15. `permgen` command to generate a template permissions file given a WASM path
16. `permclean` command for orphaned perm file garbage collection

### Phase 5: Hardening
16. Disable the `0x0000...` wildcard hash in non-dev builds
17. Add a kernel config flag `PERM_ENFORCE_STRICT` for production

---

## 13. Design Decisions & Rationale

| Decision | Why |
|----------|-----|
| SHA-256 (128-bit, 32 hex chars) | Preimage resistance of 2^128 — infeasible to forge. 32 chars fits in ext2's 255-byte filename limit |
| Path + hash naming | Prevents swap attacks; still human-browsable |
| Perm files do not auto-move with WASM | VFS stays simple; moving a program to a new security context is a deliberate act |
| JSONC format | Readable, comment support, trivial to parse a subset |
| Groups + overrides | Groups make common cases one-liners; overrides handle edge cases without explosion of groups |
| Link-time enforcement for imports | Fails fast — the program can't even reference an unpermitted function |
| Runtime enforcement for file paths | Paths are dynamic data, can only be checked at call time |
| No permissions file = no run | Fail-closed is the only safe default |
| `fd_open` not in `wasi_small` | Forces explicit opt-in to file opening; reading/writing already-open fds is less dangerous |
| Profiles as ceilings (not grants) | Profile can only remove capabilities, never add them. Prevents compromised `super` from giving network to a calculator app |
| Intersection model (perm ∩ profile) | Two-layer defense: the program's own manifest + the session's trust level. Both must agree |
| Profile inheritance at spawn | Consistent with Unix process model; child can't escape parent's ceiling |
| Password only for escalation | De-escalation is always free — encourages dropping privileges when not needed |
| Path canonicalization required | `../` traversal is the most common filesystem escape; canonicalization kills it at the source |
| Wildcard hash for dev, strict for prod | Zero-friction iteration during development; cryptographic integrity when it matters |
| `permclean` for garbage collection | Decoupled from VFS; simple to implement; catches information leaks from orphaned perm files |
| Libraries use same perm file infrastructure | No second system to learn; hash verification, versioning, and ceilings apply uniformly |
| Hash pin (default) vs. version range (opt-in) | Pinned by hash prevents surprise breakage; version ranges enable managed upgrades |
| Ceiling orthogonal to versioning | Version controls *which* binary; ceiling controls *what it can do*. Two separate concerns |
| Circular dependencies = hard fail | Prevents infinite loops and dependency hell; forces clean library design |
| Cascading ceilings (child ≤ parent) | A library can never escalate beyond what its caller allows; trust flows downward only |
