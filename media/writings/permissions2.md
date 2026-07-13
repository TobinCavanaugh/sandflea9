# Permissions System: Red-Team Audit & Hard Thinking

This document is the result of an exhaustive security and design audit of the permissions system specified in `permissions.md`. It contains findings rated CRITICAL/HIGH/MEDIUM/LOW, deeper analysis, and proposed fixes or alternative approaches.

---

## CRITICAL Findings

### C1: Tier 3 Custom Imports Can Bypass the Profile Ceiling

**The problem**: The three-tier model says Tier 3 (custom imports) bypasses the profile intersection. A malicious program could import `net_connect` as a "custom" import by declaring `"+net_connect": true` in its perm file. Since Tier 3 doesn't go through the profile ceiling, this bypasses `sp common` restrictions entirely.

**Root cause**: There's no clear namespace boundary between Tier 2 (capability) imports and Tier 3 (custom) imports. They all live in the same flat import namespace.

**Fix**: Reserve specific WASM import module namespaces for capability-controlled functions:
- `"env"` namespace → all Tier 2 capability imports (`env.fd_open`, `env.display_draw`, etc.)
- `"app"` or custom module names → Tier 3 custom imports (`console.onErrorMessage`, `ui.drawFrame`)
- The loader enforces: any import in `"env"` MUST go through the profile ceiling. Any import in other modules is Tier 3.

This means Doom's `env.fd_open` is Tier 2 (profile-filtered) but `ui.drawFrame` is Tier 3 (perm-file-only). The namespace IS the boundary.

**Alternative**: Simply don't have Tier 3 at all. Define capability groups for EVERY import, even custom ones. Programs that need custom imports must create their own capability group. This is more tedious but eliminates the bypass entirely.

### C2: JSONC Parsing in Kernel Space

**The problem**: Parsing untrusted, user-supplied JSONC in ring 0 is a massive attack surface. A malicious perm file could contain:
- Deeply nested objects causing stack overflow in recursive descent parsers
- Multi-megabyte strings causing heap exhaustion
- Invalid UTF-8 causing undefined behavior
- Integer overflow in number parsing

Even "minimal" JSON parsers in C have had CVEs. Running one in the kernel is asking for trouble.

**Fix**: Pre-compile perm files at install time. The workflow becomes:
1. User creates `/perms/myapp.wasm:0xabcd...` as JSONC (human-editable)
2. The `perminstall` tool reads the JSONC, validates it, and writes a binary representation to `/perms/.cache/myapp.wasm:0xabcd...bin`
3. The kernel ONLY reads the binary cache file (fixed-size structs, no parsing)
4. If the binary cache is missing or stale (hash mismatch), the kernel refuses to load with "run perminstall first"

Binary format sketch:
```c
struct perm_binary {
    u32 magic;           // 0x5045524D ("PERM")
    u32 version;         // format version
    u8  hash[16];        // 128-bit SHA-256 of WASM
    u32 num_groups;
    u32 group_ids[16];   // indices into compiled-in group table
    u32 num_plus_imports;
    u32 num_minus_imports;
    // ... fixed-size arrays for imports, file paths, library refs
};
```

This eliminates the JSONC parser from the kernel entirely. The attack surface shrinks to "validate a few integers and sizes."

### C3: Lexical Canonicalization vs. Symlinks

**The problem**: The spec canonicalizes paths by resolving `..` and `.` lexically (string manipulation). But if ext2 supports symlinks (it does — they're in the spec), lexical canonicalization diverges from real VFS resolution:

```
Real filesystem:
  /user/data/     → directory
  /user/link      → symlink → /system/

WASM program calls fd_open("../link/shadow"):
  Lexical: /user/data/../link/shadow → /user/link/shadow  ✓ (looks safe)
  Physical: /user/data/../link/shadow → /system/shadow     ✗ (bypassed!)
```

The permission check passes (it's under `/user/`) but the VFS resolves to `/system/`.

**Fix**: Do NOT canonicalize lexically. Instead, resolve the path physically through the VFS, one component at a time, expanding symlinks. At each step, verify the resolved path is within the permitted globs.

```c
canonicalize_physical(path):
    stack = []
    for each component in split(path, "/"):
        if component == ".":  continue
        if component == "..": pop(stack)
        else:                 push(stack, component)
        // Resolve symlinks at each step
        resolved = join(stack)
        if is_symlink(resolved):
            resolved = readlink(resolved)
            stack = split(resolved, "/")  // restart from link target
    return "/" + join(stack)
```

This is more expensive (requires VFS lookups) but is the only correct approach.

---

## HIGH Findings

### H1: Diamond Dependency Ceilings Are Non-Deterministic

**The problem**: When two libraries depend on the same sub-library with different ceilings:

```
myapp
├── libA → cryptlib (ceiling: [system])
└── libB → cryptlib (ceiling: [network])
```

The DFS visited-stack algorithm resolves `cryptlib` the first time it's encountered. But JSONC object key iteration order is undefined in most parsers. So `cryptlib` might get `[system]` or `[network]` depending on which parent is processed first. This is non-deterministic — a program that works today might fail tomorrow after a seemingly unrelated perm file edit.

**Fix A** (simpler): Compute the **intersection** of all ceilings targeting the same library. `cryptlib` gets `system ∩ network = (empty)`. If empty, warn that ceilings conflict.

**Fix B** (stricter): If two parents specify DIFFERENT ceilings for the same library, **hard fail** at load time. Force the program to reconcile the conflict explicitly.

**Fix C** (most permissive): Take the **first** ceiling encountered and warn. Simple but non-deterministic.

Recommendation: **Fix A** (intersection). It's safe (can't escalate), deterministic, and matches the "ceiling can only remove" philosophy.

### H2: fd_readdir Leaks Directory Contents

**The problem**: The spec hooks `fd_open` for path checking but ignores `fd_readdir`. If a program has `wasi_full` with `files: [{"path": "user/data/*", "perms": "r"}]`, it can:
1. `fd_open("user/data/")` → gets directory FD
2. `fd_readdir(dir_fd)` → enumerates ALL files in the directory, not just the ones matching `*`

The permission check on `fd_open` passes (directory is within glob), but `fd_readdir` reveals all filenames.

**Fix**: Hook `fd_readdir` to filter results. Only return entries whose full path matches the program's permitted file patterns. Entries that don't match are silently omitted from the listing.

### H3: /perms/ Is World-Readable

**The problem**: A program with `wasi_full` on `/*` or `files: [{"path": "*", "perms": "r"}]` can read other programs' perm files. This reveals:
- What programs are installed (even orphaned ones via `permclean` leftovers)
- What capabilities each program has
- What files each program accesses
- What libraries each program depends on

This is valuable reconnaissance for an attacker planning a targeted exploit.

**Fix**: Hard-code `/perms/` as unreadable from within WASM. The `wasm_fd_open` handler checks: if the canonical path starts with `/perms/`, return `-EPERM` regardless of what the perm file's `files` array says. `permclean` runs as a kernel/shell tool, not a WASM program, so it's unaffected.

Alternatively: make `/perms/` only readable by non-WASM processes (native code). This maps to a future "process type" field.

---

## MEDIUM Findings

### M1: The `all` Group Expands Silently

**The problem**: The `all` group means "every import the kernel supports." But kernel versions add new imports. A program granted `all` today would silently gain access to new, potentially dangerous syscalls when the kernel updates.

Example: `all` today includes `[wasi_full, display, keyboard, network, system]`. Tomorrow, the kernel adds `raw_disk_write` and `dma_transfer`. Programs with `all` automatically get these.

**Fix**: Version the `all` group. Instead of `all`, use `all_v1`, `all_v2`, etc. A program requesting `all_v1` only gets the imports that existed in kernel API version 1. New kernel versions add `all_v2` with the expanded set, but old programs stay at `all_v1`.

Alternative: **Remove `all` entirely.** Require programs to explicitly list groups. This is safer but less convenient. For a hobby OS, convenience matters — keep `all` but version it.

### M2: SemVer Range Parsing Is Fragile in C

**The problem**: Parsing strings like `>=1.2, <2.0` in kernel C:
- Integer overflow: `9999999999.0.0` overflows u32
- Too many components: `1.2.3.4.5` 
- Empty string: `""` 
- Impossible ranges: `<1.0, >=2.0` (no version satisfies both)
- Trailing garbage: `1.2.3abc`

Each of these is a potential crash or logic bug in a kernel parser.

**Mitigation**: 
1. Enforce u16 for each version component (max 65535 — enough for any real version)
2. Reject any version string with more than 3 components
3. Validate that ranges are satisfiable (at least one version exists between min and max)
4. Reject on any parse failure — fail closed

**Long-term fix**: Drop version ranges entirely for V1. Use exact hash pinning only. Add version ranges in V2 after the parsing is battle-tested.

### M3: Empty Intersections Are Cryptic Failures

**The problem**: If a library requests `[network, system]` but the parent's ceiling says `[display]`, the intersection is empty. The library gets 0 capabilities. At runtime, when the WASM module tries to call `net_connect`, it gets a confusing "import not found" error. The developer has no idea why.

**Fix**: At load time, if a library's effective capability set is empty (or drops below some threshold), emit a clear error:
```
"library 'net' (system/libs/netlib.wasm): effective capabilities are empty.
 Library requests: [network, system]
 Parent ceiling allows: [display]
 Intersection: (none)
 Check your ceiling configuration."
```

### M4: Passwords Have No Brute-Force Protection

**The problem**: `sp super` prompts for a password. If an attacker can script terminal input, they can brute-force the password with no rate limiting. Even a strong password eventually falls to unlimited attempts.

**Fix**: Add exponential backoff:
- 1st failed attempt: immediate retry allowed
- 2nd: 1 second delay
- 3rd: 2 seconds
- 4th: 4 seconds
- 5th+: 60 seconds between attempts
- After 10 failures: lock `sp` for 5 minutes, log to serial

For a single-user hobby OS, even a simple 3-attempts-then-30-second-lockout would suffice.

### M5: Load-Time I/O Amplification

**The problem**: Loading a program with 5 libraries:
- Each WASM: open, read full contents, hash, close (~4 I/O ops)
- Each perm file: open, read full contents, parse, close (~3 I/O ops)  
- With binary cache (C2 fix): one read each (~2 I/O ops)

Without cache: ~35 disk reads. With cache: ~12 disk reads. On spinning rust (IDE emulation), this is seconds of load time.

**Fix**: 
1. Implement the binary perm cache (C2)
2. Cache WASM hashes: store `<path, mtime, hash>` in memory so re-loading the same WASM skips re-hashing
3. Batch library reads: load all library WASMs and perm files in parallel if the scheduler supports async I/O

---

## LOW Findings

### L1: No Detection for Unused Library Declarations

If a program's perm file declares a library dependency but the WASM module doesn't import anything from that library's namespace, the library is linked but unused. This should produce a warning but not an error. It might indicate a stale dependency declaration.

### L2: Resource Limits Not Propagated to Libraries

The spec defines `limits` (memory_pages, max_file_handles) for programs but doesn't specify how library limits interact. If a library has `memory_pages: 512` and the program has `memory_pages: 128`, the effective limit should be `min(128, 512) = 128`. The tightest limit wins.

### L3: Password Hash in Kernel Binary

The `sp` password hash is compiled into the kernel. Someone who dumps the kernel binary has the hash. This is acceptable (they still need to crack SHA-256) but worth noting. In production, the hash should be stored in an encrypted filesystem or TPM-backed storage. For a hobby OS, compiled-in is fine.

---

## Philosophical Concerns

### P1: Are We Over-Engineering for a Hobby OS?

The full system as specified has:
- JSONC perm files with hash verification
- 8+ capability groups with individual import overrides
- 6 built-in profiles with intersection logic
- Version ranges with SemVer parsing
- Transitive library dependency resolution with ceiling cascading
- Custom imports bypassing profiles
- Path canonicalization with symlink resolution
- Binary perm caching
- permclean garbage collection

This is approaching the complexity of Android's permission model or iOS's entitlement system. For a single-developer hobby OS where the developer is also the only user, much of this complexity is unnecessary.

**The 80/20 version**: Hash-pinned perm files + capability groups + `sp super` (no password, just a flag) + path checks on fd_open. That's ~200 lines of C and covers 95% of real security needs. Libraries, version ranges, ceilings, three-tier imports, and binary caching can all be deferred.

### P2: The Fundamental Tension: Security vs. Iteration Speed

Every security layer adds friction to the development workflow. The current spec has:
- Hash verification (breaks on every recompile)
- Perm file requirement (fails if forgotten)
- Profile ceiling (blocks capabilities silently)
- library version checks (breaks on library updates)

The wildcard hash helps but doesn't solve the profile ceiling problem — a developer testing their program under `common` profile might spend 30 minutes debugging why `display_draw` doesn't work before realizing they need `sp power` first.

**Mitigation**: When a capability fails due to profile intersection (not perm file denial), the loader should emit a clear message: `"import 'display_draw' blocked by profile 'common'. Use 'sp power' to grant display access."`

### P3: The "One Perm File Per Program" Model Doesn't Scale to Package Managers

In a future with a package manager (`pkg install netlib`), who creates the perm file for the installed library? The package author? The package manager? The user?

The package manager should generate the perm file at install time by:
1. Reading the library's WASM binary
2. Computing the hash
3. Looking for a bundled `.perm` template from the package author
4. Creating `/perms/system/libs/netlib.wasm:<hash>` 
5. The user's programs then reference this hash

But this means the package manager needs to run as `super` (to write to `/perms/system/`). This is correct — installing system libraries is a privileged operation.

### P4: What If We Started Over?

If we redesigned from scratch with the lessons from this audit, the minimal viable system would be:

**Phase 1 (implement now):**
- Binary perm format (no JSONC in kernel)
- Exact hash pinning only (no version ranges)
- Flat capability groups (wasi_small, wasi_full, display, keyboard, network, system, all)
- `sp <profile>` with compiled-in password hash
- Path checks on fd_open with physical canonicalization
- No library support (static linking only for now)

**Phase 2 (add when needed):**
- Library dependencies with hash pinning
- Binary perm caching in memory
- fd_readdir filtering

**Phase 3 (add when scaling):**
- Version ranges
- Library ceilings
- Transitive dependency resolution
- `/perms/` read protection
- Exponential backoff for sp

This phased approach delivers security today without drowning in complexity.

---

## Summary Table

| ID | Severity | Issue | Recommended Fix |
|----|----------|-------|-----------------|
| C1 | CRITICAL | Tier 3 can bypass profile ceiling | Namespace-gate: env.* = Tier 2, app.* = Tier 3 |
| C2 | CRITICAL | JSONC parsing in kernel | Pre-compile to binary format at install time |
| C3 | CRITICAL | Lexical canonicalization misses symlinks | Physical VFS-level path resolution |
| H1 | HIGH | Diamond deps have non-deterministic ceilings | Intersect all ceilings targeting same library |
| H2 | HIGH | fd_readdir leaks directory contents | Filter readdir results against perm file patterns |
| H3 | HIGH | /perms/ is world-readable | Hard-code /perms/ as unreadable from WASM |
| M1 | MEDIUM | `all` group silently expands with kernel updates | Version the `all` group (all_v1, all_v2) |
| M2 | MEDIUM | SemVer parsing is fragile in C | Defer version ranges; use hash pinning only for V1 |
| M3 | MEDIUM | Empty intersections produce cryptic errors | Emit detailed intersection diagnostics at load time |
| M4 | MEDIUM | No brute-force protection on sp password | Exponential backoff on failed attempts |
| M5 | MEDIUM | Load-time I/O amplification | Binary perm cache + WASM hash cache |
| L1 | LOW | No detection of unused library declarations | Emit warning, continue loading |
| L2 | LOW | Resource limits not propagated to libraries | min() across all modules in the tree |
| L3 | LOW | Password hash in kernel binary | Acceptable for hobby OS; TPM in production |
| P1 | — | Over-engineering for a hobby OS | Phase the implementation; start with the 80/20 subset |
| P2 | — | Tension between security and iteration speed | Clear error messages when profile blocks a capability |
| P3 | — | Package manager integration not designed | Package manager generates perm files at install time |
| P4 | — | What if we started over? | Three-phase roadmap: flat permissions → libraries → version ranges |

---

## Appendix A: Meta-Review — Auditing the Audit

*This section was written after stepping back and critically examining the audit above. It questions the audit's own assumptions, re-prioritizes for a hobby OS context, and proposes ergonomic improvements.*

### A1. Findings That Don't Apply to a Hobby OS

Several CRITICAL/HIGH ratings assume a multi-user, networked, attack-surface-rich environment. For a single-developer hobby OS where the developer controls every file:

| Finding | Why Overrated | Real Severity |
|---------|--------------|---------------|
| C2 — JSONC in kernel | There's no untrusted input. Every file on disk was put there by you. The parser just needs to not crash on malformed input — length limits + null checks handle this. | LOW |
| H3 — /perms/ readable | There are no other users. A WASM program reading /perms/ is a program YOU wrote. | LOW (for now) |
| M4 — Password brute-force | No SSH, no remote access, no scripting interface. Brute-force requires physically typing at the keyboard. | LOW (for now) |
| C3 — Symlink traversal | The current ext2 disk image has zero symlinks. This is solving a problem that doesn't exist yet. | MEDIUM (deferred) |

### A2. Fixes That Are Worse Than The Disease

**Binary perm format (C2 fix)**: Creates a cache invalidation problem. You'll edit the JSONC, forget to recompile the binary, and spend 20 minutes debugging why your permission changes didn't take. The fix introduces a more annoying class of bug (cache desync) than the original (JSONC parsing). Plus, `/system/profiles.jsonc` still needs parsing — are we pre-compiling that too?

**Verdict**: Keep JSONC parsing in the kernel with strict limits (max 4KB file, max 8 nesting levels, reject on any parse error). This is ~100 lines of C for a subset parser. Run the validator as a userspace tool during development if you're worried about malformed files.

**Physical VFS canonicalization (C3 fix)**: Resolving symlinks component-by-component in kernel C, with cycle detection, is a recipe for infinite loops and kernel stack overflows. This introduces a genuine DoS vector that doesn't exist today.

**Verdict**: Keep lexical canonicalization for now. Add a symlink depth limit (max 8 symlink resolutions per path). When symlinks actually appear on the disk image, revisit.

### A3. What The Audit Missed

**TOCTOU on fd_open**: Even with perfect path checking, there's a window between the permission check and `fs_open()` where a symlink could be swapped. The fix isn't more canonicalization — it's checking the inode after opening and verifying it matches the expected path. This is a real vulnerability but requires an attacker with concurrent filesystem access (which doesn't exist yet).

**Redundant hash in JSON**: The hash is in the FILENAME already. The JSON `"hash"` field is pure redundancy — it can only disagree with the filename, never add value. If they disagree, which one wins? The filename should win (it's the one the loader actually checks). Drop the JSON `"hash"` field entirely.

**profiles.jsonc parsing**: Even if we pre-compile program perm files, `/system/profiles.jsonc` still needs runtime parsing unless we pre-compile it too. This is inconsistent with the C2 fix.

---

## Appendix B: Ergonomic Design — Editing & Viewing Permissions

*A permissions system is only as good as its tooling. On a text-mode OS without a GUI, how do users actually interact with permissions?*

### B1. The Hash-in-Filename Problem

The current naming scheme produces unreadable directory listings:

```
$ ls /perms/user/programs/
calculator.wasm:0xa1b2c3d4e5f60718a1b2c3d4e5f60718
editor.wasm:0x1f3e5d7c9b0a28161f3e5d7c9b0a28161
doom.wasm:0x7f3a1c5e9d2b40817f3a1c5e9d2b40817
```

Every filename is truncated at the terminal width. You can't tell which program is which at a glance.

**Recommended fix: Subdirectory-based naming.**

```
/perms/user/programs/calculator.wasm/0xa1b2c3d4e5f60718a1b2c3d4e5f60718
/perms/user/programs/editor.wasm/0x1f3e5d7c9b0a28161f3e5d7c9b0a28161
/perms/user/programs/doom.wasm/0x7f3a1c5e9d2b40817f3a1c5e9d2b40817
```

Now `ls /perms/user/programs/` shows clean program names:

```
$ ls /perms/user/programs/
calculator.wasm/
editor.wasm/
doom.wasm/
```

Each program directory contains hash-named perm files. Multiple versions of the same program naturally coexist. `ls /perms/user/programs/doom.wasm/` shows all installed versions.

**Tradeoff**: Creates more directories. But ext2 handles this fine, and the readability improvement is worth it.

### B2. Viewing Permissions: `permshow`

```
$ permshow doom.wasm
┌─ Permissions for doom.wasm ─────────────────────────────┐
│ Hash:      0x7f3a1c5e...                                 │
│ Profile:   common                                        │
│                                                          │
│ REQUESTED  (from perm file)      EFFECTIVE  (∩ profile)  │
│ ─────────  ─────────────────     ────────  ────────────  │
│ wasi_small                       wasi_small              │
│ display         ← BLOCKED        (none)                  │
│ keyboard        ← BLOCKED        (none)                  │
│ system                           system                  │
│ +fd_open                          (none)  ← BLOCKED      │
│                                                          │
│ Files:                                                   │
│   r   DOOM1.WAD                                          │
│   r   DOOM2.WAD                                          │
│   rw  user/saves/doom/*                                  │
│                                                          │
│ Limits: memory=64MB  file_handles=8                      │
│ Libraries: (none)                                        │
│                                                          │
│ ⚠ 2 capabilities blocked by profile 'common'             │
│   Use 'sp power' to grant display + keyboard access      │
└──────────────────────────────────────────────────────────┘
```

Key design decisions:
- **Requested vs. Effective side-by-side** — the developer instantly sees WHAT was blocked and WHY
- **BLOCKED markers** — visual indication that a capability was filtered by the profile
- **Actionable advice at the bottom** — tells the user exactly what command to run
- **Box-drawing characters** — looks intentional and polished even in text mode

### B3. Editing Permissions: `permset`

For a text-mode OS without a full text editor, a command-line property setter is more practical than editing raw JSONC:

```
$ permset doom.wasm groups wasi_small display keyboard system
  Set groups: wasi_small, display, keyboard, system

$ permset doom.wasm +display +keyboard
  Added: display, keyboard
  Groups now: wasi_small, display, keyboard, system

$ permset doom.wasm -network
  Removed: network (wasn't set anyway)

$ permset doom.wasm file "user/saves/doom/*" rw
  Added file: user/saves/doom/* (rw)

$ permset doom.wasm limit memory_pages 1024
  Set memory_pages: 1024 (64MB)

$ permset doom.wasm hash auto
  Computing hash of /user/programs/doom.wasm...
  Hash: 0x7f3a1c5e9d2b40817f3a1c5e9d2b40817
  Renamed perm file to doom.wasm/0x7f3a1c5e9d2b40817f3a1c5e9d2b40817
```

This works without a text editor. Each `permset` invocation modifies one aspect of the permissions and saves immediately. For new programs:

```
$ permset myapp.wasm create
  Created /perms/user/programs/myapp.wasm/0x0000... (wildcard hash)
  Use 'permset myapp.wasm groups ...' to add capabilities
  Use 'permset myapp.wasm hash auto' to lock in a real hash
```

### B4. Creating from Template: `permgen`

For programs that need complex permissions (many files, multiple libraries), a template generator:

```
$ permgen myapp.wasm
{
    "hash": "0x00000000000000000000000000000000",
    "capabilities": {
        "groups": ["wasi_small", "logging"]
    },
    "files": [],
    "limits": {
        "memory_pages": 64,
        "max_file_handles": 8
    }
}
# Edit this template, then run:
#   perminstall myapp.wasm
```

`permgen` reads the WASM binary, discovers its imports, and suggests groups that cover them. It doesn't write the perm file — it outputs a template for the user to edit.

### B5. The Full Workflow

**Creating permissions for a new program:**
```
$ permgen myapp.wasm
  (reviews suggested groups, edits template)
$ permset myapp.wasm create
$ permset myapp.wasm groups wasi_small display logging
$ permset myapp.wasm file "user/data/*" rw
$ permset myapp.wasm hash auto
  Done. myapp.wasm can now run.
```

**Debugging "why doesn't my program work?":**
```
$ permshow myapp.wasm
  (sees display is BLOCKED by 'common' profile)
$ sp power
  Profile: power
$ permshow myapp.wasm
  (display is now EFFECTIVE)
$ myapp
  (works!)
```

**Releasing a program:**
```
$ permset myapp.wasm hash auto
  Hash: 0x7f3a...
  Perm file locked to this build.
```

---

## Appendix C: Revised New Directions

### C1. Drop the JSON `"hash"` Field

It's already in the filename. It's already verified by the loader against the WASM binary. The JSON field can only disagree with the filename and cause confusion. Remove it. The JSONC schema becomes simpler.

### C2. Phase 1: Flat-Only, No Libraries, No Version Ranges

The 80/20 subset that's actually implementable in a weekend:

| Feature | Phase 1 | Phase 2 | Phase 3 |
|---------|---------|---------|---------|
| Capability groups + import overrides | ✅ | | |
| File access control (fd_open checks) | ✅ | | |
| Profiles + `sp` command | ✅ | | |
| Path canonicalization (lexical, no symlinks) | ✅ | | |
| `permshow` + `permset` tools | ✅ | | |
| Subdirectory-based perm file naming | ✅ | | |
| Library dependencies (hash-pinned) | | ✅ | |
| fd_readdir filtering | | ✅ | |
| `/perms/` read protection | | ✅ | |
| `permclean` garbage collection | | ✅ | |
| Version ranges + ceilings | | | ✅ |
| Transitive dependency resolution | | | ✅ |
| Physical symlink canonicalization | | | ✅ |

### C3. Namespace-Gated Tier 2 vs Tier 3 (The C1 Fix)

The simplest fix for the profile bypass: WASM imports in the `"env"` module namespace go through the profile ceiling. All other modules (`"console"`, `"ui"`, `"loading"`, etc.) are Tier 3 — controlled only by the perm file. This is one `strcmp` in the linker and completely eliminates the bypass.

### C4. Actionable Error Messages (The M3 Fix)

The most impactful ergonomic improvement: when a capability is blocked, tell the user WHY and HOW to fix it. Not `"import 'display_draw' not permitted"` but `"display_draw blocked by profile 'common'. Use 'sp power' to grant display access."`

### C5. /perms/ Read Protection (Deferred but Trivial)

When it matters (multi-user): one `if` statement in `wasm_fd_open` — if the path starts with `/perms/`, return `-EPERM`. The code is trivial. Defer it until there are actually other users.
