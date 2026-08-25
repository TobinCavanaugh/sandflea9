// kern_ipc.c — N:N reference-counted shared memory and handle table IPC.

#include "../include/kern_ipc.h"
#include "../include/kern_vmm.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"
#include "../include/kern_asmstubs.h"
#include "wasm3-0.5.0/source/m3_env.h"

// ── Global Shared Memory Registry ───────────────────────────────────────────
static kern_shmem_t *g_shmem_head  = NULL;
static u32           g_next_shm_id = 1;

static kern_shmem_t *shmem_find_global(u32 shm_id) {
    kern_shmem_t *cur = g_shmem_head;
    while (cur) {
        if (cur->shm_id == shm_id) return cur;
        cur = cur->next;
    }
    return NULL;
}

// ── Region Creation ─────────────────────────────────────────────────────────

i32 shmem_create_region(u32 page_count, u32 flags) {
    if (page_count == 0) page_count = 1;

    u64 phys = pmm_alloc_page();
    if (!phys) return -1;

    kern_shmem_t *sh = (kern_shmem_t *)kmalloc(sizeof(kern_shmem_t));
    if (!sh) {
        pmm_free(phys);
        return -1;
    }

    u64 irq = save_irq_and_disable();
    u32 shm_id = g_next_shm_id++;
    if (g_next_shm_id == 0) g_next_shm_id = 1;

    sh->shm_id     = shm_id;
    sh->phys_base  = phys;
    sh->page_count = page_count;
    sh->flags      = flags;
    sh->ref_count  = 0;
    sh->next       = g_shmem_head;
    g_shmem_head   = sh;
    restore_irq(irq);

    serial_outsf("IPC: shm_id %u created (%u page(s) at phys 0x%llx)\n",
                 shm_id, page_count, phys);
    return (i32)shm_id;
}

// ── Process Attachment Handle Table ─────────────────────────────────────────

static i32 proc_alloc_shmem_slot(kern_process_t *proc) {
    if (!proc) return -1;

    // 1. Recycle any inactive slot
    if (proc->shmem_table) {
        for (u32 i = 0; i < proc->shmem_capacity; i++) {
            if (!proc->shmem_table[i].active) return (i32)i;
        }
    }

    // 2. Expand capacity dynamically (4 -> 8 -> 16 -> 32 ...)
    u32 new_cap = (proc->shmem_capacity == 0) ? 4 : proc->shmem_capacity * 2;
    u64 old_size = (u64)proc->shmem_capacity * sizeof(proc_shmem_entry_t);
    u64 new_size = (u64)new_cap * sizeof(proc_shmem_entry_t);

    proc_shmem_entry_t *new_table = (proc_shmem_entry_t *)kmalloc(new_size);
    if (!new_table) return -1;

    mem_set((u8 *)new_table, 0, new_size);
    if (proc->shmem_table) {
        mem_copy((u8 *)new_table, (const u8 *)proc->shmem_table, old_size);
        kfree(proc->shmem_table);
    }

    i32 slot = (i32)proc->shmem_capacity;
    proc->shmem_table = new_table;
    proc->shmem_capacity = new_cap;
    return slot;
}

i32 shmem_attach_proc(kern_process_t *proc, u32 shm_id) {
    if (!proc || shm_id == 0 || !proc->cr3) return -1;

    u64 irq = save_irq_and_disable();
    kern_shmem_t *sh = shmem_find_global(shm_id);
    if (!sh || sh->page_count == 0 || sh->phys_base == 0) {
        restore_irq(irq);
        return -1;
    }

    // Check if this process already has an active handle for this shm_id
    if (proc->shmem_table) {
        for (u32 i = 0; i < proc->shmem_capacity; i++) {
            if (proc->shmem_table[i].active && proc->shmem_table[i].shm_id == shm_id) {
                restore_irq(irq);
                return (i32)i; // Safely return existing handle
            }
        }
    }

    i32 slot = proc_alloc_shmem_slot(proc);
    if (slot < 0) {
        restore_irq(irq);
        return -1;
    }

    // Bump process heap vptr for virtual mapping
    u64 va = proc->heap_vptr;
    proc->heap_vptr += sh->page_count * PAGE_SIZE;

    // Map physical pages into process PML4
    u32 map_flags = (sh->flags & (PAGE_RW | PAGE_USER)) | PAGE_USER;
    for (u64 p = 0; p < sh->page_count; p++) {
        vmm_map_page_in_pml4(proc->cr3, sh->phys_base + p * PAGE_SIZE,
                             va + p * PAGE_SIZE, map_flags);
    }

    proc->shmem_table[slot].active     = true;
    proc->shmem_table[slot].shm_id     = shm_id;
    proc->shmem_table[slot].virt_addr  = va;
    proc->shmem_table[slot].page_count = sh->page_count;
    proc->shmem_count++;

    sh->ref_count++;
    restore_irq(irq);

    serial_outsf("IPC: PID %d attached to shm_id %u -> handle %d (VA 0x%llx, refs=%u)\n",
                 proc->pid, shm_id, slot, va, sh->ref_count);
    return slot;
}

bool shmem_detach_proc(kern_process_t *proc, i32 handle) {
    if (!proc || handle < 0 || (u32)handle >= proc->shmem_capacity) return false;
    if (!proc->shmem_table || !proc->shmem_table[handle].active) return false;

    u64 irq = save_irq_and_disable();
    proc_shmem_entry_t *entry = &proc->shmem_table[handle];
    u32 shm_id = entry->shm_id;
    u64 va = entry->virt_addr;
    u64 page_count = entry->page_count;

    // Unmap from PML4
    for (u64 p = 0; p < page_count; p++) {
        vmm_unmap_page_in_pml4(proc->cr3, va + p * PAGE_SIZE);
    }

    entry->active = false;
    entry->shm_id = 0;
    entry->virt_addr = 0;
    entry->page_count = 0;
    if (proc->shmem_count > 0) proc->shmem_count--;

    // Update global shmem reference count
    kern_shmem_t *prev = NULL;
    kern_shmem_t *cur = g_shmem_head;
    while (cur && cur->shm_id != shm_id) {
        prev = cur;
        cur = cur->next;
    }

    if (cur) {
        if (cur->ref_count > 0) cur->ref_count--;
        if (cur->ref_count == 0) {
            serial_outsf("IPC: shm_id %u ref_count reached 0; freeing physical memory\n", shm_id);
            for (u64 p = 0; p < cur->page_count; p++) {
                pmm_free(cur->phys_base + p * PAGE_SIZE);
            }
            if (prev) prev->next = cur->next;
            else      g_shmem_head = cur->next;
            kfree(cur);
        }
    }

    restore_irq(irq);
    return true;
}

void ipc_process_cleanup(kern_process_t *proc) {
    if (!proc) return;

    u64 irq = save_irq_and_disable();
    if (proc->shmem_table) {
        for (u32 i = 0; i < proc->shmem_capacity; i++) {
            if (proc->shmem_table[i].active) {
                shmem_detach_proc(proc, (i32)i);
            }
        }
        kfree(proc->shmem_table);
        proc->shmem_table = NULL;
        proc->shmem_capacity = 0;
        proc->shmem_count = 0;
    }
    restore_irq(irq);
}

// ── Direct Read / Write Byte Operations ──────────────────────────────────────

i32 shmem_read_bytes(kern_process_t *proc, i32 handle, u32 offset, void *dst, u32 len) {
    if (!proc || !dst || len == 0) return -1;
    if (handle < 0 || (u32)handle >= proc->shmem_capacity) return -1;
    if (!proc->shmem_table || !proc->shmem_table[handle].active) return -1;

    proc_shmem_entry_t *entry = &proc->shmem_table[handle];
    if (offset + len > entry->page_count * PAGE_SIZE) return -1;

    u64 irq = save_irq_and_disable();
    kern_shmem_t *sh = shmem_find_global(entry->shm_id);
    if (!sh) {
        restore_irq(irq);
        return -1;
    }

    u8 *src_ptr = (u8 *)(sh->phys_base + vmm_get_hhdm()) + offset;
    mem_copy((u8 *)dst, src_ptr, len);
    restore_irq(irq);
    return (i32)len;
}

i32 shmem_write_bytes(kern_process_t *proc, i32 handle, u32 offset, const void *src, u32 len) {
    if (!proc || !src || len == 0) return -1;
    if (handle < 0 || (u32)handle >= proc->shmem_capacity) return -1;
    if (!proc->shmem_table || !proc->shmem_table[handle].active) return -1;

    proc_shmem_entry_t *entry = &proc->shmem_table[handle];
    if (offset + len > entry->page_count * PAGE_SIZE) return -1;

    u64 irq = save_irq_and_disable();
    kern_shmem_t *sh = shmem_find_global(entry->shm_id);
    if (!sh) {
        restore_irq(irq);
        return -1;
    }

    u8 *dst_ptr = (u8 *)(sh->phys_base + vmm_get_hhdm()) + offset;
    mem_copy(dst_ptr, (const u8 *)src, len);
    restore_irq(irq);
    return (i32)len;
}

i32 shmem_write_ring(u32 shm_id, const void *src, u32 len) {
    if (shm_id == 0 || !src || len == 0) return -1;
    u64 irq = save_irq_and_disable();
    kern_shmem_t *sh = shmem_find_global(shm_id);
    if (!sh || sh->page_count == 0) {
        restore_irq(irq);
        return -1;
    }

    u8 *ring = (u8 *)(sh->phys_base + vmm_get_hhdm());
    u32 tail = ring[1];
    const u8 *buf = (const u8 *)src;
    for (u32 i = 0; i < len; i++) {
        ring[2 + tail] = buf[i];
        tail = (tail + 1) & 0xFF; // 256 byte circular ring
    }
    ring[1] = (u8)tail;
    restore_irq(irq);
    return (i32)len;
}

// ── Signals ──────────────────────────────────────────────────────────────────

bool ipc_signal_send(i32 target_pid, u32 signal_mask) {
    if (target_pid <= 0 || signal_mask == 0) return false;

    u64 irq = save_irq_and_disable();

    kern_task_t *task = sched_get_by_pid(target_pid);
    if (!task || !task->process || task->process->pid != target_pid) {
        restore_irq(irq);
        return false;
    }

    kern_process_t *proc = task->process;
    proc->pending_signals |= signal_mask;

    // Wake any BLOCKED task whose wait mask overlaps.
    kern_task_t *head = sched_get_task_list_head();
    if (head) {
        kern_task_t *cur = head;
        do {
            if (cur->process == proc &&
                cur->state == TASK_STATE_BLOCKED &&
                (cur->signal_wait_mask & signal_mask)) {
                sched_unblock(cur);
            }
            cur = cur->next;
        } while (cur != head);
    }

    restore_irq(irq);
    return true;
}

u32 ipc_signal_wait(u32 mask) {
    if (mask == 0) return 0;

    kern_process_t *proc = sched_get_current_process();
    kern_task_t    *task = sched_get_current_task();
    if (!proc || !task) return 0;

    u64 irq = save_irq_and_disable();

    while (1) {
        // Fast path / wakeup check: signal matching mask is pending
        u32 pending = proc->pending_signals & mask;
        if (pending) {
            proc->pending_signals &= ~pending;
            task->signal_wait_mask = 0;
            restore_irq(irq);
            return pending;
        }

        // Block until a matching signal arrives
        task->signal_wait_mask = mask;
        restore_irq(irq);

        sched_block_current();

        irq = save_irq_and_disable();
    }
}

u32 ipc_signal_poll(u32 mask) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return 0;

    u64 irq = save_irq_and_disable();
    u32 pending = proc->pending_signals & mask;
    proc->pending_signals &= ~pending;
    restore_irq(irq);
    return pending;
}

// ── WASM host functions (linked under "env") ─────────────────────────────────

m3ApiRawFunction(wasm_ipc_get_pid) {
    m3ApiReturnType(i32)
    kern_process_t *proc = sched_get_current_process();
    m3ApiReturn(proc ? proc->pid : -1);
}

m3ApiRawFunction(wasm_ipc_shm_create) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, page_count)
    if (page_count == 0) page_count = 1;

    kern_process_t *proc = sched_get_current_process();
    if (!proc) m3ApiReturn(-1);

    i32 shm_id = shmem_create_region(page_count, PAGE_RW | PAGE_USER);
    if (shm_id < 0) m3ApiReturn(-1);

    i32 handle = shmem_attach_proc(proc, (u32)shm_id);
    if (handle < 0) m3ApiReturn(-1);

    // Returns global shm_id (local handle in caller is `handle`)
    m3ApiReturn(shm_id);
}

m3ApiRawFunction(wasm_ipc_shm_attach) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, shm_id)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) m3ApiReturn(-1);

    i32 handle = shmem_attach_proc(proc, shm_id);
    m3ApiReturn(handle);
}

m3ApiRawFunction(wasm_ipc_shm_detach) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) m3ApiReturn(-1);

    bool ok = shmem_detach_proc(proc, handle);
    m3ApiReturn(ok ? 0 : -1);
}

m3ApiRawFunction(wasm_ipc_shm_read_byte) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(u32, offset)

    kern_process_t *proc = sched_get_current_process();
    u8 b = 0;
    i32 res = shmem_read_bytes(proc, handle, offset, &b, 1);
    m3ApiReturn(res == 1 ? (i32)b : -1);
}

m3ApiRawFunction(wasm_ipc_shm_write_byte) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(u32, offset)
    m3ApiGetArg(i32, val)

    kern_process_t *proc = sched_get_current_process();
    u8 b = (u8)val;
    i32 res = shmem_write_bytes(proc, handle, offset, &b, 1);
    m3ApiReturn(res == 1 ? 0 : -1);
}

m3ApiRawFunction(wasm_ipc_shm_read) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(u32, shm_offset)
    m3ApiGetArg(u32, wasm_buf_off)
    m3ApiGetArg(u32, len)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || wasm_buf_off + len > mem_size) m3ApiReturn(-1);

    kern_process_t *proc = sched_get_current_process();
    i32 res = shmem_read_bytes(proc, handle, shm_offset, mem + wasm_buf_off, len);
    m3ApiReturn(res);
}

m3ApiRawFunction(wasm_ipc_shm_write) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(u32, shm_offset)
    m3ApiGetArg(u32, wasm_buf_off)
    m3ApiGetArg(u32, len)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || wasm_buf_off + len > mem_size) m3ApiReturn(-1);

    kern_process_t *proc = sched_get_current_process();
    i32 res = shmem_write_bytes(proc, handle, shm_offset, mem + wasm_buf_off, len);
    m3ApiReturn(res);
}

m3ApiRawFunction(wasm_ipc_signal_send) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, target_pid)
    m3ApiGetArg(i32, mask)
    bool ok = ipc_signal_send(target_pid, (u32)mask);
    m3ApiReturn(ok ? 1 : 0);
}

m3ApiRawFunction(wasm_ipc_signal_wait) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, mask)
    u32 got = ipc_signal_wait((u32)mask);
    m3ApiReturn((i32)got);
}
