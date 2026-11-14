#ifndef REDWOOD_VENUS_CS35_POWER_H
#define REDWOOD_VENUS_CS35_POWER_H

#include <stdbool.h>
#include <stdint.h>

#define VENUS_CS35_PWR1 0x2014u
#define VENUS_CS35_PWR2 0x2018u
#define VENUS_CS35_POWER_IRQ 0x10010u
#define VENUS_CS35_POWER_UP 0x01000000u
#define VENUS_CS35_POWER_DOWN 0x00800000u
#define VENUS_CS35_POWER_FAULTS 0x800281c0u
#define VENUS_CS35_OUTPUT_BLOCKS 0x31u
#define VENUS_CS35_BENCH_BLOCKS 0x21u

struct venus_cs35_power_io {
	void *context;
	bool (*read)(void *context, uint32_t reg, uint32_t *value);
	bool (*write)(void *context, uint32_t reg, uint32_t value);
	bool (*guard)(void *context);
	bool (*reset)(void *context);
	uint64_t (*now_us)(void *context);
	void (*delay_ms)(void *context);
};

enum venus_cs35_power_result {
	VENUS_CS35_POWER_OK,
	VENUS_CS35_POWER_REFUSED,
	VENUS_CS35_POWER_RESET,
	VENUS_CS35_POWER_RESET_FAILED,
};

static inline bool
venus_cs35_power_write_checked(const struct venus_cs35_power_io *io, uint32_t reg, uint32_t value)
{
	uint32_t readback;

	return io->write(io->context, reg, value) &&
	       io->read(io->context, reg, &readback) && readback == value;
}

/* These power-transition keys are from the Venus main-amp event, separate from B2 probe errata. */
static inline bool
venus_cs35_power_patch(const struct venus_cs35_power_io *io, uint32_t value)
{
	bool programmed = io->write(io->context, 0x40u, 0x55u) &&
	    io->write(io->context, 0x40u, 0xaau) &&
	    venus_cs35_power_write_checked(io, 0x2084u, value);
	bool lock1 = io->write(io->context, 0x40u, 0xccu);
	bool lock2 = io->write(io->context, 0x40u, 0x33u);

	return programmed && lock1 && lock2;
}

static inline bool
venus_cs35_power_events_clear(const struct venus_cs35_power_io *io)
{
	uint32_t irq;

	return io->read(io->context, VENUS_CS35_POWER_IRQ, &irq) &&
	       (irq & (VENUS_CS35_POWER_FAULTS | VENUS_CS35_POWER_UP | VENUS_CS35_POWER_DOWN)) == 0;
}

static inline bool
venus_cs35_power_event_acknowledged(const struct venus_cs35_power_io *io, uint32_t event)
{
	uint32_t irq;

	return io->read(io->context, VENUS_CS35_POWER_IRQ, &irq) &&
	       (irq & (VENUS_CS35_POWER_FAULTS | event)) == 0;
}

/* Count the enable write, readback and guard transactions in the transition deadline. */
static inline bool
venus_cs35_power_transition_events(const struct venus_cs35_power_io *io, uint32_t power,
    uint32_t event, uint32_t unexpected)
{
	const uint64_t began = io->now_us(io->context);

	if (!venus_cs35_power_write_checked(io, VENUS_CS35_PWR1, power)) {
		return false;
	}
	for (unsigned i = 0; i < 100 && io->now_us(io->context) - began < 100000u; i++) {
		uint32_t irq;

		if (!io->read(io->context, VENUS_CS35_POWER_IRQ, &irq) ||
		    (irq & (VENUS_CS35_POWER_FAULTS | unexpected)) != 0 || !io->guard(io->context) ||
		    io->now_us(io->context) - began >= 100000u) {
			return false;
		}
		if ((irq & event) != 0 && (irq & unexpected) == 0) {
			/* W1C acknowledgement must complete and read back within the same deadline. */
			return io->write(io->context, VENUS_CS35_POWER_IRQ, event) &&
			       venus_cs35_power_event_acknowledged(io, event) &&
			       io->now_us(io->context) - began < 100000u;
		}
		io->delay_ms(io->context);
	}
	return false;
}

static inline bool
venus_cs35_power_transition(const struct venus_cs35_power_io *io, uint32_t power, uint32_t event)
{
	return venus_cs35_power_transition_events(io, power, event,
	    (VENUS_CS35_POWER_UP | VENUS_CS35_POWER_DOWN) & ~event);
}

/* One muted transition, with no retry after a failed write, guard, or event timeout. */
static inline enum venus_cs35_power_result
venus_cs35_muted_power_cycle(const struct venus_cs35_power_io *io)
{
	uint32_t power, blocks, irq;
	const uint32_t events = VENUS_CS35_POWER_UP | VENUS_CS35_POWER_DOWN;

	if (!io->guard(io->context) ||
	    !io->read(io->context, VENUS_CS35_PWR1, &power) || (power & 1u) != 0 ||
	    !io->read(io->context, VENUS_CS35_PWR2, &blocks) ||
	    (blocks & VENUS_CS35_OUTPUT_BLOCKS) != 0 ||
	    !io->read(io->context, VENUS_CS35_POWER_IRQ, &irq) ||
	    (irq & (VENUS_CS35_POWER_FAULTS | events)) != 0) {
		return VENUS_CS35_POWER_REFUSED;
	}

	if (venus_cs35_power_write_checked(io, VENUS_CS35_PWR2, blocks | VENUS_CS35_BENCH_BLOCKS) &&
	    venus_cs35_power_patch(io, 0x2f1aa0u) && io->guard(io->context) &&
	    venus_cs35_power_events_clear(io) &&
	    venus_cs35_power_transition(io, power | 1u, VENUS_CS35_POWER_UP) &&
	    venus_cs35_power_transition(io, power, VENUS_CS35_POWER_DOWN) &&
	    venus_cs35_power_patch(io, 0x2f1aa3u) &&
	    venus_cs35_power_write_checked(io, VENUS_CS35_PWR2, blocks) &&
	    io->guard(io->context)) {
		return VENUS_CS35_POWER_OK;
	}
	return io->reset(io->context) ? VENUS_CS35_POWER_RESET : VENUS_CS35_POWER_RESET_FAILED;
}

/* Retain a muted power stage between separate startup and shutdown calls.
 * The owner unmutes only after its protection DSP has acknowledged RESUME,
 * and mutes/pauses it before stop. A failed transition poisons the state;
 * no I2C transaction or automatic retry follows the emergency reset. */
struct venus_cs35_power_state {
	uint32_t power, blocks;
	uint64_t down_began;
	bool active, poisoned, down_started, restart_ready;
};

static inline enum venus_cs35_power_result
venus_cs35_power_abort(const struct venus_cs35_power_io *io, struct venus_cs35_power_state *state)
{
	state->active = false;
	state->poisoned = true;
	return io->reset(io->context) ? VENUS_CS35_POWER_RESET : VENUS_CS35_POWER_RESET_FAILED;
}

static inline enum venus_cs35_power_result
venus_cs35_muted_power_start(const struct venus_cs35_power_io *io, struct venus_cs35_power_state *state)
{
	uint32_t power, blocks;
	uint32_t irq;
	if (state->active || state->poisoned || !io->guard(io->context) ||
	    !io->read(io->context, VENUS_CS35_PWR1, &power) || (power & 1u) != 0 ||
	    !io->read(io->context, VENUS_CS35_PWR2, &blocks) || (blocks & VENUS_CS35_OUTPUT_BLOCKS) != 0 ||
	    !io->read(io->context, VENUS_CS35_POWER_IRQ, &irq) ||
	    (irq & VENUS_CS35_POWER_FAULTS) != 0 ||
	    (!state->restart_ready && (irq & (VENUS_CS35_POWER_UP | VENUS_CS35_POWER_DOWN)) != 0)) {
		return VENUS_CS35_POWER_REFUSED;
	}
	if ((irq & (VENUS_CS35_POWER_UP | VENUS_CS35_POWER_DOWN)) != 0 &&
	    (!io->write(io->context, VENUS_CS35_POWER_IRQ, VENUS_CS35_POWER_UP | VENUS_CS35_POWER_DOWN) ||
	     !venus_cs35_power_events_clear(io))) {
		return venus_cs35_power_abort(io, state);
	}
	state->restart_ready = false;
	state->power = power;
	state->blocks = blocks;
	if (!venus_cs35_power_write_checked(io, VENUS_CS35_PWR2, blocks | VENUS_CS35_BENCH_BLOCKS) ||
	    !venus_cs35_power_patch(io, 0x2f1aa0u) || !io->guard(io->context) ||
	    !venus_cs35_power_events_clear(io) ||
	    !venus_cs35_power_transition(io, power | 1u, VENUS_CS35_POWER_UP)) {
		return venus_cs35_power_abort(io, state);
	}
	state->active = true;
	return VENUS_CS35_POWER_OK;
}

/* Mark the start of this shutdown before the owner clears AMP_EN and
 * pauses CSPL. PDN_DONE may latch during that stock amp-gating sequence;
 * accepting it later is valid only after this fresh-event boundary. */
static inline enum venus_cs35_power_result
venus_cs35_muted_power_prepare_stop(const struct venus_cs35_power_io *io, struct venus_cs35_power_state *state)
{
	if (!state->active || state->poisoned || state->down_started) return VENUS_CS35_POWER_REFUSED;
	if (!io->guard(io->context) || !venus_cs35_power_events_clear(io))
		return venus_cs35_power_abort(io,state);
	state->down_began=io->now_us(io->context);
	state->down_started=true;
	return VENUS_CS35_POWER_OK;
}

static inline bool
venus_cs35_power_down_events_ready(const struct venus_cs35_power_io *io, const struct venus_cs35_power_state *state)
{
	uint32_t irq;
	if (!state->down_started) return venus_cs35_power_events_clear(io);
	if (io->now_us(io->context)-state->down_began>=250000u) return false;
	return io->read(io->context,VENUS_CS35_POWER_IRQ,&irq) &&
	    (irq & (VENUS_CS35_POWER_FAULTS | VENUS_CS35_POWER_UP)) == 0;
}

static inline enum venus_cs35_power_result
venus_cs35_muted_power_stop(const struct venus_cs35_power_io *io, struct venus_cs35_power_state *state)
{
	uint32_t power;
	if (state->poisoned) return VENUS_CS35_POWER_REFUSED;
	if (!state->active) return VENUS_CS35_POWER_OK;
	if (!io->guard(io->context) ||
	    !io->read(io->context, VENUS_CS35_PWR1, &power) || power != (state->power | 1u) ||
	    !venus_cs35_power_down_events_ready(io,state) ||
	    !venus_cs35_power_transition_events(io, state->power, VENUS_CS35_POWER_DOWN,
	        state->down_started ? 0u : VENUS_CS35_POWER_UP) ||
	    !venus_cs35_power_patch(io, 0x2f1aa3u) ||
	    !venus_cs35_power_write_checked(io, VENUS_CS35_PWR2, state->blocks) ||
	    !io->guard(io->context)) {
		return venus_cs35_power_abort(io, state);
	}
	state->active = false;
	state->down_started = false;
	state->restart_ready = true;
	return VENUS_CS35_POWER_OK;
}

/* Signed 11-bit volume in eighth-dB units. Physical output keeps an
 * additional minimum 24 dB attenuation below the stock analog-gain setting. */
static inline uint32_t
venus_cs35_output_target(uint32_t extra, uint32_t base)
{
	base=base<192u?192u:base>816u?816u:base;
	return extra>816u-base?816u:base+extra;
}

static inline uint32_t
venus_cs35_output_next(uint32_t current, uint32_t target, bool muted)
{
	if (muted) return 816u;
	if (current<target) return target-current>48u?current+48u:target;
	return current-target>48u?current-48u:target;
}

static inline uint32_t
venus_cs35_output_field(uint32_t below, bool muted)
{
	return muted?0x400u:(0x800u-venus_cs35_output_target(0,below))&0x7ffu;
}

#endif
