### Floating point in Interrupt Handlers

Don't do FPU stuff in interrupt handlers, as it will clobber the sse registers.
If you really, really want to, then do something like this:

```C
void safe_float_print(double val) {
    char buf[512]; //<-- NEEDS TO BE STACK ALLOCATED, NOT GLOBAL
    asm volatile("fxsave %0" : : "m"(buf)); // Save current state
    
    // Do float stuff here

    asm volatile("fxrstor %0" : : "m"(buf)); // Restore state
}
```