#include <cassert>

#include "ultramodern/ultramodern.hpp"

static PTR(OSThread) running_queue_impl = NULLPTR;

static PTR(OSThread)* queue_to_ptr(RDRAM_ARG PTR(PTR(OSThread)) queue) {
    if (queue == ultramodern::running_queue) {
        return &running_queue_impl;
    }
    return TO_PTR(PTR(OSThread), queue);
}

void ultramodern::thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) toadd_) {
    PTR(OSThread)* cur = queue_to_ptr(PASS_RDRAM queue_);
    OSThread* toadd = TO_PTR(OSThread, toadd_); 
    debug_printf("[Thread Queue] Inserting thread %d into queue 0x%08X\n", toadd->id, (uintptr_t)queue_);
    while (*cur && TO_PTR(OSThread, *cur)->priority > toadd->priority) {
        cur = &TO_PTR(OSThread, *cur)->next;
    }
    toadd->next = (*cur);
    toadd->queue = queue_;
    *cur = toadd_;

    debug_printf("  Contains:");
    cur = queue_to_ptr(PASS_RDRAM queue_);
    while (*cur) {
        debug_printf("%d (%d) ", TO_PTR(OSThread, *cur)->id, TO_PTR(OSThread, *cur)->priority);
        cur = &TO_PTR(OSThread, *cur)->next;
    }
    debug_printf("\n");
}

bool ultramodern::thread_queue_to_front(RDRAM_ARG PTR(PTR(OSThread)) queue_, int id) {
    PTR(OSThread)* head = queue_to_ptr(PASS_RDRAM queue_);
    PTR(OSThread)* link = head;
    while (*link != NULLPTR) {
        OSThread* th = TO_PTR(OSThread, *link);
        if (th->id == id) {
            if (link != head) {
                PTR(OSThread) found = *link;
                *link = th->next;
                th->next = *head;
                *head = found;
            }
            return true;
        }
        link = &th->next;
    }
    return false;
}

PTR(OSThread) ultramodern::thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    PTR(OSThread) ret = *queue;
    *queue = TO_PTR(OSThread, ret)->next;
    TO_PTR(OSThread, ret)->queue = NULLPTR;
    debug_printf("[Thread Queue] Popped thread %d from queue 0x%08X\n", TO_PTR(OSThread, ret)->id, (uintptr_t)queue_);
    return ret;
}

bool ultramodern::thread_queue_remove(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_) {
    debug_printf("[Thread Queue] Removing thread %d from queue 0x%08X\n", TO_PTR(OSThread, t_)->id, (uintptr_t)queue_);
    // A running thread is not on a list. osSetThreadPri reads the queue field
    // again after this returns, so the field has to stay as it was.
    if (queue_ == NULLPTR) {
        return false;
    }

    PTR(OSThread)* link = queue_to_ptr(PASS_RDRAM queue_);
    while (*link != NULLPTR) {
        OSThread* th = TO_PTR(OSThread, *link);
        if (*link == t_) {
            *link = th->next;
            return true;
        }
        link = &th->next;
    }

    return false;
}

bool ultramodern::thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    return *queue == NULLPTR;
}

PTR(OSThread) ultramodern::thread_queue_peek(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    return *queue;
}
