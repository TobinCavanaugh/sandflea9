#include "../include/kern_tests.h"
#include "../include/kern_mem.h"
#include "../include/kern_terminal.h"
#include "../include/kern_serial.h"
#include "../include/util_cmd.h"
#include "../util/util_str.h"
#include "../include/stbsupport.h"
#include "../include/kern_ext2.h"
#include "../include/ssfn.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_vmm.h"
#include "../include/kern_fs.h"
#include "../include/kern_sched.h"
#include "wasm3-0.5.0/source/wasm3.h"
#include "wasm3-0.5.0/source/m3_api_libc.h"

// TODO Running kmalloc2 and then kmalloc causes early page fault? I think

cmd_word_t *word;

u0 lsrtest(char *path_arg) {
    char *path = path_arg ? path_arg : "/";

    u32 inode_no = 2;
    ext2_inode_t *start_inode = ext2_find_path(path, &inode_no);

    if (start_inode) {
        ext2_explorer_t exp;
        ext2_explorer_init(&exp, inode_no);
        ext2_explore_result_t res;

        while (ext2_explorer_next(&exp, &res)) {
            screen_push_linef("%*s|-- %s", (int) res.depth * 2, "", res.name);
        }
        ext2_explorer_deinit(&exp);
        kfree(start_inode);
    } else {
        screen_push_linef("lsr: Path not found: %s", path);
    }

    if (path_arg) kfree(path_arg);
}

extern u0 delay(u64 ms);

u0 dotest(u0 *arg) {
    kern_process_t *proc = sched_get_current_process();
    kern_task_t *task = sched_get_current_task();

    i64 iterations = (i64) arg;
    if (iterations <= 0) iterations = 5;

    for (int i = 0; i < iterations; i++) {
        serial_outsf("[PID %d | TID %d] Heartbeat %d\n", proc->pid, task->tid, i);
        screen_push_linef("[PID %d | TID %d] Heartbeat %d", proc->pid, task->tid, i);
        delay(1000);
        pmalloc(4096 * 1024);
    }

    serial_outsf("[PID %d | TID %d] Exiting\n", proc->pid, task->tid);
    screen_push_linef("[PID %d | TID %d] Exiting", proc->pid, task->tid);
}

m3ApiRawFunction(wasm_fs_open) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (u32, path_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);

    if (mem && path_offset < memory_size) {
        const char *path = (const char *) (mem + path_offset);
        screen_push_linef("wasm open: %s", path);
        serial_outsf("WASM: Open called for path: %s\n", path);

        i32 fd = fs_open(path);
        m3ApiReturn(fd);
    } else {
        screen_push_line("WASM: Invalid memory access in sys_open");
        m3ApiReturn(-1);
    }
}

m3ApiRawFunction(wasm_fs_close) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)

    screen_push_linef("wasm close: %d", fd);
    serial_outsf("wasm close: %d\n", fd);
    i32 result = fs_close(fd);
    m3ApiReturn(result);
}

m3ApiRawFunction(wasm_fs_read) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (i32, fd)
    m3ApiGetArg     (u32, buf_offset)
    m3ApiGetArg     (u32, count)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);

    if (mem && buf_offset + count <= memory_size) {
        u8 *kernel_buf = mem + buf_offset;
        i32 bytes_read = fs_read(fd, kernel_buf, count);
        m3ApiReturn(bytes_read);
    } else {
        m3ApiReturn(-1);
    }
}

u0 wasm_test(u0 *arg) {
    const char *wasm_path = (const char *) arg;
    if (!wasm_path) wasm_path = "add_test.wasm";

    IM3Environment env = null;
    IM3Runtime runtime = null;
    u8 *wasm_data = null;

    serial_outsf("WASM: Loading %s\n", wasm_path);
    i32 fd = fs_open(wasm_path);
    if (fd < 0) {
        screen_push_linef("WASM: Could not open %s", wasm_path);
        goto Label_Done;
    }

    u32 size = fs_size(fd);
    wasm_data = kmalloc(size);
    if (!wasm_data) {
        screen_push_line("WASM: Out of memory for WASM data");
        fs_close(fd);
        goto Label_Done;
    }
    fs_read(fd, wasm_data, size);
    fs_close(fd);

    serial_outsl("WASM: Initializing environment...");
    env = m3_NewEnvironment();
    if (!env) {
        screen_push_line("WASM: Could not create environment");
        goto Label_Done;
    }

    serial_outsl("WASM: Initializing runtime...");
    runtime = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!runtime) {
        screen_push_line("WASM: Could not create runtime");
        goto Label_Done;
    }


    IM3Module module = NULL;
    serial_outsl("WASM: Parsing module...");
    M3Result result = m3_ParseModule(env, &module, wasm_data, size);
    if (result) {
        screen_push_linef("WASM: Parse error: %s", result);
        goto Label_Done;
    }

    if (!module) {
        screen_push_line("WASM: Parsing failed, module is NULL");
        goto Label_Done;
    }

    serial_outsf("m3_LoadModule: %p\n", m3_LoadModule);
    result = m3_LoadModule(runtime, module);
    if (result) {
        screen_push_linef("WASM: Load error: %s", result);
        goto Label_Done;
    }

    m3_LinkRawFunction(module, "env", "sys_open", "i(i)", &wasm_fs_open);
    m3_LinkRawFunction(module, "env", "sys_read", "i(iii)", &wasm_fs_read);
    m3_LinkRawFunction(module, "env", "sys_close", "i(i)", &wasm_fs_close);

    serial_outsl("WASM: Linking LibC...");
    result = m3_LinkLibC(module);
    if (result) {
        screen_push_linef("WASM: Link error: %s", result);
        goto Label_Done;
    }

    IM3Function f;
    result = m3_FindFunction(&f, runtime, "entry");
    if (result) {
        screen_push_linef("WASM: Function error: %s", result);
        goto Label_Done;
    }

    serial_outsl("WASM: Calling entry()...");
    result = m3_CallArgv(f, 0, NULL);
    if (result) {
        screen_push_linef("WASM: Call error: %s", result);
    } else {
        i32 res = 0;
        m3_GetResultsV(f, &res);
        screen_push_linef("WASM: entry() returned %d", res);
        serial_outsf("WASM: entry() returned %d\n", res);
    }

    Label_Done:
    if (runtime) m3_FreeRuntime(runtime);
    if (env) m3_FreeEnvironment(env);
    if (wasm_data) kfree(wasm_data);
    if (arg) kfree(arg);
}

u0 handle_command() {
    char workingbuf[256] = {0};
    u64 add = PAGE_SIZE * 1000;

    if (typingbuf[0] == '\0') return;

    word = cmd_parse(typingbuf, kmalloc);
    if (!word) return;

    serial_outsf("[[%s]]\n", word->loc);


    if (str_eql(word->loc, "proc", word->len)) {
        kern_task_t *head = sched_get_task_list_head();
        if (!head) {
            screen_push_line("No tasks found.");
            goto Label_Free;
        }

        screen_push_line("PROCESSES:");
        screen_push_line("PID  CR3               HEAP_VPTR     MEM (KB)  THREADS");
        screen_push_line("---  ---               ---------     ---       -------");

        kern_task_t *curr = head;
        kern_process_t *visited[64] = {0};
        int visited_count = 0;

        do {
            kern_process_t *proc = curr->process;
            if (proc) {
                bool already_visited = false;
                for (int i = 0; i < visited_count; i++) {
                    if (visited[i] == proc) {
                        already_visited = true;
                        break;
                    }
                }

                if (!already_visited && visited_count < 64) {
                    visited[visited_count++] = proc;

                    int thread_count = 0;
                    kern_task_t *t_curr = head;
                    do {
                        if (t_curr->process == proc) thread_count++;
                        t_curr = t_curr->next;
                    } while (t_curr != head);

                    u64 total_mem = 0;
                    kern_mem_region_t *region = proc->mem_regions;
                    while (region) {
                        total_mem += region->page_count * PAGE_SIZE;
                        region = region->next;
                    }

                    screen_push_linef("%-3d  %016llx  %012llx  %-8lld  %d",
                                      proc->pid, proc->cr3, proc->heap_vptr, total_mem / 1024, thread_count);
                }
            }
            curr = curr->next;
        } while (curr != head);

        screen_push_line("");

        screen_push_line("THREADS:");
        screen_push_line("TID  PID  STATE    RSP");
        screen_push_line("---  ---  -----    ---");

        curr = head;
        do {
            const char *state_str = "UNKNOWN";
            switch (curr->state) {
                case TASK_STATE_READY:
                    state_str = "READY";
                    break;
                case TASK_STATE_RUNNING:
                    state_str = "RUNNING";
                    break;
                case TASK_STATE_BLOCKED:
                    state_str = "BLOCKED";
                    break;
                case TASK_STATE_DEAD:
                    state_str = "DEAD";
                    break;
            }

            screen_push_linef("%-3d  %-3d  %-7s  %016llx",
                              curr->tid, curr->process ? curr->process->pid : -1, state_str, curr->rsp);

            curr = curr->next;
        } while (curr != head);

        goto Label_Free;
    }

    if (str_eqlb(word->loc, "wasm")) {
        char *path = null;
        if (word->next != null) {
            path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        }

        kern_process_t *proc = process_create();
        sched_create_process_thread(proc, wasm_test, path);
        goto Label_Free;
    }


    if (str_eqlb(word->loc, "do")) {
        i64 val = 5;
        if (word->next && word->next->val_type == CMD_WT_i64) {
            val = word->next->val_i64;
        }

        kern_process_t *new_proc = process_create();
        sched_create_process_thread(new_proc, dotest, (u0 *) val);
        goto Label_Free;
    }


    if (str_eqlb(word->loc, "kmalloc")) {
        u64 size = 0;
        serial_outs("Testing Kmalloc and page fault handling.\n");
        i64 inc = 0;
        while (1) {
            size += add;
            kmalloc(add);
            stbsp_snprintf(workingbuf, 255, "%lldB\n", size);
            serial_outsf("%lldB\n", size);
            if (inc % 2 == 0) {
                screen_puts_r(workingbuf, V2I(0, font_height * 2), COLOR_WHITE, COLOR_BLACK);
                screen_draw();
            }
            ++inc;
        }
        goto Label_Free;
    }


    if (str_eqlb(word->loc, "cls")) {
        typingbuf[0] = 0;
        screen_terminal_clear();
        goto Label_Free;
    }

    if (str_eqlb(word->loc, "lsr")) {
        char *path = null;
        if (word->next != null) {
            path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        }
        sched_create_thread((u0 (*)(u0 *)) lsrtest, path);
//        lsrtest(); // works fine
        goto Label_Free;
    }


    if (str_eqlb(word->loc, "open") && word->next != null) {
        char *path = str_dup(word->next->loc, kmalloc);
        path[word->next->len] = 0;

        i32 w = fs_open(path);

        if (w < 0) {
            screen_push_linef("Failed to open file at `%s`", path);
            goto Label_Free;
        }

        fs_seek(w, 0, SEEK_END);
        i32 len = fs_tell(w);
        fs_seek(w, 0, SEEK_SET);


        char *dat = kmalloc(len + 1);
        fs_read(w, dat, len);
        dat[len] = '\0';
        fs_close(w);

        screen_push_line(dat);

        kfree(dat);

        goto Label_Free;
    }

    if (str_eqlb(word->loc, "ext2") && word->next != null) {
        char *path = str_dup(word->next->loc, kmalloc);
        path[word->next->len] = 0;

        i32 i = fs_open(path);

        const i32 bufSize = 4096;
        char *buf = kmallocz(bufSize + 1);

        if (i >= 0) {
            while (true) {
                i32 amt = fs_read(i, buf, bufSize);
                if (amt <= 0) break;
                buf[amt] = 0;
                screen_push_line(buf);
            }
        }

        fs_close(i);

        kfree(buf);
        kfree(path);
        goto Label_Free;
    }

    if (str_eqlb(word->loc, "cat") && word->next != null) {
        char *path = str_dup(word->next->loc, kmalloc);
        path[word->next->len] = 0;

        i32 fd = fs_open(path);
        if (fd >= 0) {
            u32 size = fs_size(fd);
            screen_push_linef("Reading %s (%d bytes) via FD %d", path, size, fd);

            u8 *buf = kmallocz(size + 1);
            i32 read = fs_read(fd, buf, size);
            if (read >= 0) {
                screen_push_line((char *) buf);
                serial_outsf("CAT: %s\n", (char *) buf);
            } else {
                screen_push_line("Error reading file");
            }
            kfree(buf);
            fs_close(fd);
        } else {
            screen_push_linef("Could not open: %s", path);
        }

        kfree(path);
        goto Label_Free;
    }

    if (str_eqlb(word->loc, "kmalloc2")) {
        u64 sum = 0;
        serial_outs("Testing Kmalloc and freeing.\n");
        while (sum < system.total_mem_size * 4) {
            sum += add;
            void *dat = kmalloc(add);
            if (!dat) break;
            mem_set(dat, COLOR_MAGENTA, add);
            kfree(dat);
            stbsp_snprintf(workingbuf, 255, "%lld\n", sum);
            serial_outs(workingbuf);
        }
        goto Label_Free;
    }


    if (str_eqlb(word->loc, "pci")) {
        pci_device_t *dev = system.pci_list_head;
        while (dev) {
            screen_push_linef("C:%X S:%X | V:%X D:%X\n",
                              dev->class_code, dev->subclass,
                              dev->vendor_id, dev->device_id);
            dev = dev->next;
        }
        goto Label_Free;
    }

    Label_Fail:
    {
        const char *failure = "Failure to parse command";
        screen_push_line(failure);
        serial_outsf("%s: %s\n", failure, word->loc);
    }

    Label_Free:
    cmd_parse_free(word, kfree);
}
