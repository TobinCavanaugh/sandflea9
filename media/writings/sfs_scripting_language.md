# Sandflea Scripting Language (SFS) - Specification

SFS is a Wasm-native, table-oriented scripting language designed for sandfleaOS. It moves away from the unstructured text-stream paradigm of POSIX (stdin/stdout) in favor of **Reactive Table Pipelines** and **Structured Metadata**.

## 1. Core Philosophy
- **Wasm-First:** Every command is an invocation of a WebAssembly module.
- **Table-Oriented:** Programs exchange structured data chunks via kernel-managed table handles.
- **Strict Syntax:** Minimal exceptions, clear token sigils, and mandatory structure.
- **Reactive Dataflow:** Programs act as state machines reacting to chunks of data, allowing for streaming, memory efficiency, and "game-loop" execution models.

---

## 2. Syntax & Lexing

### 2.1 Sigils
SFS uses a strict sigil system to ensure zero ambiguity between variables, modifiers, and positional arguments.

- **`$var`**: **Variables.** Shell-resident state (e.g., `$path`, `$limit`).
- **`@opt=val`**: **Modifiers.** Named parameters passed at program initialization. 
  - `@recursive` is shorthand for `@recursive=true`.
  - `@lim=10` is an atomic key-value pair.
  - `@lim=$max` injects a variable into a modifier.
- **`command`**: **Wasm Modules.** The name of the module to execute (e.g., `ls`, `grep`).
- **`ident.method`**: **Direct Export Calls.** Invokes a specific Wasm export instead of the default entry point.
- **`:`**: **System/Permission Separator.** Reserved for user:group and capability descriptors (e.g., `root:admin`).

### 2.2 Literals & Quotes
- **Double Quotes (`"..."`)**: Interpolated strings. Resolves `$variables` and escape codes.
- **Single Quotes (`'...'`)**: Raw strings. "What you see is what you get."
- **Raw Prefix (`r"..."`)**: Alternative raw syntax for strings containing single quotes.
- **Numbers**: Standard integer and float literals. Negative numbers (e.g., `-5`) are tokens, distinct from modifiers.

---

## 3. The Table Execution Model (TEM)

Instead of byte streams, SFS programs communicate via **Chunked Table Pipelines**.

### 3.1 Reactive Entry Points
A Sandflea program is a state machine with three primary Wasm exports:
1.  **`init(pos_args_handle, mods_handle)`**: Called once. Programs allocate buffers and configure state.
2.  **`on_chunk(channel_name, chunk_handle)`**: Triggered when the upstream yields data. Data is processed in "chunks" to maintain a low memory footprint.
3.  **`on_complete()`**: Triggered when the upstream has finished its stream. Programs yield final results here (e.g., a `sum` or `sort`).

### 3.2 Table Structure
- **Indices are Truth:** Columns are accessed by index (0, 1, 2) for performance and strictness.
- **Name Hints:** Column names are provided as metadata hints for user display and "shaper" programs (like `select`).
- **Multi-Output (Multiplexing):** Programs can yield chunks to different named channels (e.g., `@stdout`, `@stderr`, `@data`).

---

## 4. Pipeline Operations

### 4.1 The Pipe Operator (`|`)
The pipe operator passes a **Table Handle** from one program's output to another's input.

### 4.2 Shapers & Selectors
Because pipelines are structured, SFS includes native "shaper" logic for remapping data before it hits the next program.

```
// Basic pipeline with modifiers
ls / @recursive @lim=10 | print

// Selecting specific columns by hint
ls | select "size" "name" | sum

// Manual column remapping (swapping index 0 and 1)
ls | @[1, 0] | print
```

---

## 5. Execution Flow
1. **Compilation:** The shell parses the command into an AST and performs **Schema Verification**.
2. **Schema Verification:** The shell queries the Wasm modules' exported schemas. It ensures that the output table of Program A matches the expected input schema of Program B *before* execution begins.
3. **Initialization:** The kernel calls `init()` on all modules in the pipe.
4. **Signaling:** Programs call `shell_initialized()` when they are ready to receive data.
5. **Dataflow:** Chunks flow through the reactive `on_chunk` callbacks.
6. **Completion:** The pipeline collapses as `on_complete` propagates.

---

## 6. Example Scripts

```
let $threshold = 1048576 // 1MB
ls / @recursive 
  | filter @col="size" @op=">" @val=$threshold 
  | sort "size" 
  | @[0, 1] // Keep only name and size
  | print
```
