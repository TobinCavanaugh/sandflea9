# Command Dispatch & Symlink Resolution for sandfleaOS

**Status:** Design Proposal
**Date:** July 2026
**Updated:** July 2026 — write-through cache, inline storage, brief-lock copy-out
**Author:** Tobin (via Buffy)

---

## 1. The Idea in One Paragraph

We're going to fill **a single restricted directory** with **ext2 symlinks**:
one symlink per command, each pointing at the actual `.wasm` file that
implements that command. The shell's `handle_command()` will check that
directory for every typed command and, if a symlink matches, follow it,
read the target path, and hand it off to `wasm_spawn()`. Built-ins
(`cd`, `cls`, `kill`, `pci`, etc.) stay where they are in C and run before
the symlink scan. This gives us:

- **Discoverability:** `ls //A/bin/` shows every available command.
- **User-installable:** Users can `ln -s /my/fancy/grep.wasm //A/bin/grep`
  (when `ln` exists) to add commands without rebuilding the kernel.
- **No manifest file:** No `/etc/commands.txt` to keep in sync — the
  symlinks themselves *are* the manifest.
- **Symlink support as a kernel feature:** We get exactly what we already
  deferred in `permissions2.md` (C3: physical symlink resolution), and
  use it.
- **Always-fresh cache:** Symlink additions/removals flow through the FS
  layer → cache update in the same atomic write window. No rehash, no
  reboot, no PATH refresh — every shell sees the current set.

---

## 2. Why a Symlink Directory, Not Other Options

| Option | Verdict | Reason |
|--------|---------|--------|
| **Symlink dir with write-through cache (this proposal)** | ✅ Chosen | Filesystem IS the manifest. Cache is always fresh. No refresh commands. Bounded memory. |
| Flat C table: `{ "ls", "//A/wasm/ls.wasm" }` | ❌ | Requires kernel rebuild to add a command; hidden from users; no boot-time seed via `debugfs` |
| Manifest file: `/etc/commands.txt` | ❌ | Boot ordering problem; race conditions across writes; needs its own mini-parser |
| `$PATH` env var | ❌ Overkill | We have at most ~20 commands; multi-drive already gives per-drive lookup. Env-var search is what we want when we have *many* places to install things. Today: fixed dir. |
| Flag-set cache + polling daemon | ❌ Considered & rejected | Polling ties freshness to a timer; write-through is cheaper and provably correct. Reviewed in §5.2. |
| Union directory from `multidrive_design.md` | ⏳ Future | Plan-9-style overlay (`/data/bin` shadows `/A/bin`). Worth doing later — same design we get *for free* once symlink resolution works. Composites with the write-through model: each tier updates its own cache slice. |

**Decision:** Symlink dir at `//A/bin/` (with `//data/bin/` overlay being
the obvious next step). One boot-volume location, one persistent-volume
location, **searched in order** (`data` first — highest priority).

---

## 3. Resolution Pipeline at a Glance

```
                 ┌──────────────────────────┐
user types:      │  handle_command()        │
   "ls /"        │                          │
                 │  1. Builtin table?       │── yes ──▶ execute C builtin (cd, cls, kill…)
                 │       - cd               │            return
                 │       - cls              │
                 │       - kill             │
                 │       - pci              │
                 │       - proc             │
                 │  2. cmd_dir_lookup(name) │── miss ─▶ scan //data/bin/ (if present)
                 │       hash probe (32-slot│            scan //A/bin/
                 │       inline cache,      │── none ─▶ "command not found"
                 │       spinlock briefly)  │
                 │         ↓                │
                 │       target = "..."     │
                 │  3. cmd_dir_lookup_copy( │
                 │       &target_path[])    │  ← acquire lock for the memcpy
                 │  4. wasm_spawn(          │  ← release lock
                 │       path=target_path,  │
                 │       argv=tail_words,   │
                 │       foreground=true)  │
                 │  5. tail of line becomes │
                 │     the wasm program's   │
                 │     argv                 │
                 └──────────────────────────┘
```

Built-ins always beat symlinks. A user who drops a symlink called `cd`
into `//A/bin/` **cannot** shadow the built-in: a warning is logged and
the built-in still runs. (We may want to allow superuser override later,
but the MVP forbids it.)

---

## 4. The Command Directory

### 4.1 Location

- **Primary**: `//A/bin/` — boot drive, volatile per QEMU session,
  ships with the system
- **Overlay (future)**: `//data/bin/` — persistent drive, survives
  reboots, user-installable
- **Search order**: builtins → `//data/bin/` (persistent, higher
  priority) → `//A/bin/` (boot, system defaults)

This matches the "shadow system defaults" pattern from
`multidrive_design.md` §7. The persistent drive wins when both have
the same name — that lets a user install `//data/bin/grep.wasm` that
replaces the boot version.

> **Reserved contents:** `/bin/` MUST contain only symlinks. Real
> files or subdirectories there are ignored by `cmd_dir_lookup()` (see
> §5.8). Active enforcement in Phase 2 (see §6).

### 4.2 Versioning — flat names, NOT subdirectories

Versions live in the **symlink name itself**, not in a subdirectory:

```
//A/bin/grep           → /wasm/grep-1.10.wasm       (default alias)
//A/bin/grep-1.10      → /wasm/grep-1.10.wasm       (pinned version)
//A/bin/grep-1.11      → /wasm/grep-1.11.wasm       (alternative)
//A/bin/python3        → /wasm/python3.12.wasm      (default alias)
//A/bin/python3.12     → /wasm/python3.12.wasm
//A/bin/python3.13     → /wasm/python3.13.wasm
```

Why flat (not `//A/bin/v1/grep`, `//A/bin/v2/grep`):

| Concern | Subdirs | Flat names |
|---|---|---|
| **Lookup cost** | Recursive scan | Single hash probe |
| **Naming freedom** | Forced `v*/` prefix | Free |
| **Stable-version pinning** | `bin/v1/grep` is *one* command in a subdir | `bin/grep-1.10` is itself a command — type it directly, no traversal |
| **Default alias** | Whoever creates `bin/grep` decides | Same — whoever creates the unversioned symlink decides |
| **Discoverability** | Subdirs hidden by default in flat `ls` | All on one screen |
| **Boot-volume cost** | More inodes (each subdir takes one) | One inode per symlink |

The killer reason: `bin/grep-1.10` is **already a stable-pinned command**.
If you want grep 1.10 forever, type it. If you want "whatever the
maintainer recommends," type `grep`. Linux's `/usr/bin/python3 → .../python3.12` does exactly the same thing.

### 4.3 Boot-Time Seeding

`build.sh` populates `//A/bin/` via `debugfs` during ISO packaging:

```sh
# Create the bin dir
mkdir -p /bin

# Built-in (no symlink needed): cd, cls, kill, pci, proc, do, dox, doxw,
#                                 kmalloc, kmalloc2, ext2, touch, open,
#                                 fileinfo, write, wat2wasm, ln (Phase 3)

# Real wasm commands:
ln -s /wasm/ls.wasm          /bin/ls
ln -s /wasm/lsr.wasm         /bin/lsr
ln -s /wasm/cat.wasm         /bin/cat
ln -s /wasm/file_test.wasm   /bin/file_test
ln -s /wasm/add_test.wasm    /bin/add_test
ln -s /wasm/dummy.wasm       /bin/dummy
ln -s /wasm/doom-v0.1.0.wasm /bin/doom
ln -s /wasm/ed.wasm          /bin/ed
ln -s /wasm/sh.wasm          /bin/sh
```

After boot, `ls //A/bin/` will look like:

```
ls          -> /wasm/ls.wasm
lsr         -> /wasm/lsr.wasm
cat         -> /wasm/cat.wasm
doom        -> /wasm/doom-v0.1.0.wasm
ed          -> /wasm/ed.wasm
sh          -> /wasm/sh.wasm
...
```

### 4.4 Search-Path Awareness in Prompts

The status bar (or prompt) shows where a command resolved from:

```
# A:/bin/> doom
[A/bin] doom    ── found in //A/bin  (default boot symlink)
[data/bin] doom ── would find it on persistent drive (overrides boot)
```

This makes `which`-like debugging trivial and signals shadowing. Cheap
add — the lookup result includes a `src_drive` byte.

---

## 5. `cmd_dir_lookup()` — the heart of the design

### 5.1 Algorithm

```c
// Returns a stable view into the cache (no allocation) if found.
// Zero length view if not found.
//
// `name`         — first word of the command line (e.g. view of "ls").
// `out_source`   — optional; if non-null, filled with the resolved
//                   target view (e.g. "/wasm/ls.wasm").
// `out_drive`    — optional; which bin dir it was found in.
//
// Resolution order:
//   1. Builtin table (handled by handle_command, not here)
//   2. Hash-probe the inline cache (32 slots, write-through up-to-date)
//   3. Cache miss → scan //data/bin/ (if present), insert on hit
//   4. Cache miss → scan //A/bin/, insert on hit
//   5. Otherwise: "command not found"
str_view_t cmd_dir_lookup_view(str_view_t name);
bool       cmd_dir_lookup_copy(str_view_t name, char *out_buf, u32 out_cap);
bool       cmd_dir_lookup_full(str_view_t name,
                               str_view_t *out_target,
                               u8         *out_src_drive);
```

### 5.2 Cache: Inline Storage, Write-Through

**Two non-negotiable refinements** from the original sketch:

1. **Inline storage**: each cache entry embeds `name[64]` and `target[256]`
   directly. No kmalloc per entry. The whole cache is a fixed-size array
   in `.bss` — always-resident, never freed, never leakable.
2. **Write-through**: every symlink operation that lands in a bin dir
   updates the cache **atomically with the disk write success**, never
   speculatively. Cache lag is impossible by construction.

```c
// One fixed-size slot in a 32-element array — never kfree'd, never kmalloc'd.
typedef struct cmd_dir_entry {
    char  name[64];        // command name ("grep", "lsr", "python3.12")
    u16   name_len;
    char  target[256];     // symlink target — covers both fast & long symlinks
    u16   target_len;
    u8    used;            // 0 = empty slot, 1 = occupied
    u8    src_drive;       // which cmd bin dir (A=0, data=1, ...)
    u32   src_inode;       // for debugging, written at insert time
} cmd_dir_entry_t;

#define CMD_DIR_CACHE_SIZE 32
static cmd_dir_entry_t g_cache[CMD_DIR_CACHE_SIZE];   // ~11 KB. Always resident.
static u32             g_cache_count;
static spinlock_t      g_cmd_dir_lock;                // single global lock
static bool            g_cmd_dir_initialized;
```

**Why this shape dissolves the read/write concurrency argument:**

- **No kmalloc per entry.** Nothing to leak, nothing to dangle. A "delete"
  just flips `used = 0`. An "insert" overwrites the slot in place.
- **A reader's view is stable for the duration of one lookup.** Returning
  `str_view_t { data = &slot->target[0], len = slot->target_len }` is a
  pointer into always-resident memory. The data may change on a
  concurrent write, but the pointer never becomes dangling.
- **A caller who intends to keep a path across subsequent FS operations
  is expected to copy out under the brief lock** (see §5.7).

**How write-through fires:**

```c
// ext2_create_symlink completes:
//   1. disk write to data block + inode + dir entry
//   2. only if (1) succeeded: cmd_dir_update_for_new_link(parent,name,target)
//      which acquires g_cmd_dir_lock, copies into the cache, releases.
// ext2_unlink completes:
//   1. dir entry cleared + inode freed
//   2. only if (1) succeeded: cmd_dir_update_for_removed_link(parent,name)
//      which acquires the lock, marks the slot used=0, releases.
```

If the disk write fails, the cache stays unchanged. There is no scenario
where the cache and the disk diverge — the cache *only* reflects
operations that have already succeeded on disk.

> **Why not flag-set + polling daemon?** Reviewed and rejected. A
> daemon is a thread, requires scheduler cooperation, introduces a
> "staleness window" between disk-write-success and cache-rebuild, and
> makes in-flight behavior depend on timer tick rate. Write-through is
> cheaper, faster, and provably correct. The kernel has a natural
> "moment" right after a successful disk op — use it.

### 5.3 Lookup primitives

```c
// ── Fast read path ─────────────────────────────────────────────────────
// Lock is held only for the hash probe + slot check. Reader returns a
// str_view_t pointing into a slot's inline buffer; the data is
// guaranteed-resident, but may change on a concurrent write. If the
// caller needs durability, follow up with cmd_dir_lookup_copy().
str_view_t cmd_dir_lookup_view(str_view_t name) {
    str_view_t view = {0};
    if (!g_cmd_dir_initialized) return view;

    spinlock_acquire(&g_cmd_dir_lock);
    for (u32 i = 0; i < CMD_DIR_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        str_view_t slot_name = str_view_from_parts(g_cache[i].name, g_cache[i].name_len);
        if (str_view_eq(slot_name, name)) {
            view = str_view_from_parts(g_cache[i].target, g_cache[i].target_len);
            break;
        }
    }
    spinlock_release(&g_cmd_dir_lock);

    if (view.data) return view;

    // Cache miss → on-disk fallback. See §5.4.
    return cmd_dir_scan_fallback(name);
}

// ── Stable copy: hold the lock only for the memcpy ──────────────────────
// Returns true on hit; out_buf is null-terminated. This is the version
// handle_command uses, because the path is passed to wasm_spawn later.
bool cmd_dir_lookup_copy(str_view_t name, char *out_buf, u32 out_cap) {
    bool found = false;
    if (!g_cmd_dir_initialized) goto fallback;

    spinlock_acquire(&g_cmd_dir_lock);
    for (u32 i = 0; i < CMD_DIR_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        str_view_t slot_name = str_view_from_parts(g_cache[i].name, g_cache[i].name_len);
        if (str_view_eq(slot_name, name)) {
            u32 n = (g_cache[i].target_len < out_cap - 1) ? g_cache[i].target_len : out_cap - 1;
            mem_copy((u8*)out_buf, (const u8*)g_cache[i].target, n);
            out_buf[n] = 0;
            found = true;
            break;
        }
    }
    spinlock_release(&g_cmd_dir_lock);

    if (found) return true;
fallback:
    return cmd_dir_scan_fallback_into(name, out_buf, out_cap);
}
```

Lock windows are **O(strlen)** — single-digit microseconds at most. On
multicore future, contention is brief enough that readers effectively
don't block.

### 5.4 `ext2_read_symlink(inode, out_buf)` — kernel-level addition

ext2 has two symlink storage kinds:

| Kind | When | How to read |
|------|------|-------------|
| **Fast symlink** | target ≤ 60 chars | Stored *inside* `inode.block[0..14]` as 15 × `u32 = 60` bytes |
| **Long symlink** | target > 60 chars | Stored in the data block(s) the inode points at, no null terminator; `inode.size` is the length |

Read:

```c
// Reads the symlink target into out_buf (size out_buf_cap).
// Returns the number of bytes written, or 0 on error.
u32 ext2_read_symlink(u32 inode_no,
                     char *out_buf, u32 out_buf_cap) {
    ext2_inode_t inode;
    if (!ext2_get_inode(inode_no, &inode)) return 0;
    if ((inode.mode & 0xF000) != 0xA000) return 0;  // not a symlink

    u32 target_len = inode.size;
    if (target_len >= out_buf_cap) target_len = out_buf_cap - 1;

    if (inode.size <= 60) {
        // Fast symlink: target is in inode.block[0..14]
        mem_copy((u8*)out_buf, (u8*)inode.block, target_len);
    } else {
        // Long symlink: read first data block
        u32 phys = ext2_get_bmap(&inode, 0);
        u8 *blk = kmalloc(block_size);
        if (!blk) return 0;
        ext2_read_block(phys, blk);
        mem_copy((u8*)out_buf, blk, target_len);
        kfree(blk);
    }
    out_buf[target_len] = 0;
    return target_len;
}
```

> **`mem_copy` on `inode.block[]`:** The fast symlink target is
> little-endian bytes packed into 15 uint32s. We copy them as bytes —
> the kernel is x86_64 LE so this works directly. If we ever port to
> BE we'd need a byte swap.

### 5.5 Plumbing symlink-following into `ext2_find_path`

Today `ext2_find_path` returns the directory's *raw* inode, ignoring
mode. We need one new step:

```c
// In ext2_find_path's loop, after ext2_find_child succeeds:
const u32 MAX_SYMLINK_DEPTH = 8;
for (u32 depth = 0; depth < MAX_SYMLINK_DEPTH; depth++) {
    if ((child_inode.mode & 0xF000) != 0xA000) break;  // not a symlink, stop
    char target_buf[256];
    u32 n = ext2_read_symlink(current_inode_no, target_buf, 256);
    if (n == 0) return null;  // broken link

    // For MVP, support only absolute symlink targets (the build.sh
    // creates only /wasm/... or //drive/... targets).
    if (target_buf[0] != '/') return null;  // relative — not supported yet

    // Cross-drive or same-drive: re-enter ext2_find_path.
    // It handles //drive/ prefix for us.
    return ext2_find_path(target_buf, out_inode_no);
}
return null;  // depth limit hit
```

**Two consequences:**

1. `fs_open` and `wasm_spawn` automatically work — they go through
   `ext2_find_path`, so symlink resolution is transparent.
2. `ls //A/bin/` will list symlinks themselves, not their targets —
   `wasm_ls` prints the entry's name only without dereferencing.
   That's correct ("ls shows directory contents" semantics).

### 5.6 Cycle detection

A symlink loop would hang the kernel. Conservative rules:

- `cmd_dir_lookup_view/copy` does **not** walk through symlinks — it
  reads a single symlink's target string and trusts it. The lookup is
  one hop deep into the cache, not into the filesystem's symlink chain.
- `ext2_find_path` does walk, but bounded at 8 hops. A symlink cycle
  (A→B→A) hits `MAX_SYMLINK_DEPTH` and returns null; the caller prints
  "too many levels of symbolic links".
- Hash collisions on the cache don't trigger filesystem walks either —
  the cache is one entry per name, with `name_len` checked first to
  reject early collisions (FNV-1a + linear probe).

This means **cycles are not a denial-of-service vector** — bounded
depth + bounded cache probe + read-only /bin/ by convention make it
safe.

### 5.7 Concurrency Model — write-through + brief lock

The kernel's threading model today is single-core with cooperative +
timer-driven preemption at ~100 Hz. Two threads can run between any
two user-visible kernel operations. The model we want:

```
                       ┌───────────────────────────────────────────┐
                       │ g_cmd_dir_lock (single spinlock)          │
                       └───────────────────────────────────────────┘
                                       ▲ ▲
                                       │ │
   writer A: ext2_create_symlink ──spinlock_acquire──► slot[i] ← name,target
                                       |              used=1
                                       └──spinlock_release──┘
                                       (disk write succeeded BEFORE acquiring lock)

   writer B: ext2_unlink ──────────spinlock_acquire──► slot[i].used = 0
                                       └──spinlock_release──┘

   reader R: cmd_dir_lookup_view ──spinlock_acquire──► probe slots, take view
                                       |              into slot[i].target[]
                                       └──spinlock_release──┘
                                       (view is non-owning; valid until next
                                        write to this slot)

   reader R: cmd_dir_lookup_copy ──spinlock_acquire──► probe, memcpy into out_buf
                                       └──spinlock_release──┘
                                       (out_buf is local; fully owned by caller)
```

**Key properties:**

- **Disk write is OUTSIDE the lock.** It happens first, sequentially.
  If it fails, the cache is *not* updated. The lock window is the cache
  manipulation only — a memcpy + a couple of bools. Sub-millisecond,
  even on multicore.
- **Reads are non-blocking from the caller's perspective.** Worst case
  is "you wait until the in-flight memcpy or slot insertion completes."
- **Cache and disk cannot diverge.** Cache update only follows disk
  success. There is no background reconciler that can race against a
  writer, no daemon that can miss an update.
- **A reader's view into `slot.target[]` is a pointer into always-
  resident memory.** The data there may change under a concurrent
  write, but the pointer never becomes dangling (we don't free slots,
  we just `mem_set()` or overwrite in place). Callers that plan to hold
  a target across subsequent operations should use `lookup_copy`.
- **On multicore future**, a single `g_cmd_dir_lock` is sufficient.
  Critical sections are O(strlen) which is ~10 CPU cycles. The lock is
  uncontended on the read path ~99% of the time.

> **Why a single lock, not RCU / per-entry seqlock?** Considered.
> RCU + seqlock buys lock-free reads at the cost of (a) retry loops for
> readers who hit a generation change, (b) much harder reasoning about
> invariants, (c) more code than the entire schematic above. At ~20
> entries with sub-microsecond critical sections, the lock is right.

### 5.8 Subdirectory policy — browse-yes, lookup-no

`cmd_dir_scan` walks the bin dir. Two rules:

1. **Skip directories silently during lookup.** They're for *browsing*
   (`ls //A/bin/` shows them, `cd //A/bin/subdir` works for inspection).
   Lookup stays flat — single-level, by symlink name.
2. **Skip non-symlink files.** Only `ext2_dir_entry_t.file_type ==
   EXT2_FT_SYMLINK` entries are added to the cache. Directories are
   `EXT2_FT_DIR`. Regular files are `EXT2_FT_REG_FILE`. Both are
   ignored with a serial-log debug message at most.

This dodges the entire "what if user creates nested symlinks" question.
The convention is "don't sandflea-bin inside `bin/`." Future shell
helpers like `tree //A/bin/` make browsing easy without polluting the
lookup tables.

```c
// In cmd_dir_scan_for_one_drive, when iterating entries:
if (ext_result.file_type == EXT2_FT_DIR) continue;   // browse but don't lookup
if (ext_result.file_type != EXT2_FT_SYMLINK) {
    serial_outsf("CMD_DIR: ignoring non-symlink //%s/bin/%s\n", ...);
    continue;
}
// ... read symlink target, insert into cache
```

---

## 6. Enforcement: "only symlinks in the bin dir"

The user said "a directory which is restricted to only allow symlinks."
Two layers, applied in order:

### 6.1 Convention (always-on)

`cmd_dir_lookup` silently skips non-symlinks. Convention enforcement
is free and ships from day one.

### 6.2 Active enforcement (Phase 2)

A new hook in `ext2_create_file_in_dir`:

```c
// Register bin dirs at boot by inode:
//   g_cmd_bin_dirs[] = { { drive_idx, parent_inode_no, BIN_FLAG_A },
//                        { drive_idx, parent_inode_no, BIN_FLAG_DATA } };
//
// The parent_inode_no is the dir's inode, so the check is O(1) compare.
//
// In ext2_create_file_in_dir:
if (bin_dir_lookup(parent_inode_no, &bin_idx)) {
    // Force-reject anything that isn't a symlink creation.
    // (For create_symlink: allow. For create_file: EACCES.)
    if (new_inode_mode != 0xA000) {
        serial_outsf("CMD_DIR: rejecting non-symlink in //%s/bin\n",
                     bin_idx_to_drive_name(bin_idx));
        return 0;  // EACCES
    }
}

// In ext2_unlink_in_dir:
if (bin_dir_lookup(parent_inode_no, &bin_idx)) {
    // OK to unlink — but make sure the cache update fires afterward.
}
```

Why this matters now: **we already need the bin-dir parent inode
registry** for write-through to fire on the right path. With that in
place, adding the rejection branch is one more `if`, not a separate
development stream. Move enforcement up to Phase 2.

---

## 7. Built-ins vs. WASM Commands

### 7.1 What stays built-in

| Command | Why built-in |
|---------|-------------|
| `cd` | Mutates `cwd` (kernel-only state); can't reasonably run as wasm |
| `cls` | Writes to framebuffer directly; needs kernel cursor state |
| `kill` | Calls kernel scheduler state (`sched_get_proc_by_pid`) |
| `pci` | Reads PCI config space directly |
| `proc` | Reads sched process list |
| `do` / `dox` / `doxw` | I/O-port read/write; sandboxed out of wasm |
| `kmalloc` / `kmalloc2` | Allocator stats; kernel-only |
| `ext2` | Direct inode/block manipulation |
| `touch` / `write` (if non-WASM variant) | Field test of fs_write via C |
| `open` | File-descriptor test helper |
| `ln` | Symlink creation (Phase 3) |
| `rm` | Symlink removal (Phase 3) |
| `which` | Reports where a command resolves from (Phase 4) |

### 7.2 What moves to symlinks

| Command | Current dispatch | New home |
|---------|------------------|----------|
| `cat` | C: `wasm_spawn(...)` | `//A/bin/cat → /wasm/cat.wasm` |
| `ls` | C: `wasm_spawn(...)` | symlink |
| `lsr` | C: `wasm_spawn(...)` | symlink |
| `ed` | C: `wasm_spawn(...)` | symlink |
| `doom` | C: `wasm_spawn(...)` | symlink |
| `sh` | C: `wasm_spawn(...)` | symlink |
| `wasm` (test harness) | C: a wasm_test wrapper | **stays C** (test harness) OR folds into a `wasm` shell builtin |
| `wat2wasm` | C: host tool | **stays C** (host tool, not a wasm process) |
| `file_test` / `add_test` | C: tests | symlinks (`//A/bin/file_test`) |

> Note the asymmetric `wat2wasm`. It's a host-native tool that calls
> `wat2wasm_compile()` or `wat2wasm_native()` directly. It does not
> spawn a wasm process. Built-ins always win, so a user cannot
> accidentally make `wat2wasm.wasm` and have the kernel try to spawn it
> — that's the resistance mechanism.

### 7.3 Refactoring handle_command

```c
// src/kernel/kern_cmd_dir.c — dispatch helpers

void handle_command(void) {
    cmd_word_t *root = cmd_parse(typingbuf, kmalloc);
    cmd_word_t *word = root;
    if (!word) goto Label_Free;

    // 1. Builtin table — builtins always win
    for (u32 i = 0; builtins[i].name; i++) {
        if (cmd_word_eq(word, builtins[i].name)) {
            builtins[i].handler(word);
            goto Label_Free;
        }
    }

    // 2. cmd_dir lookup -> stable copy of the target path
    char target_path[256];
    u8 src_drive = 0;
    if (!cmd_dir_lookup_full(cmd_word_view(word), target_path, 256, &src_drive)) {
        screen_push_linef("command not found: %.*s",
                          word->len, word->loc);
        goto Label_Free;
    }

    // 3. Build argv from the tail of the command line
    int argc = 1;
    char **argv_list = kmalloc(sizeof(char*) * 32);
    argv_list[0] = kmalloc(word->len + 1);
    mem_copy((u8*)argv_list[0], (const u8*)word->loc, word->len);
    argv_list[0][word->len] = 0;

    for (cmd_word_t *w = word->next; w; w = w->next) {
        if (argc >= 31) break;
        argv_list[argc++] = str_view_to_c(cmd_word_view(w));
    }
    argv_list[argc] = null;

    // 4. Spawn
    wasm_spawn_opts_t opts = {
        .path       = str_view_from_c(target_path).data,  // already null-terminated
        .argc       = argc,
        .argv       = argv_list,
        .foreground = false,
        .wait       = true,
    };
    wasm_spawn(&opts);

Label_Free:
    cmd_parse_free(root, kfree);
}
```

> **Why `lookup_full` instead of `lookup_view`?** Because we're about
> to hand the path to `wasm_spawn`, which goes through several other
> operations. A view could be invalidated between lookup and spawn
> if a concurrent FS write touched the slot. Copy under the lock; safe
> thereafter. (Per §5.7.)

> **Why default `foreground=true, wait=true`?** Today's shell is
> synchronous — when you type `cat file.txt`, you wait for output
> before seeing the next prompt. Async is a bigger refactor (return-
> to-prompt-rapidly). Phase 2 may add `&` for background.

### 7.4 Refactor pay-off

Once complete, `kern_tests.c::handle_command` is **just the table
lookup + builtin dispatch + spawn**. The giant `if/else` chain shrinks
from ~600 lines to ~50. Each existing builtin's handler stays where
it is, but the explicit `wasm_spawn(...)` blocks move out as symlinks
in `//A/bin/`.

---

## 8. Migration Plan

### Phase 0 — Inventory (1 day)

- List every `wasm_spawn(...)` call site in `kern_tests.c`.
- List every `cmd_word_eq(...)` which spawns a wasm process.
- Decide: which move to symlinks, which stay built-in.

Result: a table like §7.2 above.

### Phase 1 — Symlink support in the kernel (3 days)

1. Add `ext2_read_symlink(inode_no, out_buf, cap)` in `kern_ext2.c`.
2. Add `ext2_create_symlink_in_dir(name, parent, target)` in `kern_ext2.c`.
   - Allocates inode, sets mode to 0xA1FF, fast-or-long based on
     `target.len`.
   - After successful disk write, fires `cmd_dir_update_for_new_link`
     (no-op until Phase 2; today just logs the target).
3. Modify `ext2_find_path` to detect `mode & 0xF000 == 0xA000` and
   follow up to `MAX_SYMLINK_DEPTH` (8). Bails null on cycles.
4. Add unit tests in `kern_tests.c`:
   - Create fast symlink, read it back → assert target matches.
   - Create long symlink, read it back → assert.
   - Create a cycle, `ext2_find_path` returns null within 8 hops.
5. Build a debug-only IFS image with symlinks, qemu-boot it,
   confirm `//A/bin/ls → /wasm/ls.wasm` resolves via `ext2_find_path`.

### Phase 2 — `cmd_dir` kernel module + write-through (3-4 days)

1. Add `kern_cmd_dir.h/c`:
   - Cache array (`g_cache[32]`) with inline `name[64]`, `target[256]`.
   - Single spinlock `g_cmd_dir_lock`.
   - `cmd_dir_lookup_view/copy/full`.
   - `cmd_dir_update_for_new_link/removed_link` (called from FS ops).
2. Register bin dirs at boot:
   - `cmd_dir_register_bin(drive_idx, parent_inode_no)` populates
     `g_cmd_bin_dirs[]`. Called during `ext2_init` after drives probed.
3. Wire write-through:
   - `ext2_create_symlink_in_dir` → `cmd_dir_update_for_new_link` after disk success.
   - `ext2_create_file_in_dir` (reject in bin dirs if mode != 0xA000).
   - New `ext2_unlink_in_dir` → `cmd_dir_update_for_removed_link`.
   - New `ext2_symlink_set_target` (ln -sf) → `cmd_dir_update_for_new_link` (overwrite slot).
4. Seed `//A/bin/` symlinks in `build.sh` via `debugfs` (per §4.3).
5. Refactor `handle_command` to the table-based shape (§7.3).
6. Move built-ins-with-spawn branches out.
7. Test: `cat foo.txt` resolves via `//A/bin/cat` to `/wasm/cat.wasm`.
   Insert symlink live, verify next lookup hits.

### Phase 3 — User-installable `ln` / `rm` (1-2 days)

1. Add a built-in `ln -s <target> <path>` that calls
   `ext2_create_symlink_in_dir`.
2. Add a built-in `rm <path>` that calls `ext2_unlink_in_dir`.
3. Verify each operation updates the cache and a concurrent shell
   sees the change immediately.
4. `ls //A/bin/`, `tree //A/bin/` (if a tree wasm exists) show the
   new entries.

### Phase 4 — Persistent overlay + drop-in (later)

1. Add `//data/bin/` registration in `cmd_dir_register_bin`.
2. Search order: `data` first, `A` second.
3. Add `which` built-in.
4. Add a usage-cost heuristic: if `grep` is missing, auto-prune from
   alert list (deprecated?).

### Phase 5 — Multicore stress (later, with SMP)

1. Add an actual stress test: two core threads installing and removing
   symlinks while a third spawns `cat` etc.
2. Confirm spinlock contention is below 1% in steady state.

---

## 9. Security & Edge Cases

### 9.1 Symlink-target tampering

If `//A/bin/doom` points at `/wasm/doom-v0.1.0.wasm` and someone
replaces `/wasm/doom-v0.1.0.wasm` between boots, the symlink still
resolves to the new file. **No security issue** — both live in
`//A/wasm/`, which is RW today. Per-process permissions enforcement
will happen at open time (`vmm`/filesystem), not at dir-lookup time.

### 9.2 Cross-drive targets

`//A/bin/grep → //data/grep.wasm` is supported. Cross-drive resolution
works via `ext2_find_path` re-entry. `wasm_spawn` takes a path, not a
drive — no special handling needed.

### 9.3 Relative symlinks

`//A/bin/foo → ../mycode/foo.wasm` (relative). For MVP: **reject** in
`ext2_find_path` — log warning, return null. Reason: relative
symlinks combined with `cwd` rewriting (which happens at `cd` time)
create bizarre behavior. Absolute paths are the cleanest contract.
Phase 2: support relative with explicit cwd-context capture.

### 9.4 Dangling symlinks

`//A/bin/foo → /wasm/missing.wasm`. `cmd_dir_lookup` returns the target
string anyway (it's a string match — the target file isn't checked).
`wasm_spawn` then `fs_open`s it, fails, prints an error, returns to
the prompt. **No kernel crash** — handled cleanly.

If we want to be helpful, `cmd_dir_lookup_full` could pass
`O_NOFOLLOW`-style semantics — "verify target exists" — but that's a
second inode read per lookup. Skip for MVP.

### 9.5 Built-in shadowing

A user creates `//A/bin/cd → /wasm/cd.wasm`. `handle_command` sees `cd`
in the builtin table first and runs the C builtin. The symlink is
never read. The user sees no shadowing. (Future: superuser override —
defer.)

### 9.6 Symlink to non-wasm file

`cat foo.txt` runs `wasm cat.wasm foo.txt`. If `//A/bin/cat` is
dangling or points at a non-wasm file, `wasm_spawn` fails cleanly via
`m3_ParseModule` returning an error. Handled.

### 9.7 Cache freshness — write-through eliminates staleness

The cache is *always* current. There is no staleness window because
there is no asynchronous reconciler. Symlinks created via the kernel
`ln` builtin trigger the cache update atomically with disk-write-
success. Boot-time seed symlinks are in the cache after
`cmd_dir_init`. Permanent drive-mount writes (out-of-band, e.g.
debugfs on a still-attached drive) are the one blind spot — same as
today's block cache. Document it; don't solve.

### 9.8 DoS via symlink cycles

`MAX_SYMLINK_DEPTH=8` means a cycle is O(8) per `ext2_find_path` call.
Not a panic vector. The cmd_dir-level lookup is one-hop into the cache,
so cycle attempts at lookup time don't even touch the filesystem.

### 9.9 Long-lived view gotcha

The `cmd_dir_lookup_view` API returns a `str_view_t` whose `.data`
points into `g_cache[i].target[]`. The memory is always-resident and
the pointer will never become dangling (no slot is ever kfree'd).
**However**: a concurrent `cmd_dir_update_for_new_link` may overwrite
the slot's target bytes. The view's pointer is still valid (it points
into the array), but the data it points at is now somebody else's.

**Rule for callers:**

- Use `lookup_view` if the path is consumed within the current call —
  one syscall, one `str_view_eq`, no cache write in between. Safe.
- Use `lookup_copy` (or `lookup_full`) if the path will be passed
  across functions or stored. Stable across subsequent ops.
- `handle_command` uses `lookup_full` because the path goes to
  `wasm_spawn` which reads it several times.

### 9.10 Spinlock contention on multicore future

The lock window for reads is ~10 CPU cycles (probe + memcmp of equal
names — most lookups miss after the first `used` check on a hot slot).
Writes are O(strlen) memcpy + bool flip. On 8 cores doing nothing but
path lookups, contention is below 1%. Acceptable.

---

## 10. Performance

### 10.1 Cost of `cmd_dir_lookup_copy` (cache hit — hot path)

- Acquire spinlock: ~5 cycles
- Probe up to 32 slots checking `used` then `name_len` then name memcmp: at most a few hundred cycles
- `memcpy(target_len bytes)` into stack buffer: ≤256 cycles for any reasonable target
- Release spinlock: ~5 cycles

Total: **~500 cycles ≈ 0.2 µs** on modern x86. With cache hit rate of
~99% (steady state — we type the same `ls` hundreds of times per
session), the shell feels instantaneous.

### 10.2 Cost on cache miss (cold)

- Walk `//data/bin/` (if drive present): 1 IDE read of the dir block,
  parse up to ~20 entries.
- Same for `//A/bin/`.
- For each symlink entry, `ext2_read_symlink` reads a fast symlink
  (zero extra reads — already in inode table block) or a long
  symlink (one extra data block read).

Total: 1-3 IDE reads, ~1 ms for the worst case. After one lookup,
the entry stays in the cache.

### 10.3 Memory footprint

- Cache: 32 × ~360 B ≈ **11.5 KB**. One page of kernel memory.
- Always resident — never allocated or freed.
- Fits in L1 on modern x86 (32 KB+ L1D).

### 10.4 Hot-path comparison

Today: each command already issues:
- `cmd_word_eq` calls (slice comparisons on input buffer)
- A syscall-equivalent: `wasm_spawn(...)`
- Several `fs_open` calls

Adding `cmd_dir_lookup_copy` adds ~0.2 µs. Below noise floor.

---

## 11. SFS Integration (Future)

The symlink-dir design **is the symbol table** that SFS relies on for
resolving program names to wasm files. SFS queries program schemas
(exported types); the symlink dir tells it *where* the wasm file
lives. The two layers compose cleanly:

- **shell layer** (today): `ls` → look up `//A/bin/ls` → read symlink
  → spawn wasm at target
- **SFS layer** (future): `let $x = ls(...)` → same lookup → then
  schema-verify the resolved module against expected signature

We get symlink-based dispatch for free; SFS builds on top. The
write-through contract means SFS's symbol-resolver always agrees with
the current disk state.

---

## 12. Resolved Decisions

The original draft had open questions; with the modeled decisions,
these are now resolved:

1. ~~**Which cache type?**~~ → **Inline storage array, single global
   spinlock.** Write-through is the update contract. See §5.2 and §5.7.
2. ~~**Subdirectories in `/bin/`?**~~ → **Browse yes, lookup no.** See
   §5.8. Versions live in the symlink name itself (§4.2).
3. ~~**Daemon vs polling?**~~ → **Neither.** Write-through is
   synchronous, simple, and provably correct.
4. ~~**Long-lived view gotcha?**~~ → Resolved. Use `lookup_copy` /
   `lookup_full`. Documented in §9.9.

### Still open (lower priority)

5. **`which` command:** Do we expose `which ls`? Linux users expect
   this. Cheap to add — uses `cmd_dir_lookup_full`.
6. **Target-prefix semantics:** Should symlink targets be allowed to
   include argv? SFS's `@opt=val` style? MVP: no, 1-to-1 mapping.
7. **Foreground vs background:** `&`. Defer to Phase 4.

### Resolved by §7.1 / §7.2

- `cd`, `cls`, `kill`, `pci`, `proc`, `do*`, `kmalloc*`, `ext2`, `touch`,
  `open`, `fileinfo`, `write`, `ln`, `rm`, `which` — built-in C
  handlers.
- `cat`, `ls`, `lsr`, `ed`, `doom`, `sh`, `file_test`, `add_test` —
  drive `//A/bin/` symlinks.
- `wasm` test harness — C, host-side.
- `wat2wasm` — C, host tool, never a wasm program.

---

## 13. Summary

| Aspect | Choice |
|--------|--------|
| **Approach** | Restricted symlink dir at `//A/bin/` (+ overlay `//data/bin/`) |
| **Lookup algorithm** | Builtins first → hash-probe cache → scan `//data/bin/` → scan `//A/bin/` → "not found" |
| **Symlink resolution** | `ext2_find_path` follows ext2 symlinks, depth ≤ 8 |
| **Cache layout** | Fixed-size inline slots (`cmd_dir_entry_t` × 32, `~11 KB`) |
| **Cache update contract** | Write-through — disk write success → `cmd_dir_update_*` under spinlock |
| **Concurrency** | Single global spinlock `g_cmd_dir_lock`; held only during cache manipulation (memcpy ~10 cycles); readers copy out under lock if path persists |
| **Cache freshness** | Always current. No polling, no daemon. Modified cache in Phase 4 if out-of-band writes are detected. |
| **Symlink names** | Flat. Versions embedded in name (`grep-3.12`, not `v3/grep`). |
| **Subdirectory policy** | Browse (`ls`, `cd`) yes; lookup no. Lookup walks only symlinks. |
| **Built-ins win** | Builtin table checked first; no shadowing. |
| **Built-ins stay** | `cd`, `cls`, `kill`, `pci`, `proc`, `do*`, `kmalloc*`, `ext2`, `touch`, `open`, `fileinfo`, `write`, `ln`, `rm`, `which`, `wat2wasm`, `wasm` test harness |
| **Commands → symlinks** | `cat`, `ls`, `lsr`, `ed`, `doom`, `sh`, `file_test`, `add_test` |
| **Enforcement** | Convention (Phase 1) — lookup silently skips non-symlinks. Active enforcement (Phase 2) — `ext2_create_file_in_dir` rejects non-symlinks in registered bin dirs. |
| **Edge cases** | Dangling → "open failed"; cycles → bounded; relative symlinks → reject; cross-drive → supported; long-lived views → use `lookup_copy` |
| **SFS** | This IS SFS's symbol-table layer; SFS builds on top |

**Effort**: Phase 0–3 ≈ 1.5 weeks of focused work. Phase 4–5 later.

**Win**: `handle_command()` shrinks from ~600 lines of if/else to ~50
lines + a builtin handler table. Users can `ls //A/bin/` to see what's
available. New commands are added at boot via `build.sh`, or at
runtime via `ln -s` (Phase 3). Symlink resolution becomes a kernel
feature we already deferred in `permissions2.md` — paying off that
debt and the user-provided write-through agree on it being right.

---

## 14. Code Shape Snapshot

```c
// src/include/kern_cmd_dir.h
#ifndef KERN_CMD_DIR_H
#define KERN_CMD_DIR_H

#include "../util/str_slice.h"

typedef struct cmd_dir_entry {
    char  name[64];
    u16   name_len;
    char  target[256];
    u16   target_len;
    u8    used;
    u8    src_drive;
    u32   src_inode;
} cmd_dir_entry_t;

#define CMD_DIR_CACHE_SIZE 32

typedef struct cmd_bin_dir {
    u8  drive_idx;
    u32 parent_inode_no;  // bin dir's inode, registered at boot
} cmd_bin_dir_t;

#define CMD_BIN_DIRS_MAX 4

void cmd_dir_init(void);

// Public read API
str_view_t cmd_dir_lookup_view(str_view_t name);
bool       cmd_dir_lookup_copy(str_view_t name, char *out_buf, u32 out_cap);
bool       cmd_dir_lookup_full(str_view_t name,
                               char *out_buf, u32 out_cap,
                               u8 *out_src_drive);

// Write-through hooks (called by FS ops on success)
void cmd_dir_update_for_new_link(u32 parent_inode_no,
                                 str_view_t name,
                                 str_view_t target);
void cmd_dir_update_for_removed_link(u32 parent_inode_no,
                                    str_view_t name);

// Bin dir registration (Phase 2)
void cmd_dir_register_bin(u8 drive_idx, u32 parent_inode_no);
bool cmd_dir_is_bin_dir(u32 parent_inode_no, u8 *out_bin_idx);

#endif
```

```c
// src/kernel/kern_cmd_dir.c  (skeleton)
#include "../include/kern_cmd_dir.h"

static cmd_dir_entry_t g_cache[CMD_DIR_CACHE_SIZE];
static u32             g_cache_count;
static spinlock_t      g_cmd_dir_lock;
static bool            g_cmd_dir_initialized;

static cmd_bin_dir_t   g_cmd_bin_dirs[CMD_BIN_DIRS_MAX];
static u32             g_cmd_bin_dir_count;

static u32 hash_name(str_view_t name) {
    u32 h = 0x811c9dc5;
    for (u32 i = 0; i < name.len; i++) {
        h ^= (u8)name.data[i];
        h *= 0x01000193;
    }
    return h % CMD_DIR_CACHE_SIZE;
}

void cmd_dir_init(void) {
    mem_set((u8*)g_cache, 0, sizeof(g_cache));
    mem_set((u8*)g_cmd_bin_dirs, 0, sizeof(g_cmd_bin_dirs));
    g_cmd_dir_initialized = true;

    // Seed: register known bin dirs. //A/bin/ is created at boot;
    // //data/bin/ is created when data drive detected. We probe here.
    cmd_dir_register_bin(0 /*drive A*/, /*bin inode*/ 11 /* placeholder */);
    // (a real implementation reads the bin dir inode from the parent read)
}

void cmd_dir_register_bin(u8 drive_idx, u32 parent_inode_no) {
    if (g_cmd_bin_dir_count >= CMD_BIN_DIRS_MAX) return;
    g_cmd_bin_dirs[g_cmd_bin_dir_count].drive_idx = drive_idx;
    g_cmd_bin_dirs[g_cmd_bin_dir_count].parent_inode_no = parent_inode_no;
    g_cmd_bin_dir_count++;
}

bool cmd_dir_is_bin_dir(u32 parent_inode_no, u8 *out_bin_idx) {
    for (u32 i = 0; i < g_cmd_bin_dir_count; i++) {
        if (g_cmd_bin_dirs[i].parent_inode_no == parent_inode_no) {
            if (out_bin_idx) *out_bin_idx = (u8)i;
            return true;
        }
    }
    return false;
}

void cmd_dir_update_for_new_link(u32 parent_inode_no,
                                 str_view_t name,
                                 str_view_t target) {
    u8 bin_idx;
    if (!cmd_dir_is_bin_dir(parent_inode_no, &bin_idx)) return;

    spinlock_acquire(&g_cmd_dir_lock);
    // Try to find existing slot for this name; otherwise first empty.
    i32 slot = -1;
    for (u32 i = 0; i < CMD_DIR_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        str_view_t slot_name = str_view_from_parts(g_cache[i].name, g_cache[i].name_len);
        if (str_view_eq(slot_name, name)) { slot = (i32)i; break; }
    }
    if (slot < 0) {
        for (u32 i = 0; i < CMD_DIR_CACHE_SIZE; i++) {
            if (!g_cache[i].used) { slot = (i32)i; break; }
        }
    }
    if (slot < 0) {
        // Cache full — log and skip (rare with 32 slots / ~20 commands).
        spinlock_release(&g_cmd_dir_lock);
        serial_outsl("CMD_DIR: cache full, ignoring update");
        return;
    }

    // Overwrite slot in place. Inline storage: no kmalloc, no pointers to dangle.
    mem_copy((u8*)g_cache[slot].name, name.data, name.len);
    g_cache[slot].name_len = name.len;
    g_cache[slot].name[name.len] = 0;
    u32 tn = (target.len < 255) ? target.len : 255;
    mem_copy((u8*)g_cache[slot].target, target.data, tn);
    g_cache[slot].target_len = tn;
    g_cache[slot].target[tn] = 0;
    g_cache[slot].used = 1;
    g_cache[slot].src_drive = g_cmd_bin_dirs[bin_idx].drive_idx;
    g_cache[slot].src_inode = /* symlink inode from caller */ 0;
    spinlock_release(&g_cmd_dir_lock);
}

void cmd_dir_update_for_removed_link(u32 parent_inode_no,
                                    str_view_t name) {
    u8 bin_idx;
    if (!cmd_dir_is_bin_dir(parent_inode_no, &bin_idx)) return;

    spinlock_acquire(&g_cmd_dir_lock);
    for (u32 i = 0; i < CMD_DIR_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        if (g_cache[i].src_drive != g_cmd_bin_dirs[bin_idx].drive_idx) continue;
        str_view_t slot_name = str_view_from_parts(g_cache[i].name, g_cache[i].name_len);
        if (str_view_eq(slot_name, name) == false) continue;
        mem_set((u8*)&g_cache[i], 0, sizeof(cmd_dir_entry_t));
        break;
    }
    spinlock_release(&g_cmd_dir_lock);
}

bool cmd_dir_lookup_full(str_view_t name,
                         char *out_buf, u32 out_cap,
                         u8 *out_src_drive) {
    bool found = false;
    spinlock_acquire(&g_cmd_dir_lock);
    for (u32 i = 0; i < CMD_DIR_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        str_view_t slot_name = str_view_from_parts(g_cache[i].name, g_cache[i].name_len);
        if (str_view_eq(slot_name, name)) {
            u32 n = (g_cache[i].target_len < out_cap - 1) ? g_cache[i].target_len : out_cap - 1;
            mem_copy((u8*)out_buf, (const u8*)g_cache[i].target, n);
            out_buf[n] = 0;
            if (out_src_drive) *out_src_drive = g_cache[i].src_drive;
            found = true;
            break;
        }
    }
    spinlock_release(&g_cmd_dir_lock);
    if (found) return true;
    return cmd_dir_scan_fallback(name, out_buf, out_cap, out_src_drive);
}
```

This is the rough shape; production C will replace placeholders, hook
spinlock primitives to the existing kernel scheduler, and wire the
ext2_create_symlink_in_dir / ext2_unlink_in_dir call sites.

---

## 15. Why This Is the Right Shape

- **It is small.** A handful of helper functions in `kern_cmd_dir.c`,
  seed changes in `build.sh`, ~50 lines saved in `kern_tests.c` once
  fully refactored.
- **It is the *Unix* shape.** Symlinks-as-commands is the pattern
  Plan 9, Linux `alternatives`, Homebrew `brew link`, all use. We're
  not inventing anything.
- **It pays off debt.** `permissions2.md` deferred physical symlink
  resolution because "no symlinks on disk yet." This brings them in.
- **It composes with future work.** SFS, multi-drive overlays,
  per-drive command sets — all built on top of this.
- **It is always fresh.** Write-through is synchronous. No rehash, no
  PATH-refresh. The cache cannot lie about disk state.
- **It is reversible.** The boot-time seed symlinks can be replaced
  (one symlink per command) without breaking anything; the
  `handle_command` dispatch table is just C — typed command names
  with handler pointers, easy to read.
- **It is correctly concurrent.** Single global spinlock is held only
  for `O(strlen)` memcpy. Inline-storage guarantees no pointer ever
  dangles. Multicore future requires zero revisited design.
- **It enforces its own discipline.** Symlinks-only in `/bin/`; lookup
  is single-level flat; versions in names not dirs. Each rule is
  simple and verifiable.

---

## 16. Path Lengths

### Per-component hard limit (filesystem, immutable)

**255 bytes per directory component.** This is ext2's `name_len`
byte limit on a single segment between `/` separators. One component
cannot exceed 255 bytes (255 ASCII chars, or fewer Unicode chars in
multibyte encodings). This is the spec, not a sandfleaOS choice. We
don't fight it — and we don't recommend circumventing it with hashed
or shortened filenames. If a user's command identifier is 200+ chars,
they should rethink.

### Total path limit (kernel-wide)

**`PATH_MAX = 1024`** defined in `src/include/kern_limits.h`. Used at
the few static stack-buffer call sites that need a hard cap.

Why 1024 (not 256, not 4096):

- 1024 = ¼ page. Power of 2.
- 5× headroom over real-world worst-case paths (~200 chars).
- Won't pad a single stack frame to consume an entire 4 KB page.
- "You're passing absurd paths" surfaces as a real error rather than
  silently filling 3.85 KB of zeros per frame.
- Comfortable for descriptive kebab-case command names like
  `//A/bin/some-descriptive-tool-with-many-words → /wasm/...`.

### `cwd` becomes `str_t`

`char cwd[256]` → `str_t cwd`. The one mutable, kernel-owned path
buffer that grows on every `cd`:

| Op | `char cwd[256]` | `str_t cwd` |
|---|---|---|
| **Length** | `str_len(cwd)` — O(n) | `cwd.len` — O(1) field read |
| **Assign new path** | `memcpy` + null-term | `str_set(&cwd, view)` |
| **Pass to printf** | `printf("%s", cwd)` | `printf("%s", cwd.data)` (always null-term) |
| **Per-frame size** | 256 B (mostly zeros) | 16 B struct + heap bytes that fit |

We lose O(n) length operations, gain a single subtraction instruction,
gain 8 bytes per cached path. **Lateral at worst, strictly better for
length-sensitive code.** Same code complexity lines; cleaner invariant
(`cwd.data` is always null-terminated).

### Symlink filename enforcement (in `/bin/` dirs)

When creating / reading / removing a symlink **in a registered bin
directory** (`//A/bin/`, `//data/bin/`), the symlink's *name* (the
"command identifier") is bounded to **≤ 64 chars**:

```c
u32 name_len = str_view_len(name_view);
if (name_len > 64) {
    serial_outsf("CMD_DIR: bin symlink name too long (%u > 64): '%.*s'\n",
                 name_len, (int)name_view.len, name_view.data);
    return 0;  // E2BIG or EINVAL — explicit error, NOT silent truncation
}
```

We **throw an error, never silently truncate**. Reasons:

- Silent truncation hides user bugs ("my command works but doesn't
  match the file I created"). Surfaces as visible failure.
- 64 bytes is plenty for command identifiers. Beyond that, command
  names are unreadable anyway — the error surfaces them so the user
  renames.
- Length checks against `str_t.path` are essentially free (one
  subtraction + branch).

The 64-byte bin-name cap is enforced in:

- `ext2_create_symlink_in_dir(name, parent, target)` — at create time
- `ext2_unlink_in_dir(name, parent)` — at remove time (so we don't
  cache-stale an entry that was rejected earlier)
- `cmd_dir_register_bin` advisory log only

Symlinks outside `/bin/` (e.g., a user's `//A/scratch/foo →
/elsewhere.wasm`) are **not** bound this way — their full path is
bounded by `PATH_MAX = 1024` instead.

### Path buffers in the kernel — at-a-glance

| Buffer | Before | After |
|---|---|---|
| `cwd` global (`kern_ext2.h`) | `char cwd[256]` | `str_t cwd` |
| WASI `path_buf` × 2 (`wasm_spawn.c`) | `[256]` | `[1024]` |
| `kern_fs.c` local `path_buf[N]` | `[256]` | `[1024]` |
| `ext2_find_path` `name_buf[256]` | stays | stays (per-component, 255-byte filesystem bound) |
| cmd-dir cache `target[256]` | stays | stays (cmd-dir targets are <100 chars in practice) |
| Bin-symlink name length (in `/bin/`) | unbounded | ≤ 64 chars, error otherwise |

### Why this combination is right

- **Per-component (255)** is filesystem-grounded; we don't argue.
- **Cmd-dir targets (256)** are concrete, short, externally bounded.
  No reason to inflate.
- **WASI `path_buf` (1024)** covers arbitrary user input we don't
  control. 1024 is comfortable upper, wastes only ¾ of a page in
  the worst (rare) case.
- **`cwd` (`str_t`)** is the one place dynamic growth is the honest
  answer — it's the only kernel-owned long-lived path buffer.
- **Bin-symlink name (64)** is a UX-driven limit. Command identifiers
  longer than 64 chars are unreadable; the error surfaces them.

### Trade-offs we're rejecting

- **NTFS-style 32 KB**. We don't port Windows tools; no reason to
  mirror its limits.
- **4096 = full page**. Single-frame waste; lazy glibc-matching that
  doesn't fit sandfleaOS scale.
- **Unbounded dynamic (`str_t` everywhere)**. The diagnostic cost
  of one missing `kfree` per path op is non-zero. Reserve `str_t`
  for paths that actually grow (cwd); keep the rest as bounded
  static buffers.
- **No limit / log-only / silent truncation**. All are footguns.
  Throw the error or pass through.
