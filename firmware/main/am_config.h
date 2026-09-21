/* am_config.h: every tunable in one place.
 *
 * Each value is #ifndef-guarded, so Kconfig (main/Kconfig.projbuild) or a
 * -D on the command line wins over the default here. Nothing else in the
 * firmware may hard-code a pin, a frequency, or a buffer size: add it here
 * and in Kconfig, or in neither.
 *
 * The RF numbers are a hardware-software contract. Changing one without
 * updating docs/design/03_hardware.md is a defect, not a tweak.
 */
#pragma once

#include "sdkconfig.h"

/* ---------------------------------------------------------------- network */

/* Fallback credentials, used only if NVS holds none. Leave empty and use the
 * SoftAP setup portal instead; never commit a real password. */
#ifndef WIFI_SSID
#define WIFI_SSID CONFIG_AMTX_WIFI_SSID
#endif
#ifndef WIFI_PASS
#define WIFI_PASS CONFIG_AMTX_WIFI_PASS
#endif

/* The two stations a factory-fresh device seeds, one per transmitter, each
 * made that transmitter's power-on station. Any HTTP (not HTTPS) MP3
 * shoutcast/icecast endpoint. Design 07 section 5. */
#ifndef STATION_A_NAME
#define STATION_A_NAME CONFIG_AMTX_STATION_A_NAME
#endif
#ifndef STATION_A_URL
#define STATION_A_URL CONFIG_AMTX_STATION_A_URL
#endif
#ifndef STATION_B_NAME
#define STATION_B_NAME CONFIG_AMTX_STATION_B_NAME
#endif
#ifndef STATION_B_URL
#define STATION_B_URL CONFIG_AMTX_STATION_B_URL
#endif

/* The hostname until the operator sets one, for mDNS (http://amtx.local) and
 * DHCP. Changeable at run time and saved in NVS, because two boards on one
 * network cannot both be amtx.local. Design 07 section 3. */
#ifndef AMTX_HOSTNAME
#define AMTX_HOSTNAME "amtx"
#endif
/* Buffer size including the terminator: 31 characters of name. */
#define HOSTNAME_MAX 32
#ifndef AMTX_AP_SSID
#define AMTX_AP_SSID "AMTX-Setup"     /* provisioning SoftAP */
#endif

/* The BOOT button, GPIO 0 on the ESP32-S3-Zero. Held this long while the board
 * is running, it erases NVS and restarts: design 07 section 7, which also
 * explains why the gesture cannot be a press at power-on (that belongs to the
 * ROM's download mode) and why RESET cannot be read at all.
 *
 * Five seconds is longer than any accidental press and shorter than anyone's
 * patience. A brief press does nothing. */
#ifndef AMTX_FACTORY_BUTTON_GPIO
#define AMTX_FACTORY_BUTTON_GPIO CONFIG_AMTX_FACTORY_BUTTON_GPIO
#endif
#ifndef AMTX_FACTORY_HOLD_MS
#define AMTX_FACTORY_HOLD_MS CONFIG_AMTX_FACTORY_HOLD_MS
#endif

/* The serial console is the only diagnostic on a board with no display, and
 * the interesting question during setup is always the same one: is it on the
 * network, and at what address? Say so on a fixed cadence, so silence on the
 * monitor means "nothing is running" rather than "nothing has changed".
 * 0 disables the heartbeat. */
#ifndef AMTX_WIFI_HEARTBEAT_S
#define AMTX_WIFI_HEARTBEAT_S 30
#endif

/* Redirect hops a station URL may take before the stream gives up. CDN
 * hand-offs and http-to-https promotions routinely take two. */
#ifndef STREAM_REDIRECT_MAX
#define STREAM_REDIRECT_MAX 5
#endif

/* 64 KB of raw MP3 in flight: about 4 s at 128 kbit/s. Absorbs network
 * jitter so the decoder never starves on a slow TCP window. */
#ifndef STREAM_RINGBUF_BYTES
#define STREAM_RINGBUF_BYTES (64 * 1024)
#endif

/* ----------------------------------------------------------------- RF path */

#ifndef RF_CARRIER_GPIO
#define RF_CARRIER_GPIO CONFIG_AMTX_RF_GPIO
#endif

/* Two independent transmitters: two carriers, two stations, two sets of audio
 * settings. They share only the 20 kHz modulation timer and the 80 MHz LEDC
 * clock. Each needs its own output low-pass network; a shared one lets them
 * intermodulate, and the third harmonic of a 200 kHz carrier lands at 600 kHz
 * where a second carrier might well be. */
#ifndef RF_CHAINS
#define RF_CHAINS 2
#endif

#ifndef RF_CARRIER_GPIO_B
#define RF_CARRIER_GPIO_B CONFIG_AMTX_RF_GPIO_B
#endif

/* Chain B's default. Kept clear of chain A's 200 kHz and of its own harmonic
 * relationship to it: 250 kHz is not an integer multiple of 200 kHz, so the
 * two do not reinforce each other's spurs. Both are inside the reachable
 * range, which at 8-bit duty on an 80 MHz clock tops out at 312.5 kHz. */
#ifndef RF_CARRIER_HZ_B
#define RF_CARRIER_HZ_B 250000
#endif

/* 200 kHz sits in the longwave broadcast band (148.5-283.5 kHz), 2 kHz from
 * BBC Radio 4 on 198 kHz. See KI-04: keep the field local. */
#ifndef RF_CARRIER_HZ
#define RF_CARRIER_HZ 200000
#endif

/* 8 bits = 256 duty steps per carrier period. At 200 kHz on the 80 MHz LEDC
 * clock this is the hardware ceiling: 80e6/200e3 = 400 clocks per period, so
 * 9 bits (512 steps) will not fit. */
#ifndef RF_DUTY_RES_BITS
#define RF_DUTY_RES_BITS 8
#endif

/* Modulation depth m. The envelope swings between A0*(1-m) and A0*(1+m);
 * the positive peak is placed exactly at 50% duty, the point of maximum
 * radiated amplitude, so the unmodulated carrier sits at 1/(1+m) of it.
 * Keep at or below 0.75: beyond that the trough pinches the carrier and an
 * envelope detector produces gross distortion. */
#ifndef AM_MODULATION_DEPTH
#define AM_MODULATION_DEPTH 0.70f
#endif

/* Duty modulation is linearised through an arcsine table (LL-02/KI-01), so
 * there is no separate bias setting: the bias falls out of the depth. This
 * is the resulting quiescent duty as a fraction, recorded for the docs and
 * asserted at startup. asin(1/1.70)/pi = 0.2002 for m = 0.70. */
#ifndef AM_DUTY_TABLE_ENTRIES
#define AM_DUTY_TABLE_ENTRIES 512
#endif

/* How often the fade engine recomputes the carrier level and rebuilds a duty
 * table. Each rebuild is AM_DUTY_TABLE_ENTRIES arcsines, so this is a real
 * cost and it is paid only while a chain is actually fading. 100 Hz gives ten
 * updates per cycle at the fastest flutter rate the UI offers, which is
 * smooth enough not to hear as steps. */
#ifndef CARRIER_FADE_UPDATE_HZ
#define CARRIER_FADE_UPDATE_HZ 100
#endif

/* Push duty-quantiser error above the audio band with first-order error
 * feedback. Costs two adds in the ISR. Set to 0 to compare A/B on a scope. */
#ifndef RF_NOISE_SHAPING
#define RF_NOISE_SHAPING 1
#endif

/* --------------------------------------------------------------- audio DSP */

/* Modulation update rate. One duty write per tick. 20 kHz gives a 10 kHz
 * Nyquist for a 4.5 kHz audio band, and puts resampling images at the
 * carrier +/- 20 kHz. See EH-05 before raising it. */
#ifndef AM_SAMPLE_RATE_HZ
#define AM_SAMPLE_RATE_HZ 20000
#endif

/* Sharp enough that nothing lands in the neighbouring 9 kHz channel. */
#ifndef AUDIO_LPF_CUTOFF_HZ
#define AUDIO_LPF_CUTOFF_HZ CONFIG_AMTX_LPF_CUTOFF_HZ
#endif

/* Cascaded biquads in the anti-alias / splatter filter. 3 sections is a
 * 6th-order Butterworth: 41 dB down at 10 kHz, where a 2nd-order design
 * manages only 14 dB. */
#ifndef AUDIO_LPF_SECTIONS
#define AUDIO_LPF_SECTIONS 3
#endif

/* One-pole DC blocker. MP3 decoders emit a DC offset on some material, and
 * DC on this path shifts the carrier bias rather than being inaudible. */
#ifndef AUDIO_DC_BLOCK_HZ
#define AUDIO_DC_BLOCK_HZ 25.0f
#endif

/* Limiter ceiling as a fraction of full scale, and its time constants.
 * Fast enough to catch a transient, slow enough not to pump. */
#ifndef AUDIO_LIMIT_CEILING
#define AUDIO_LIMIT_CEILING 0.95f
#endif
#ifndef AUDIO_LIMIT_ATTACK_MS
#define AUDIO_LIMIT_ATTACK_MS 1.0f
#endif
#ifndef AUDIO_LIMIT_RELEASE_MS
#define AUDIO_LIMIT_RELEASE_MS 250.0f
#endif

/* Programme gain applied before the limiter, as a percentage. Broadcast AM
 * runs hot; 100 means unity on a nominally full-scale stream. */
#ifndef AUDIO_GAIN_PERCENT_DEFAULT
#define AUDIO_GAIN_PERCENT_DEFAULT 140
#endif

/* What the AGC aims the programme average at, as a percentage of full scale.
 * 25% is about -12 dBFS, which leaves the limiter room to work on peaks while
 * still using most of the modulation depth. */
#ifndef AUDIO_AGC_TARGET_PERCENT
#define AUDIO_AGC_TARGET_PERCENT 25
#endif

/* Samples buffered between the decode task and the modulation ISR. Must be a
 * power of two. 2048 at 20 kHz is 102 ms; the controller holds it near half
 * full, which is two MP3 frames of headroom against decode jitter. */
#ifndef SAMPLE_FIFO_LEN
#define SAMPLE_FIFO_LEN 2048
#endif

/* The resampler nudges its step to hold the FIFO at this fill, absorbing the
 * drift between the station's clock and this board's crystal. Without it the
 * FIFO walks to one end and clicks every few minutes. */
#ifndef SAMPLE_FIFO_TARGET
#define SAMPLE_FIFO_TARGET (SAMPLE_FIFO_LEN / 2)
#endif

/* Maximum step correction, in parts per million. 5000 ppm = 0.5%, far more
 * than two crystals can differ, and small enough to be inaudible. */
#ifndef RESAMPLER_TRIM_PPM_MAX
#define RESAMPLER_TRIM_PPM_MAX 5000
#endif

/* ------------------------------------------------------------ task layout */

/* Core 0 carries Wi-Fi, lwIP and the web server already; the stream task
 * joins them there so nothing on the audio path ever waits on the network. */
#define TASK_CORE_NET   0
#define TASK_CORE_AUDIO 1

#define TASK_PRIO_STREAM  5
#define TASK_PRIO_DECODE 10

#define TASK_STACK_STREAM  8192
#define TASK_STACK_DECODE 12288

/* ----------------------------------------------------------------- storage */

/* ------------------------------------------------ station discovery (EH-03) */

/* Radio-Browser is a community directory served by a handful of donated
 * mirrors. A fixed host keeps the client simple; if this one retires, the
 * round-robin name all.api.radio-browser.info picks another. */
#ifndef DISCOVER_API_HOST
#define DISCOVER_API_HOST "de1.api.radio-browser.info"
#endif

/* Ten, not a screenful, and the limit is memory rather than taste. Radio-
 * Browser returns about thirty fields per station -- favicon, geo, codec,
 * click counters -- so a result is roughly 1.5 kB of JSON, and the whole
 * document has to be resident for cJSON to parse it. An HTTPS fetch is the
 * worst moment to ask for a large contiguous block: mbedTLS with the full
 * certificate bundle has just taken tens of kB, and only about 100 kB of
 * internal heap exists in the first place. Twenty results asked for ~30 kB
 * after the handshake and failed outright. See EH-03; PSRAM would lift this. */
#ifndef DISCOVER_RESULTS_MAX
#define DISCOVER_RESULTS_MAX 10
#endif

/* Ceiling on the reply pulled into RAM, allocated once before the connection
 * is opened so it is claimed while the heap is least fragmented. */
#ifndef DISCOVER_BODY_MAX
#define DISCOVER_BODY_MAX (20 * 1024)
#endif

#define STATION_MAX      20
#define STATION_NAME_MAX 40
#define STATION_URL_MAX  160
#define WIFI_CRED_MAX     5
