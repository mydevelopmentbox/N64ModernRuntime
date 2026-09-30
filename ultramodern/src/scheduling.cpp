#include "ultramodern/ultramodern.hpp"

#include <cstdio>

void ultramodern::schedule_running_thread(RDRAM_ARG PTR(OSThread) t_) {
    debug_printf("[Scheduling] Adding thread %d to the running queue\n", TO_PTR(OSThread, t_)->id);
    thread_queue_insert(PASS_RDRAM running_queue, t_);
    TO_PTR(OSThread, t_)->state = OSThreadState::QUEUED;
}

void swap_to_thread(RDRAM_ARG OSThread *to) {
    debug_printf("[Scheduling] Thread %d giving execution to thread %d\n", TO_PTR(OSThread, ultramodern::this_thread())->id, to->id);
    // Insert this thread in the running queue.
    ultramodern::thread_queue_insert(PASS_RDRAM ultramodern::running_queue, ultramodern::this_thread());
    TO_PTR(OSThread, ultramodern::this_thread())->state = OSThreadState::QUEUED;
    // Unpause the target thread and wait for this one to be unpaused.
    ultramodern::resume_thread_and_wait(PASS_RDRAM to);
}

// Let a lower-priority thread run. The scheduler stays runnable whenever a
// retrace is already queued, which otherwise starves the intro.
extern "C" void kig_log_stall(uint8_t* rdram);

extern "C" void kig_yield_ready(RDRAM_ARG1) {
    static uint32_t last_frame = 0;
    static int same_frame = 0;
    uint32_t frame = *reinterpret_cast<uint32_t*>(rdram + (0x802C9CF4u & 0x1FFFFFFFu));
    if (frame >= 1300u && frame == last_frame) {
        if (++same_frame == 40) {
            kig_log_stall(rdram);
        }
    } else {
        same_frame = 0;
        last_frame = frame;
    }
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        return;
    }
    // A higher-priority thread at the head would otherwise take every yield
    // and leave the intro queued, so the versus screen stops advancing.
    PTR(OSThread) held[8];
    int held_count = 0;
    while (!ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue) && held_count < 8) {
        held[held_count++] = ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue);
    }
    // The intro (1) has to keep the title moving. The match thread (73) is the
    // only receiver on 0x802D3D48; leaving it behind a higher-priority thread
    // fills that queue and the intro blocks forever on the next send.
    static const int prefer_ids[] = {1, 73};
    int chosen = 0;
    bool found = false;
    for (int prefer : prefer_ids) {
        for (int i = 0; i < held_count; i++) {
            if (TO_PTR(OSThread, held[i])->id == prefer) {
                chosen = i;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    for (int i = 0; i < held_count; i++) {
        if (i != chosen) {
            ultramodern::thread_queue_insert(PASS_RDRAM ultramodern::running_queue, held[i]);
        }
    }
    swap_to_thread(PASS_RDRAM TO_PTR(OSThread, held[chosen]));
}

void ultramodern::check_running_queue(RDRAM_ARG1) {
    // Check if there are any threads in the running queue.
    if (!thread_queue_empty(PASS_RDRAM running_queue)) {
        // Check if the highest priority thread in the queue is higher priority than the current thread.
        OSThread* next_thread = TO_PTR(OSThread, ultramodern::thread_queue_peek(PASS_RDRAM running_queue));
        OSThread* self = TO_PTR(OSThread, ultramodern::this_thread());
        if (next_thread->priority > self->priority) {
            if (self->id == 1) {
                static int intro_yield_logs = 0;
                if (intro_yield_logs < 16) {
                    intro_yield_logs++;
                    std::fprintf(stderr, "intro yield to %d pri %d\n", next_thread->id, next_thread->priority);
                    std::fflush(stderr);
                }
            }
            ultramodern::thread_queue_pop(PASS_RDRAM running_queue);
            // Swap to the higher priority thread.
            swap_to_thread(PASS_RDRAM next_thread);
        }
    }
}

extern "C" void pause_self(RDRAM_ARG1) {
    while (true) {
        // This is a recompiled busy-wait. The original spins until an interrupt,
        // which also resumes every other ready thread. Yield to any of them,
        // including the same priority, or the intro stays queued forever.
        ultramodern::wait_for_external_message(PASS_RDRAM1);
        if (!ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
            OSThread* next_thread = TO_PTR(OSThread, ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue));
            swap_to_thread(PASS_RDRAM next_thread);
        }
    }
}

extern "C" void yield_self(RDRAM_ARG1) {
    ultramodern::wait_for_external_message(PASS_RDRAM1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}

extern "C" void yield_self_1ms(RDRAM_ARG1) {
    ultramodern::wait_for_external_message_timed(PASS_RDRAM1, 1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}
