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


u0 handle_command() {
    char workingbuf[256] = {0};
    u64 add = PAGE_SIZE * 1000;

    if (typingbuf[0] == '\0') return;

    cmd_word_t *word = cmd_parse(typingbuf, kmalloc);
    if (!word) return;

    serial_outsf("[[%s]]\n", word->loc);

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
        char *path = "/";
        if (word->next != null) {
            path = str_dup(word->next->loc, kmalloc);
            path[word->next->len] = 0;
        }

        u32 inode_no = 2;
        ext2_inode_t *start_inode = ext2_find_path(path, &inode_no);
        
        if (start_inode) {
            ext2_explorer_t exp;
            ext2_explorer_init(&exp, inode_no);
            ext2_explore_result_t res;
            
            while (ext2_explorer_next(&exp, &res)) {
                screen_push_linef("%*s|-- %s", (int)res.depth * 2, "", res.name);
            }
            kfree(start_inode);
        } else {
            screen_push_linef("lsr: Path not found: %s", path);
        }

        if (word->next != null) kfree(path);
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
