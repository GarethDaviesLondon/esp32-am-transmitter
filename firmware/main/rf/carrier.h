/* carrier.h: the carriers and their amplitude modulators.
 *
 * LEDC generates a square wave on each chain's GPIO. One gptimer fires at
 * AM_SAMPLE_RATE_HZ; its ISR takes one audio sample from each chain's sample
 * FIFO, looks it up in that chain's arcsine table, and writes the result
 * straight into that chain's LEDC duty register.
 *
 * Why a table and not arithmetic: the radiated amplitude of a square wave of
 * duty D goes as sin(pi*D), so duty is NOT proportional to amplitude, and
 * around 50% duty it is barely related to it at all (LL-02, KI-01). The table
 * inverts that curve once, at configuration time, so the ISR stays four
 * integer operations long per chain.
 *
 * There are RF_CHAINS of these, indexed 0 and 1, and they are independent
 * transmitters: separate frequency, depth, output pin and sample stream. They
 * share the modulation timer and the 80 MHz LEDC clock, and nothing else.
 * Every call below takes a chain index; out-of-range indices are ignored
 * rather than fatal, because these are reached from web handlers.
 */
#ifndef AMTX_CARRIER_H
#define AMTX_CARRIER_H

#include <stdbool.h>
#include <stdint.h>

#include "am_config.h"
#include "esp_err.h"

/* How the carrier level wanders on its own. A transmitter that never varies
 * sounds nothing like a real distant station; these make it sound like one.
 * The effect is on the carrier itself, not the audio, so it behaves the way
 * propagation does: the whole signal comes and goes, noise and all. */
typedef enum {
    CARRIER_FADE_OFF = 0,
    CARRIER_FADE_SLOW,     /* one smooth sine. Long, lazy fades */
    CARRIER_FADE_FLUTTER,  /* faster sine. Aircraft flutter, auroral wobble */
    CARRIER_FADE_RANDOM,   /* three incommensurate sines: never repeats */
} carrier_fade_t;

typedef struct {
    carrier_fade_t mode;
    uint32_t rate_mhz;     /* millihertz, so 500 is 0.5 Hz */
    int depth_pct;         /* how far down it dips, 0..100 */
} carrier_fade_cfg_t;

typedef struct {
    uint32_t underruns;      /* ISR ticks with an empty FIFO */
    uint32_t fifo_count;     /* samples currently buffered */
    uint32_t fifo_capacity;
    uint32_t carrier_hz;     /* frequency LEDC actually produced */
    float depth;
    float level;             /* commanded carrier level, 0..1 */
    float level_now;         /* after fading, which is what is on air */
    bool enabled;
} carrier_stats_t;

/* Configure LEDC for every chain, start the shared modulation timer, and put
 * both carriers on air.
 * MUST be called from a task pinned to TASK_CORE_AUDIO: the gptimer
 * interrupt is allocated on whichever core registers it, and each FIFO's
 * lockless contract assumes producer and consumer share a core. */
esp_err_t carrier_init(void);

/* Gate one chain's output pin without tearing down the timer. */
void carrier_set_enabled(int ch, bool on);
bool carrier_is_enabled(int ch);

/* Stop and restart the modulation timer around an operation that writes SPI
 * flash. Global, because there is one timer feeding every chain.
 *
 * A flash erase or program disables the instruction cache on both cores for
 * milliseconds at a time. The modulation ISR itself survives that: it is
 * IRAM_ATTR and its tables are DRAM_ATTR, which is what the whole real-time
 * design rests on. But a 20 kHz interrupt taken continuously through the
 * cache-disabled window trips the interrupt watchdog, and the board takes a
 * TG1WDT_SYS_RST before the write lands. See KI-07.
 *
 * These are not a mute: each LEDC channel keeps its last duty, so the pins
 * hold an unmodulated carrier for the few milliseconds the write takes rather
 * than dropping out. Calls do not nest. Safe to call before carrier_init(),
 * where they do nothing. */
void carrier_suspend(void);
void carrier_resume(void);

/* Retune one chain. Rejects anything the LEDC divider cannot reach at the
 * configured duty resolution, which at 8 bits and an 80 MHz clock is above
 * ~312 kHz. Each chain has its own LEDC timer, so the two are free to run at
 * unrelated frequencies. */
esp_err_t carrier_set_frequency(int ch, uint32_t hz);
uint32_t carrier_get_frequency(int ch);

/* Rebuild one chain's duty table for a new modulation depth (0.05 .. 0.90).
 * Cheap enough to call from a web handler; the ISR sees the new table on its
 * next tick and the transition is inaudible. */
esp_err_t carrier_set_depth(int ch, float depth);
float carrier_get_depth(int ch);

/* Carrier level, 0.05 to 1.0. Scales the whole radiated envelope: 1.0 puts
 * the modulation peak at 50% duty where a square wave radiates most, and
 * lower values shrink the envelope towards zero.
 *
 * This is the honest way to turn the power down, and it costs resolution: the
 * envelope occupies proportionally fewer duty steps, so at 0.25 there is a
 * quarter of the amplitude resolution there was at 1.0. Turning the level
 * down and the receiver volume up is not free. */
esp_err_t carrier_set_level(int ch, float level);
float carrier_get_level(int ch);

/* Wander the level on its own, to imitate propagation. Rebuilds the duty
 * table from a timer at CARRIER_FADE_UPDATE_HZ while it is running, and costs
 * nothing at all when the mode is off. */
esp_err_t carrier_set_fade(int ch, const carrier_fade_cfg_t *cfg);
void carrier_get_fade(int ch, carrier_fade_cfg_t *out);

/* Producer side of a chain's sample FIFO. Returns how many samples were
 * accepted; a short count means that modulator is behind. */
uint32_t carrier_push(int ch, const int16_t *samples, uint32_t n);

/* Drop everything buffered on one chain, e.g. when changing station. */
void carrier_flush(int ch);

void carrier_get_stats(int ch, carrier_stats_t *out);

#endif /* AMTX_CARRIER_H */
