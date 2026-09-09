
Switch everything to UTF-8


### WASM
- WASM permissions 
- WASM modules / syscalls
- General API
- Module interaction

### ELF
- Need to be able to execute ELFs, WASM is good for now

### ACPI
- Power button

### Multicore
- MADT / ACPI parsing
- Multicore support / HW threads
- give each process an affinity, if multicore give core 0 kernel stuff, mux other threads to processes

### FS
- AHCI, replace IDE
- Need fs agnostic file interface that uses handles (uring? async?)

### PCI
- Check ports implemented to speed up booting and be more sane

### IPC
- Needs research

### WM 
- proper infinite canvas
- Drawing API
- Win + scroll / win + alt scroll
- win alt arrows to move across workspaces

### Permissions / hashing
https://github.com/BLAKE3-team/BLAKE3


integer overflow in wasm quake