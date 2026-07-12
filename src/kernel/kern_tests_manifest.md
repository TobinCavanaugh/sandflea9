# kern_tests.c Function Manifest

This table lists the functions defined in `kern_tests.c`, providing a high-level overview of their purpose and how their arguments are semantically interpreted.

| Function | Description |
| :--- | :--- |
| `test_lsr(path)` | Recursively lists the contents of the filesystem starting at the specified path. |
| `test_ext2(path)` | Opens a file and streams its raw content to the terminal. |
| `dotest(iterations)` | A multi-threaded test that prints heartbeat messages and performs stress-test memory allocations. |
| `wasm_fd_open(path_offset)` | Host function for Wasm modules to open files via the kernel's filesystem layer. |
| `wasm_fd_close(fd)` | Host function for Wasm modules to close an active file descriptor. |
| `wasm_fd_read(fd, buffer, count)` | Host function for Wasm modules to read data from a file descriptor into their internal memory. |
| `wasm_fd_write(fd, iovs, iovs_len, nwritten)` | Host function for Wasm modules to write data to the terminal using WASI-style vector I/O. |
| `wasm_test(wasm_path)` | Orchestrates the Wasm lifecycle: loading from disk, initializing the runtime, linking host functions, and executing `_start`. |
| `handle_command()` | The primary entry point for the kernel's interactive shell; parses and executes terminal commands. |
