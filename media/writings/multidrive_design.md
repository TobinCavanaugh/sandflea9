# Multi-Drive Filesystem Design for sandfleaOS

## Motivation

Currently sandfleaOS boots from a single disk image that resets each QEMU run. We want:

1. **A volatile boot drive** — resets each run, easy to update/replace
2. **Persistent data drives** — files survive across QEMU sessions
3. **Clean path syntax** — no `/mnt` cruft, no `C:` drive letters

## Prior art

### Plan 9 — per-process namespaces
No global filesystem tree. Each process builds its own namespace by mounting
resources (disks, network, even the mouse) wherever it wants. **Union
directories** are the standout feature: mount multiple directories onto the
same path and reads search through all of them. Overlay your `/bin` on the
system `/bin` — your tools take priority, the rest falls through.

Requires a fundamentally different kernel architecture (everything is a 9P
server). Too radical for sandfleaOS, but the union directory concept is worth
stealing long-term.

### BeOS / Haiku — volume-rooted at `/`
Every volume is a root-level directory: `/boot`, `/data`, `/projects`. No
drive letters, no `/mnt`. The `/boot` volume is special (kernel lives there),
all others are peers. This is the closest to the sandfleaOS model.

### AmigaOS — `VolumeName:` prefix
Every path starts with a volume name and colon: `Workbench:Utilities/MultiView`.
If a volume isn't inserted, the OS prompts "Please insert volume Workbench."
The `:` acts as an unambiguous namespace boundary.

### RISC OS — `FS::Volume.$` prefix
Even more explicit: `SDFS::HardDisk.$.Documents.Letter`. The filing system
type, volume name, and root marker (`$`) are all in the path.

### Windows — `C:\`, `D:\`
Single-letter drive roots. Simple but limited to 26 drives, and the `:` has
no semantic meaning beyond "this is a drive letter." The `/D` flag on `cd`
is an artifact of per-drive working directories.

### Linux — `/mnt/foo`
Clean but adds a meaningless intermediate directory. The mount point's name
and location are unrelated to what's actually on the disk.

## The sandfleaOS model

```
//             ← synthetic root, lists all drives (virtual directory)
//A/           ← boot drive (volatile, resets each run)
//data/        ← persistent drive 1 (named from ext2 volume label)
//projects/    ← persistent drive 2
```

- `//A` is always present and volatile
- Other drives are named by their ext2 volume label
- `//` is a virtual directory — creating files directly is an error
- Path syntax: `//A/wasm/cat.wasm`, `//data/documents/notes.txt`
- Bare paths (`/proj/sandflea`) resolve on the current drive
- `ls //` shows all available drives
- No `mount` command needed — drives auto-appear at boot

## Drive separator syntax options

The question: how to write "file X on drive Y" in a single expression?
We have a few syntactical choices. Each is shown with examples and tradeoffs.

### Option 1: Slash-rooted (`/drive/path`)

```
cd /data/projects/sandflea
ls /A/wasm/
cat /documents/notes.txt
```

**How it works:** The first path component after `/` is the drive name. The
kernel's `ext2_find_path()` splits on the first `/`, looks up the drive,
then resolves the rest on that drive's ext2.

**Pros:**
- Familiar — looks like any other Unix path
- No new syntax to learn
- Tab-completion at `/` shows all drives
- `cd`, `ls`, `cat` all work identically

**Cons:**
- Ambiguous if a drive has a subdirectory with the same name as another
  drive (e.g., `/data/data/file` — is the second `data` a drive or a
  subdirectory? Not a problem since we disallow nesting drives, but worth
  noting)

### Option 2: Colon prefix (`Drive:path`)

```
cd data:projects/sandflea
ls A:wasm/
cat documents:notes.txt
```

**How it works:** Colon separates the drive namespace from the path within
that drive. Parsed as `<drive> ':' <path>`.

**Pros:**
- Unambiguous — the colon is an explicit boundary
- AmigaOS heritage — proven design
- Short to type (one extra character)
- `:` can't appear in ext2 filenames, so zero ambiguity

**Cons:**
- Collides with `var:type = val` in your SFS language design. In the language,
  `:` means "has type" (type annotation). In the filesystem, `:` means "on
  drive." These are different concepts using the same glyph. A user might
  read `data:projects` as "data, which is a projects" rather than "projects
  on the data drive."

### Option 3: At-sign prefix (`@drive/path`)

```
cd @data/projects/sandflea
ls @A/wasm/
cat @documents/notes.txt
```

**How it works:** `@` marks the drive name. `@data` = "locate drive named data."

**Pros:**
- No collision with SFS syntax
- `@` has a connotation of "at" / "located at" — semantically clean
- Also works as a standalone command: `cd @data` = "go to root of data drive"
- No ambiguity with filenames

**Cons:**
- `@` feels "internet-y" rather than filesystem-y
- Shift+2 on US keyboard (minor)

### Option 4: Colon suffix (`/drive:/path`)

```
cd /data:/projects/sandflea
ls /A:/wasm/
cat /documents:/notes.txt
```

**How it works:** Slash-rooted like `/drive`, but the colon explicitly marks
the drive boundary. `/data:` means "drive named data."

**Pros:**
- Keeps the `/` prefix convention ("this is an absolute path")
- Colon marks the namespace boundary explicitly
- `ls /` shows drives, `ls /data:` shows drive contents

**Cons:**
- Verbose — two extra characters vs Option 1
- Still collides with SFS `:` semantics
- Reads awkwardly: `/data:/projects` has two separators

### Option 5: Pipe prefix (`|drive/path`)

```
cd |data/projects/sandflea
ls |A/wasm/
```

**Cons:** `|` is universally "pipe" in shells. Total collision.

### Option 6: Double slash (`//drive/path`)

```
cd //data/projects/sandflea
ls //A/wasm/
cat //documents/notes.txt
```

**How it works:** `//` marks a drive root boundary. What follows is the drive
name, then the path within that drive. Bare paths (no `//`) resolve on the
current drive: `//A/proj/` crosses to drive A, `/proj/` stays local.

**Pros:**
- No glyph collision with SFS syntax
- `/` is already the path separator — `//` extends naturally: "root of a
  different tree"
- Bare paths stay clean for the common case (current drive)
- `cd //A` = "go to root of A drive", `cd /` = "go to root of current drive"

---

## Recommendation

**Option 6 (`//drive/path`)** — chosen for sandfleaOS:

- `//A/proj/` — cross to drive A, path `/proj/`
- `/proj/` — path on the current drive (no prefix needed)
- `cd //A` — switch to drive A at its root
- `ls //` — list all available drives

This keeps the common case (same-drive paths) clean while making drive
crossings explicit. No glyph collision with SFS, no new separator character
to learn.

## Union directories (future)

A longer-term goal inspired by Plan 9. A drive could declare that its
`/bin` overlays the boot drive's `/bin`. When a WASM process looks up
`/bin/cat`, the kernel searches `/data/bin` first, then falls through
to `/A/bin`. This lets users install programs on persistent drives that
shadow system defaults — no symlinks, no PATH manipulation, no package
manager.

Syntax sketch:
```
#> union /data/bin:/A/bin
```

But this is for another design doc. The multi-drive model comes first.

## Implementation plan

1. **Block cache** — 32-entry LRU at the `ext2_read_block` layer (~32KB)
2. **IDE slave support** — `ide_read_sectors()` takes a drive selector
3. **Drive registry** — `drive_t` array with name, IDE selector, ext2 superblock
4. **Path resolution** — `//` prefix routes to correct drive, bare `/` stays local
5. **Synthetic root** — `ls //` assembles virtual directory entries from
   `drives[0..n]`
6. **Persistent image** — QEMU `-drive file=data.img,if=ide,index=1`
