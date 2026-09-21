/* sample_fifo.h: the handover from the decode task to the modulation ISR.
 *
 * Single producer (the decode task on core 1), single consumer (the 20 kHz
 * gptimer ISR, also on core 1). Lockless by construction: the producer only
 * ever writes `head`, the consumer only ever writes `tail`, and the buffer
 * length is a power of two so the wrap is a mask.
 *
 * Header-only and `static inline` on purpose: the consumer side is called
 * from an IRAM ISR, so the code must land in the caller's section rather than
 * in a flash-resident function. The buffer instance itself must be declared
 * DRAM_ATTR by whoever owns it.
 *
 * Do not add a second writer. Do not "just take a mutex": a mutex on this
 * path is a flash access and a scheduler call inside an interrupt.
 */
#ifndef AMTX_SAMPLE_FIFO_H
#define AMTX_SAMPLE_FIFO_H

#include <stdbool.h>
#include <stdint.h>

#include "am_config.h"

#if (SAMPLE_FIFO_LEN & (SAMPLE_FIFO_LEN - 1)) != 0
#error "SAMPLE_FIFO_LEN must be a power of two"
#endif

#define SAMPLE_FIFO_MASK (SAMPLE_FIFO_LEN - 1)

typedef struct {
    uint32_t head;                    /* producer only */
    uint32_t tail;                    /* consumer only */
    int16_t buf[SAMPLE_FIFO_LEN];
} sample_fifo_t;

static inline void sample_fifo_reset(sample_fifo_t *f)
{
    f->head = 0;
    f->tail = 0;
}

/* Both indices are read with acquire ordering and written with release
 * ordering, so a sample is never visible before the data it points at. On
 * Xtensa these compile to a plain load or store plus a memory barrier. */
static inline uint32_t sample_fifo_count(const sample_fifo_t *f)
{
    const uint32_t head = __atomic_load_n(&f->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&f->tail, __ATOMIC_ACQUIRE);
    return (head - tail) & SAMPLE_FIFO_MASK;
}

static inline uint32_t sample_fifo_space(const sample_fifo_t *f)
{
    /* One slot is always left empty so full and empty stay distinguishable. */
    return SAMPLE_FIFO_MASK - sample_fifo_count(f);
}

/* Producer. Writes as many of `n` as fit; returns how many were taken.
 * A short return means the consumer is behind, which is the resampler
 * controller's cue to slow down rather than an error. */
static inline uint32_t sample_fifo_write(sample_fifo_t *f, const int16_t *src,
                                         uint32_t n)
{
    uint32_t head = f->head;
    const uint32_t tail = __atomic_load_n(&f->tail, __ATOMIC_ACQUIRE);
    const uint32_t space = SAMPLE_FIFO_MASK - ((head - tail) & SAMPLE_FIFO_MASK);

    if (n > space) n = space;
    for (uint32_t i = 0; i < n; i++) {
        f->buf[(head + i) & SAMPLE_FIFO_MASK] = src[i];
    }
    __atomic_store_n(&f->head, (head + n) & SAMPLE_FIFO_MASK, __ATOMIC_RELEASE);
    return n;
}

/* Consumer side. Discards everything buffered.
 *
 * Flushing from the producer looks tempting and is wrong: it would write
 * `tail`, and if the consumer advanced `tail` in between, `head` ends up
 * behind it and the count wraps to nearly a full buffer. So the producer asks
 * (carrier_flush) and the ISR does it. */
static inline void sample_fifo_drain(sample_fifo_t *f)
{
    __atomic_store_n(&f->tail, __atomic_load_n(&f->head, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
}

/* Consumer, called from the ISR. Returns false when the FIFO is empty; the
 * caller decides what an underrun sounds like. */
static inline bool sample_fifo_read(sample_fifo_t *f, int16_t *out)
{
    const uint32_t tail = f->tail;
    const uint32_t head = __atomic_load_n(&f->head, __ATOMIC_ACQUIRE);

    if (tail == head) return false;

    *out = f->buf[tail];
    __atomic_store_n(&f->tail, (tail + 1) & SAMPLE_FIFO_MASK, __ATOMIC_RELEASE);
    return true;
}

#endif /* AMTX_SAMPLE_FIFO_H */
