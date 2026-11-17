#include "code_patcher.h"

#include "arm64_instructions.h"

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define RET_INSTRUCTION 0xD65F03C0u
#define JUMP_INSTRUCTIONS 4

code_patch code_patch_jump(uintptr_t address, uintptr_t target) {
    code_patch patch = { .address = address, .count = JUMP_INSTRUCTIONS };
    arm64_write_absolute_jump(patch.instructions, target);
    return patch;
}

code_patch code_patch_return(uintptr_t address) {
    return (code_patch){ .address = address, .instructions = { RET_INSTRUCTION }, .count = 1 };
}

void *trampoline_create(uintptr_t function, size_t count) {
    size_t size = (size_t)getpagesize();
    uint32_t *code = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED)
        return NULL;
    memcpy(code, (const void *)function, count * 4);
    arm64_write_absolute_jump(code + count, function + count * 4);
    if (mprotect(code, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, size);
        return NULL;
    }
    sys_icache_invalidate(code, size);
    return code;
}

void trampoline_destroy(void *trampoline) {
    if (trampoline)
        munmap(trampoline, (size_t)getpagesize());
}

static void page_range(const code_patch *patch, mach_vm_address_t *start, mach_vm_size_t *size) {
    uintptr_t page = (uintptr_t)getpagesize();
    uintptr_t first = patch->address & ~(page - 1);
    uintptr_t last = (patch->address + patch->count * 4 + page - 1) & ~(page - 1);
    *start = first;
    *size = last - first;
}

static bool protect(const code_patch *patch, vm_prot_t protection) {
    mach_vm_address_t start;
    mach_vm_size_t size;
    page_range(patch, &start, &size);
    return mach_vm_protect(mach_task_self(), start, size, FALSE, protection) == KERN_SUCCESS;
}

bool code_patches_apply(const code_patch *patches, size_t count) {
    // VM_PROT_COPY gives this process its own writable copy of the file-backed page.
    for (size_t i = 0; i < count; i++) {
        if (!protect(&patches[i], VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
            for (size_t j = 0; j < i; j++)
                protect(&patches[j], VM_PROT_READ | VM_PROT_EXECUTE);
            return false;
        }
    }
    for (size_t i = 0; i < count; i++)
        memcpy((void *)patches[i].address, patches[i].instructions, patches[i].count * 4);
    for (size_t i = 0; i < count; i++) {
        protect(&patches[i], VM_PROT_READ | VM_PROT_EXECUTE);
        sys_icache_invalidate((void *)patches[i].address, patches[i].count * 4);
    }
    return true;
}
