#include <bitset>
#include <cstdio>
#include <thread>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    bool requeue_if_blocked;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};
std::bitset<32> requeue_enabled;

void ultramodern::set_message_queue_control(const ultramodern::MessageQueueControl& mqc) {
    requeue_enabled.reset();
    requeue_enabled.set(static_cast<int>(EventMessageSource::Timer), mqc.requeue_timer);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Sp), mqc.requeue_sp);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Si), mqc.requeue_si);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Ai), mqc.requeue_ai);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Vi), mqc.requeue_vi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Pi), mqc.requeue_pi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Dp), mqc.requeue_dp);
}

void ultramodern::enqueue_external_message_src(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, EventMessageSource src) {
    external_messages.enqueue({mq, msg, jam, requeue_enabled[static_cast<int>(src)]});
}

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked});
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    std::vector<QueuedMessage> requeued_messages{};
    while (external_messages.try_dequeue(to_send)) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            requeued_messages.push_back(to_send);
        }
    }
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
}

void ultramodern::drain_external_messages(RDRAM_ARG1) {
    dequeue_external_messages(PASS_RDRAM1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
        external_messages.enqueue(to_send);
    }
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            external_messages.enqueue(to_send);
        }
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // Retraces are non-blocking and share small queues with the frame
        // loop. A full queue must still accept the new message, and the
        // enqueue has to run so a thread blocked on receive is woken.
        if (MQ_IS_FULL(mq) && mq->validCount > 0 && mq->msgCount > 0) {
            // The scheduler queue jams DP (1) and SP (2) at the head. Dropping
            // the oldest message there discards the completion, so the frame
            // never swaps and the intro stops simulating. Drop a retrace instead.
            bool dropped = false;
            if ((uint32_t)mq_ == 0x8000CF70u || (uint32_t)mq_ == 0x8004A168u) {
                OSMesg* buf = TO_PTR(OSMesg, mq->msg);
                for (s32 i = 0; i < mq->validCount; i++) {
                    OSMesg slot = buf[(mq->first + i) % mq->msgCount];
                    bool drop_slot = false;
                    if ((uint32_t)mq_ == 0x8000CF70u) {
                        drop_slot = slot == 0;
                    } else {
                        uint32_t m = (uint32_t)slot;
                        if (m >= 0x80000000u && m < 0x80800000u) {
                            // Halfwords live at the swapped half, matching MEM_H.
                            int16_t typ = *reinterpret_cast<int16_t*>(rdram + ((m ^ 2u) & 0x1FFFFFFFu));
                            drop_slot = typ == 1;
                        }
                    }
                    if (!drop_slot) {
                        continue;
                    }
                    for (s32 j = i; j < mq->validCount - 1; j++) {
                        buf[(mq->first + j) % mq->msgCount] = buf[(mq->first + j + 1) % mq->msgCount];
                    }
                    mq->validCount--;
                    dropped = true;
                    break;
                }
            } else {
                mq->first = (mq->first + 1) % mq->msgCount;
                mq->validCount--;
                dropped = true;
            }
            (void)dropped;
        }
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
            // A frame-done that cannot be queued never reaches the intro, so
            // the in-flight gfx count would stay high and new frames stop.
            if ((uint32_t)mq_ == 0x8004A168u) {
                uint32_t m = (uint32_t)msg;
                int16_t typ = -1;
                if (m >= 0x80000000u && m < 0x80800000u) {
                    typ = *reinterpret_cast<int16_t*>(rdram + ((m ^ 2u) & 0x1FFFFFFFu));
                    if (typ == 2) {
                        int32_t* ctr = reinterpret_cast<int32_t*>(rdram + (0x8004A308u & 0x1FFFFFFFu));
                        if (*ctr > 0) {
                            *ctr -= 1;
                        }
                    }
                }
                static int fail_logs = 0;
                int fail_n = fail_logs++;
                if (fail_n == 0 || fail_n == 40 || fail_n == 200 || fail_n == 800) {
                    std::fprintf(stderr, "intro send fail n %d msg %08X typ %d gfx %d q %d/%d\n",
                        fail_n,
                        m, (int)typ, (int)*reinterpret_cast<int32_t*>(rdram + (0x8004A308u & 0x1FFFFFFFu)),
                        (int)mq->validCount, (int)mq->msgCount);
                    OSMesg* buf = TO_PTR(OSMesg, mq->msg);
                    for (s32 i = 0; i < mq->validCount && i < 8; i++) {
                        uint32_t slot = (uint32_t)buf[(mq->first + i) % mq->msgCount];
                        int16_t st = -1;
                        if (slot >= 0x80000000u && slot < 0x80800000u) {
                            st = *reinterpret_cast<int16_t*>(rdram + ((slot ^ 2u) & 0x1FFFFFFFu));
                        }
                        std::fprintf(stderr, "  slot %d %08X typ %d\n", (int)i, slot, (int)st);
                    }
                    std::fflush(stderr);
                }
            }
            return false;
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        while (MQ_IS_FULL(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            // Thread 73 is the only receiver. A full queue with that thread
            // stuck behind a higher priority never drains, so the intro's
            // blocking send of the match-start message never returns.
            if ((uint32_t)mq_ == 0x802D3D48u) {
                static int promote_logs = 0;
                bool queued = ultramodern::thread_queue_to_front(PASS_RDRAM ultramodern::running_queue, 73);
                if (!queued) {
                    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
                    if (ultramodern::thread_queue_to_front(PASS_RDRAM blocked_queue, 73)) {
                        PTR(OSThread) t = ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue);
                        ultramodern::schedule_running_thread(PASS_RDRAM t);
                        ultramodern::thread_queue_to_front(PASS_RDRAM ultramodern::running_queue, 73);
                        queued = true;
                    }
                }
                if (promote_logs < 4) {
                    promote_logs++;
                    std::fprintf(stderr, "match promote 73 queued %d msg %08X\n", queued ? 1 : 0, static_cast<unsigned>(msg));
                    std::fflush(stderr);
                }
            }
            if (TO_PTR(OSThread, ultramodern::this_thread())->id == 1) {
                static int intro_send_block = 0;
                if (intro_send_block < 2) {
                    intro_send_block++;
                    std::fprintf(stderr, "intro send block q %08X count %d/%d\n",
                        static_cast<unsigned>(mq_), (int)mq->validCount, (int)mq->msgCount);
                    PTR(OSThread) ready = ultramodern::thread_queue_peek(PASS_RDRAM ultramodern::running_queue);
                    while (ready != NULLPTR) {
                        OSThread* th = TO_PTR(OSThread, ready);
                        std::fprintf(stderr, "  ready id %d pri %d\n", th->id, th->priority);
                        ready = th->next;
                    }
                    std::fflush(stderr);
                }
            }
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }
    
    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
        static int jam_logs = 0;
        if (msg == 1 && jam_logs < 4) {
            jam_logs++;
            std::fprintf(stderr, "mq jam %08X msg %08X count %d\n",
                static_cast<unsigned>(mq_), static_cast<unsigned>(msg), static_cast<int>(mq->validCount));
            std::fflush(stderr);
        }
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if ((uint32_t)mq_ == 0x8004A168u) {
        static int intro_send_logs = 0;
        int send_n = intro_send_logs++;
        if (send_n < 4 || send_n == 500 || send_n == 2000 || send_n == 8000) {
            std::fprintf(stderr, "intro send n %d count %d waiter %d\n", send_n, mq->validCount,
                ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue) ? 0 : 1);
            std::fflush(stderr);
        }
    }
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }
    
    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (block && (uint32_t)mq_ == 0x8004A168u) {
        static int intro_blockrecv_logs = 0;
        if (intro_blockrecv_logs < 4) {
            intro_blockrecv_logs++;
            std::fprintf(stderr, "intro blockrecv count %d empty %d\n", mq->validCount, MQ_IS_EMPTY(mq) ? 1 : 0);
            std::fflush(stderr);
        }
    }
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        if ((uint32_t)mq_ == 0x8000CF70u && MQ_IS_EMPTY(mq)) {
            static int sched_block_logs = 0;
            if (sched_block_logs < 16) {
                sched_block_logs++;
                PTR(OSThread) head = ultramodern::thread_queue_peek(PASS_RDRAM ultramodern::running_queue);
                int hid = head == NULLPTR ? -1 : TO_PTR(OSThread, head)->id;
                int hpri = head == NULLPTR ? -1 : TO_PTR(OSThread, head)->priority;
                std::fprintf(stderr, "sched block head %d pri %d\n", hid, hpri);
                std::fflush(stderr);
            }
        }
        // Otherwise, yield this thread in a loop until the queue is no longer full
        while (MQ_IS_EMPTY(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            if (TO_PTR(OSThread, ultramodern::this_thread())->id == 1) {
                static int intro_block_logs = 0;
                int bn = intro_block_logs++;
                if (bn < 4 || bn == 30 || bn == 80 || bn == 200) {
                    std::fprintf(stderr, "intro recv block n %d q %08X\n", bn, static_cast<unsigned>(mq_));
                    std::fflush(stderr);
                }
            }
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
            if ((uint32_t)mq_ == 0x8004A168u) {
                static int intro_wake_logs = 0;
                if (intro_wake_logs < 6) {
                    intro_wake_logs++;
                    std::fprintf(stderr, "intro wake count %d\n", mq->validCount);
                }
            }
        }
    }

    if (block && (uint32_t)mq_ == 0x8004A168u) {
        static int intro_got_logs = 0;
        if (intro_got_logs < 4) {
            intro_got_logs++;
            std::fprintf(stderr, "intro recv got count %d\n", mq->validCount);
            std::fflush(stderr);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    
    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to receive a message.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}
