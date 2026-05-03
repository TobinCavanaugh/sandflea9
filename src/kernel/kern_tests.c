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

// TODO Ditch arith64
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

u0 wasm_test(u0 *arg) {
    const char* wasm_path = (const char*)arg;
    if (!wasm_path) wasm_path = "test.wasm";

    serial_outsf("WASM: Loading %s\n", wasm_path);
    i32 fd = fs_open(wasm_path);
    if (fd < 0) {
        screen_push_linef("WASM: Could not open %s", wasm_path);
        return;
    }

    u32 size = fs_size(fd);
    u8* wasm_data = kmalloc(size);
    if (!wasm_data) {
        screen_push_line("WASM: Out of memory for WASM data");
        fs_close(fd);
        return;
    }
    fs_read(fd, wasm_data, size);
    fs_close(fd);

    serial_outsl("WASM: Initializing environment...");
    serial_outsf("m3_NewEnvironment: %p\n", m3_NewEnvironment);
    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        screen_push_line("WASM: Could not create environment");
        kfree(wasm_data);
        return;
    }

    serial_outsl("WASM: Initializing runtime...");
    serial_outsf("m3_NewRuntime: %p\n", m3_NewRuntime);
    IM3Runtime runtime = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!runtime) {
        screen_push_line("WASM: Could not create runtime");
        m3_FreeEnvironment(env);
        kfree(wasm_data);
        return;
    }

    IM3Module module = NULL;
    serial_outsl("WASM: Parsing module...");
    serial_outsf("m3_ParseModule: %p\n", m3_ParseModule);
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

    serial_outsl("WASM: Linking LibC...");
    serial_outsf("m3_LinkLibC: %p\n", m3_LinkLibC);
    result = m3_LinkLibC(module);
    if (result) {
        screen_push_linef("WASM: Link error: %s", result);
        goto Label_Done;
    }


    serial_outsf("m3_FindFunction: %p\n", m3_FindFunction);
    IM3Function f;
    result = m3_FindFunction(&f, runtime, "add");
    if (result) {
        screen_push_linef("WASM: Function error: %s", result);
        goto Label_Done;
    }

    const char* args[] = { "10", "20", NULL };
    result = m3_CallArgv(f, 2, args);
    if (result) {
        screen_push_linef("WASM: Call error: %s", result);
    } else {
        i32 res = 0;
        m3_GetResultsV(f, &res);
        screen_push_linef("WASM: add(10, 20) = %d", res);
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


    if (str_eql(word->loc, "wasm", word->len)) {
        char *path = null;
        if (word->next != null) {
            path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        }
        sched_create_thread(wasm_test, path);
        goto Label_Free;
    }


    if (str_eql(word->loc, "do", word->len)) {
        i64 val = 5;
        if (word->next && word->next->val_type == CMD_WT_i64) {
            val = word->next->val_i64;
        }

        kern_process_t *new_proc = process_create();
        sched_create_process_thread(new_proc, dotest, (u0 *) val);
        goto Label_Free;
    }


    if (str_eql(word->loc, "kmalloc", word->len)) {
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


    if (str_eql(word->loc, "cls", word->len)) {
        typingbuf[0] = 0;
        screen_terminal_clear();
        goto Label_Free;
    }

    if (str_eql(word->loc, "lsr", word->len)) {
        char *path = null;
        if (word->next != null) {
            path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        }
        sched_create_thread((u0 (*)(u0 *)) lsrtest, path);
//        lsrtest(); // works fine
        goto Label_Free;
    }


    if (str_eql(word->loc, "open", word->len) && word->next != null) {
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

    if (str_eql(word->loc, "ext2", word->len) && word->next != null) {
        char *path = str_dup(word->next->loc, kmalloc);
        path[word->next->len] = 0;

        u32 inode_no = 0;
        ext2_inode_t *i = ext2_find_path(path, &inode_no);
        if (i != null) {
            screen_push_linef("File Size: %dB", i->size);
            if ((i->mode & 0xF000) == 0x8000) {
                u8 *content = get_block_ptr(i->block[0]);
                if (content) {
                    u32 print_len = (i->size < 512) ? i->size : 511;
                    char *safe_content = kmallocz(print_len + 1);
                    mem_copy(safe_content, content, print_len);
                    screen_push_line(safe_content);
                    serial_outsf("vvv\n%s\n^^^", safe_content);
                    kfree(safe_content);
                    kfree(content);
                }
            } else {
                screen_push_line("Target is not a regular file");
            }
            kfree(i);
        } else {
            screen_push_linef("Path not found: %s", path);
        }

        kfree(path);
        goto Label_Free;
    }

    if (str_eql(word->loc, "cat", word->len) && word->next != null) {
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

    if (str_eql(word->loc, "kmalloc2", word->len)) {
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


    if (str_eql(word->loc, "pci", word->len)) {
        ssfn_dst.y = font_height * 2;
        ssfn_dst.x = 0;
        pci_device_t *dev = system.pci_list_head;
        while (dev) {
            stbsp_snprintf(workingbuf, 255, "C:%X S:%X | V:%X D:%X\n",
                           dev->class_code, dev->subclass,
                           dev->vendor_id, dev->device_id);
            ssfn_puts(workingbuf);
            dev = dev->next;
        }
        ssfn_puts("Enter any key to continue\n");
        screen_draw();
        while (!keyboard_eat_key()) { asm volatile("hlt"); }
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
