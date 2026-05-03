# Linking Kernel Functions to WebAssembly

In sandfleaOS, we use the Wasm3 engine. Unlike a traditional shared library where names are resolved automatically, Wasm3 requires us to manually "link" or "bind" C functions to the imports defined in your WebAssembly module.

## 1. How to Tie Functions to WASM

You don't just name the C function and expect it to work. You must explicitly register it using `m3_LinkRawFunction`.

### The Linker Call
The general pattern in your kernel's WASM loader (e.g., `kern_tests.c`) would be:

```c
// 1. Load the module into the runtime first!
result = m3_LoadModule(runtime, module);

// 2. Then link your host functions
m3_LinkRawFunction(module, "env", "sys_open", "i(i)", &wasm_fs_open);
```

*   **Linking Order:** You must call `m3_LoadModule` before `m3_LinkRawFunction` in most Wasm3 configurations to ensure the module is associated with a runtime.

*   **`module`**: The `IM3Module` you just parsed.
*   **`"env"`**: The module name used in the WASM `(import "env" ...)` statement.
*   **`"sys_open"`**: The function name used in the WASM import.
*   **`"i(i)"`**: The **Signature**. This tells Wasm3 the types of arguments and the return value.
    *   `i` = 32-bit integer
    *   `I` = 64-bit integer
    *   `f` = 32-bit float
    *   `F` = 64-bit float
    *   `v` = void
    *   Example: `"i(ii)"` means a function that takes two `i32` and returns an `i32`.
*   **`&wasm_fs_open`**: The actual C function in your kernel.

### The C Wrapper Signature
Host functions in Wasm3 have a specific signature. They don't take arguments directly; they take a "raw call" context.

```c
m3ApiRawFunction(wasm_fs_open) {
    m3ApiReturnType (i32)    // Define the return type
    m3ApiGetArg     (u32, path_offset) // Get arguments from WASM stack

    // Get the base of the WASM memory
    u8* mem = m3_GetMemory(runtime, NULL, 0);
    const char* path = (const char*)(mem + path_offset);

    // Call the actual kernel VFS
    i32 fd = fs_open(path);

    m3ApiReturn(fd); // Push the result back to WASM
}
```

## 2. Capability-Based Security (Restricting Functions)

"How do I allow certain functions for certain programs?"

Because you are manually linking functions for **every module you load**, you have total control over what each program can see. This is often called **Capability-Based Security**.

### Method A: The Manifest Approach
You can define a "permission level" for your WASM programs.

```c
void link_syscalls(IM3Module module, int permission_level) {
    // Every program gets basic math/logging
    m3_LinkRawFunction(module, "env", "log", "v(i)", &wasm_log);

    // Only "Level 2" programs get file access
    if (permission_level >= 2) {
        m3_LinkRawFunction(module, "env", "sys_open", "i(i)", &wasm_fs_open);
        m3_LinkRawFunction(module, "env", "sys_write", "i(iii)", &wasm_fs_write);
    }

    // Only "Admin" programs get hardware access
    if (permission_level == PERM_ADMIN) {
        m3_LinkRawFunction(module, "env", "pci_read", "i(i)", &wasm_pci_read);
    }
}
```

### Method B: Selective Imports
If a WASM module tries to import a function that you choose **not** to link, Wasm3 will return a "function not found" error during the `m3_LoadModule` or link phase, and the program will fail to start. This is a very secure way to "sandbox" applications.

## Summary
1.  **Use `m3_LinkRawFunction`** to bridge the gap.
2.  **Define a signature string** like `"i(ii)"`.
3.  **Use `m3ApiRawFunction` macros** for your C implementation.
4.  **Implement a check** in your loader to only call `m3_LinkRawFunction` for authorized features.
