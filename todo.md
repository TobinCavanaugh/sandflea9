
Switch everything to UTF-8


### WASM
- WASM permissions 
- WASM modules / syscalls

### ELF
- Need to be able to execute ELFs, WASM is good for now

### Multicore
- MADT / ACPI parsing
- Multicore support / HW threads
- give each process an affinity, if multicore give core 0 kernel stuff, mux other threads to processes

### FS
- EXT2 Needs indirect inode support 
- AHCI, replace IDE
- Need fs agnostic file interface that uses handles (uring? async?)

### PCI
- Check ports implemented to speed up booting and be more sane

### IPC
- Needs research

### WM 
- Should probably be a wasm process, idk. If perf sucks, switch it to C native

### Permissions / hashig
https://github.com/BLAKE3-team/BLAKE3