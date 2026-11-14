/*
 * Board-specific speaker paths for Redwood (AW88261) and Venus (CS35L41 B2).
 * Venus's digital session keeps GLOBAL_EN, output and boost disabled. Its retained
 * HALOs feed the system-audio path only with sound_venus_digital=1. The optional
 * measured-bench power sequencer is disabled in the default build.
 *
 * The original Redwood path below is selected only by the loader's board identity.
 *
 * Each speaker has an amplifier of its own, an Awinic AW88261 (chip ID 0x2113, a "smart PA"),
 * set up over I2C -- the bus of the SoC's serial engine 3, GPIO 12 data and 13 clock, on which
 * Android's device tree and its running kernel have these two and nothing else -- and fed by
 * the SoC's first TDM interface ("MI2S 0": GPIO 97 bit clock, 100 frame sync, 99 data out),
 * which the audio DSP drives (redwood_sound.c). In Android's tree the amplifier at 0x35 is
 * "sound-channel 0" and takes slot 0 of the frame (the left channel), the one at 0x34 is
 * channel 1 and takes slot 1; neither has a reset pin.
 *
 * Two uses:
 *
 *   speaker_probe=1    reads registers and nothing else: the pins, the power names (below)
 *   speaker_probe=2    ... and asks the amplifiers who they are (I2C, below)
 *
 *   redwood_speaker_prepare() and the rest (sys/redwood_audio.h), for boot-arg sound=3 of
 *   redwood_sound.c: the amplifiers set up and run exactly as Android's own driver does it
 *   (Xiaomi's redwood-s-oss kernel, techpack/audio/asoc/codecs/aw88263s_m20: aw882xx_device.c,
 *   aw882xx_init.c and aw882xx_pid_2113_reg.h; the phone runs its version 2.0.0), with the
 *   registers' values from Android's own configuration file -- /vendor/etc/aw882xx_acf.bin of
 *   the phone, which userland/build.sh puts into /fw of the system image from fw-audio/ and
 *   which is not in this repository. Its "Music" profile is what Android plays the speakers
 *   with (tinymix: aw_dev_0_prof, aw_dev_1_prof); 55 of its 57 registers read back from the
 *   idle chips under Android exactly as the file has them, and the other two as the driver
 *   leaves them (0x04 powered down and muted, 0x08 with the feedback off).
 *
 * What the driver does and this does the same way, per amplifier:
 *
 *   prepare  reads the chip ID and the four fuse registers; a soft reset (0x55aa into
 *            register 0); the profile's registers, every one but three as the file has them:
 *            0x04 keeps the power-down, amplifier power-down and mute bits as they are and
 *            sets the mute of the power stage ("ULS"), 0x08 has the feedback off, and 0x05 (the
 *            volume) is turned down to the lowest, -90 dB, right after; 0x50 gets the value the
 *            driver works out from the fuses (8192 * (1000 + icalk) / (1000 + vcalk)), which for
 *            this phone's two chips is the value Android left in them
 *   start    only while the TDM clock runs: power-down off, 2 ms, the PLL locked (status bits 0
 *            and 4); amplifier power-down off, 1 ms, the status as the driver wants it (bits 0, 4,
 *            8, 9); the feedback on; the two mutes off; the interrupts read away; the volume
 *            stays at the lowest
 *   volume   stepwise, 6 dB a step (the driver takes one every 100 us), to `below' eighths of a dB
 *            under Android's own level (the profile's 0x05, -2.1 dB, with Android's
 *            aw_dev_N_volume 0) and never above it
 *   stop     the power stage muted, the volume stepped down to the lowest, muted, the feedback
 *            off, 1 ms, amplifier power-down, power-down
 *
 * Every write is said in the log and read back; a value that does not read back as written, a
 * byte nobody acknowledges, or a fault in the status register (under-voltage, over-current of
 * the boost or the output, over-temperature, no clock, the watchdog, over-voltage) has both
 * amplifiers powered down at once (amplifier power-down and power-down, bits 1 and 0 of 0x04)
 * and nothing more done. What is not done, compared with Android: the DSP's Awinic algorithm,
 * which protects the speakers with their calibration (aw882xx_rx_switch); in its place the
 * samples go through redwood_guard.c on their way here. The driver's monitor is done
 * (redwood_speaker_monitor(), below): once a second the chips' temperature and the battery's
 * voltage, and the reduction that the file's table asks for at either.
 *
 * I2C is done by hand: a pin is either pulled low or let go (and the pull-ups raise it), at 100
 * kHz at most. For each register read or written the two pins are taken from the serial engine
 * (function qup03) as GPIOs and given back as they were found right after.
 *
 * Register layouts: the files above, and Linux's sound/soc/codecs/aw88261.c; pins:
 * drivers/pinctrl/qcom/pinctrl-yupik.c of the same kernel and the phone's own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <vm/memory_types.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/redwood_audio.h>

#include "redwood_venus_cs35_power.h"

#ifndef REDWOOD_VENUS_BENCH_POWER_BUILD
#define REDWOOD_VENUS_BENCH_POWER_BUILD 1 /* still requires explicit muted bench boot options */
#endif

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static vm_offset_t tlmm, gcc, cmd_db;
static uint32_t probe_level;
static struct {
	bool owned, reset_asserted;
	volatile bool cancelled;
	unsigned prepared, released, running;
	uint32_t state[2], pll[2];
} venus_digital;
static bool leaving;                    /* on a panic's way out: nothing sleeps, nothing is said but to the loader */

/* Every line to the loader's log as well: what is written to the amplifiers should be known even if the kernel dies. */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* Waits without sleeping (also with interrupts off, on a panic's way out). */
static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 12      /* the serial engine's first pin is the data line, the second the clock */
#define PIN_SCL                 13

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 12, "I2C data" }, { 13, "I2C clock" }, { 97, "MI2S 0 bit clock" }, { 100, "MI2S 0 word select" },
	{ 98, "MI2S 0 data 0" }, { 99, "MI2S 0 data 1" }, { 102, "the amplifier 0x35's interrupt" },
	{ 45, "the amplifier 0x34's interrupt" },
};

static void
show_pins(void)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("speaker: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") :
		    "input", io & 1);
	}
}

/* ---- I2C by hand ----------------------------------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most, I2C's standard mode; the bus does not mind slower */
}

/* A line is pulled low (the pin drives 0) or let go (the pin is an input and the pull-up raises it). */
static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

struct speaker_i2c_bus {
	unsigned sda, scl;
	uint32_t cfg[2], io[2];
	bool clock_failed;
};

static void
i2c_start(struct speaker_i2c_bus *bus)
{
	line(bus->sda, true);
	line(bus->scl, true);
	half_bit();
	line(bus->sda, false);
	half_bit();
	line(bus->scl, false);
	half_bit();
}

static void
i2c_stop(struct speaker_i2c_bus *bus)
{
	line(bus->sda, false);
	half_bit();
	line(bus->scl, true);
	half_bit();
	line(bus->sda, true);
	half_bit();
}

/* One bit out (the other side's bit in, if we let the data line go): the clock goes up and down once. */
static bool
i2c_bit(struct speaker_i2c_bus *bus, bool out)
{
	bool in;

	if (bus->clock_failed) {
		return true;
	}

	line(bus->sda, out);
	half_bit();
	line(bus->scl, true);
	for (unsigned stretched = 0; !level(bus->scl) && stretched < 100; stretched++) {
		half_bit();             /* (a device may hold the clock low until it is ready) */
	}
	half_bit();
	bus->clock_failed |= !level(bus->scl);
	in = level(bus->sda);
	line(bus->scl, false);
	return in;
}

/* Returns true if the byte was acknowledged. */
static bool
i2c_write(struct speaker_i2c_bus *bus, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit(bus, (byte >> bit) & 1);
	}
	bool nak = i2c_bit(bus, true);
	return !bus->clock_failed && !nak;
}

static uint8_t
i2c_read(struct speaker_i2c_bus *bus, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(bus, true));
	}
	i2c_bit(bus, !more);                 /* acknowledge all but the last byte */
	return byte;
}

/*
 * The two pins taken from the serial engine for one transaction, and given back as found. False
 * if the bus is not at rest: then nothing is driven.
 */
static bool
bus_take(struct speaker_i2c_bus *bus)
{
	bus->cfg[0] = rd(GPIO_CFG(bus->sda));
	bus->cfg[1] = rd(GPIO_CFG(bus->scl));
	bus->io[0] = rd(GPIO_IO(bus->sda));
	bus->io[1] = rd(GPIO_IO(bus->scl));
	line(bus->sda, true);
	line(bus->scl, true);
	half_bit();
	if (!level(bus->sda) || !level(bus->scl)) {
		wr(GPIO_IO(bus->sda), bus->io[0] & 2);
		wr(GPIO_IO(bus->scl), bus->io[1] & 2);
		wr(GPIO_CFG(bus->sda), bus->cfg[0]);
		wr(GPIO_CFG(bus->scl), bus->cfg[1]);
		return false;
	}
	return true;
}

static void
bus_give_back(struct speaker_i2c_bus *bus)
{
	wr(GPIO_IO(bus->sda), bus->io[0] & 2);
	wr(GPIO_IO(bus->scl), bus->io[1] & 2);
	wr(GPIO_CFG(bus->sda), bus->cfg[0]);
	wr(GPIO_CFG(bus->scl), bus->cfg[1]);
}

/* A 16-bit register of an Awinic amplifier, most significant byte first. Returns false if nobody answers. */
static bool
register_read(uint8_t address, uint8_t reg, uint16_t *value)
{
	struct speaker_i2c_bus pins = { .sda = PIN_SDA, .scl = PIN_SCL };
	struct speaker_i2c_bus *bus = &pins;
	bool answered;

	if (!bus_take(bus)) {
		say("speaker: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", bus->sda,
		    level(bus->sda), bus->scl, level(bus->scl));
		return false;
	}
	i2c_start(bus);
	answered = i2c_write(bus, (uint8_t)(address << 1)) && i2c_write(bus, reg);
	if (answered) {
		i2c_start(bus);
		answered = i2c_write(bus, (uint8_t)(address << 1 | 1));
	}
	if (answered) {
		*value = (uint16_t)(i2c_read(bus, true) << 8);
		*value |= i2c_read(bus, false);
	}
	i2c_stop(bus);
	bus_give_back(bus);
	return answered && !bus->clock_failed;
}

static bool
register_write(uint8_t address, uint8_t reg, uint16_t value)
{
	struct speaker_i2c_bus pins = { .sda = PIN_SDA, .scl = PIN_SCL };
	struct speaker_i2c_bus *bus = &pins;
	bool answered;

	if (!bus_take(bus)) {
		say("speaker: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", bus->sda,
		    level(bus->sda), bus->scl, level(bus->scl));
		return false;
	}
	i2c_start(bus);
	answered = i2c_write(bus, (uint8_t)(address << 1)) && i2c_write(bus, reg) && i2c_write(bus, (uint8_t)(value >> 8)) &&
	    i2c_write(bus, (uint8_t)value);
	i2c_stop(bus);
	bus_give_back(bus);
	return answered && !bus->clock_failed;
}

static bool
venus_cs35_bus_finish(struct speaker_i2c_bus *bus)
{
	i2c_stop(bus);
	bus_give_back(bus);
	if (rd(GPIO_CFG(bus->sda)) != bus->cfg[0] || rd(GPIO_CFG(bus->scl)) != bus->cfg[1] ||
	    (rd(GPIO_IO(bus->sda)) & 2) != (bus->io[0] & 2) || (rd(GPIO_IO(bus->scl)) & 2) != (bus->io[1] & 2)) {
		say("venus cs35: I2C pins did not restore\n");
		return false;
	}
	if (bus->clock_failed) {
		say("venus cs35: GPIO37 clock did not rise\n");
		return false;
	}
	return true;
}

/* CS35L41 uses a four-byte, big-endian register pointer followed by a repeated START. */
static bool
venus_cs35_read(uint8_t address, uint32_t reg, uint8_t *data, uint32_t length)
{
	struct speaker_i2c_bus bus = { .sda = 36, .scl = 37 };
	bool answered;

	if (!bus_take(&bus)) {
		say("venus cs35: GPIO36/37 bus did not idle high\n");
		return false;
	}
	i2c_start(&bus);
	answered = i2c_write(&bus, (uint8_t)(address << 1));
	for (int shift = 24; shift >= 0 && answered; shift -= 8) {
		answered = i2c_write(&bus, (uint8_t)(reg >> shift));
	}
	if (answered) {
		i2c_start(&bus);
		answered = i2c_write(&bus, (uint8_t)(address << 1 | 1));
	}
	if (answered) {
		for (uint32_t i = 0; i < length; i++) {
			data[i] = i2c_read(&bus, i + 1 != length);
		}
	}
	if (!venus_cs35_bus_finish(&bus)) {
		return false;
	}
	if (!answered) {
		say("venus cs35: 0x%02x I2C NACK reading %#x\n", address, reg);
	}
	return answered;
}

static bool
venus_cs35_register_read(uint8_t address, uint32_t reg, uint32_t *value)
{
	uint8_t bytes[4];

	if (!venus_cs35_read(address, reg, bytes, sizeof(bytes))) {
		return false;
	}
	*value = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
	return true;
}

/* Only the boost ceiling and a HALO stop can be written by this diagnostic. */
#define VENUS_CS35_HALO_CORE_CTRL 0x02bc1000u
#define VENUS_CS35_BOOST_PEAK_REG 0x3808u
#define VENUS_CS35_BOOST_PEAK_MASK 0x7fu
#define VENUS_CS35_BOOST_PEAK_STOCK 0x40u /* Venus DT: 4000 mA inductor peak setting */

static bool
venus_cs35_write(uint8_t address, uint32_t reg, const uint8_t *data, uint32_t length)
{
	struct speaker_i2c_bus bus = { .sda = 36, .scl = 37 };

	if (!bus_take(&bus)) {
		return false;
	}
	i2c_start(&bus);
	bool answered = i2c_write(&bus, (uint8_t)(address << 1));
	for (int shift = 24; shift >= 0 && answered; shift -= 8) {
		answered = i2c_write(&bus, (uint8_t)(reg >> shift));
	}
	for (uint32_t i = 0; i < length && answered; i++) {
		answered = i2c_write(&bus, data[i]);
	}
	return venus_cs35_bus_finish(&bus) && answered;
}

static bool
venus_cs35_control_write(uint8_t address, uint32_t reg, uint32_t value)
{
	uint8_t bytes[] = { value >> 24, value >> 16, value >> 8, value };

	return (reg == VENUS_CS35_BOOST_PEAK_REG || (reg == VENUS_CS35_HALO_CORE_CTRL && value == 0)) &&
	       venus_cs35_write(address, reg, bytes, sizeof(bytes));
}

/* The stock Venus probe checks OTP_BOOT_DONE and OTP_BOOT_ERR in these IRQ registers. */
#define VENUS_CS35_IRQ_STATUS1 0x10010u
#define VENUS_CS35_IRQ_STATUS3 0x10018u
#define VENUS_CS35_IRQ_STATUS4 0x1001cu
#define VENUS_CS35_OTP_BOOT_DONE 0x2u
#define VENUS_CS35_OTP_BOOT_ERR 0x80000000u

static bool
venus_cs35_check_otp(uint8_t address)
{
	uint32_t irq1, irq3, irq4, core;

	if (!venus_cs35_register_read(address, VENUS_CS35_IRQ_STATUS3, &irq3) ||
	    !venus_cs35_register_read(address, VENUS_CS35_IRQ_STATUS4, &irq4) ||
	    !venus_cs35_register_read(address, VENUS_CS35_IRQ_STATUS1, &irq1) ||
	    !venus_cs35_register_read(address, VENUS_CS35_HALO_CORE_CTRL, &core)) {
		return false;
	}
	bool ready = (irq4 & VENUS_CS35_OTP_BOOT_DONE) != 0 && (irq3 & VENUS_CS35_OTP_BOOT_ERR) == 0;

	say("venus cs35: 0x%02x OTP %s irq1=%#x irq3=%#x irq4=%#x HALO core=%#x\n",
	    address, ready ? "ready" : "unavailable", irq1, irq3, irq4, core);
	return ready;
}

/* Snapshot boost settings without changing the converter or clearing faults. */
static bool
venus_cs35_check_boost(uint8_t address, bool cap_boost)
{
	uint32_t peak, ramp, coeff, slope;

	if (!venus_cs35_register_read(address, VENUS_CS35_BOOST_PEAK_REG, &peak) ||
	    !venus_cs35_register_read(address, 0x380c, &ramp) ||
	    !venus_cs35_register_read(address, 0x3810, &coeff) ||
	    !venus_cs35_register_read(address, 0x3814, &slope)) {
		return false;
	}
	say("venus cs35: 0x%02x boost peak=%#x ramp=%#x coeff=%#x slope=%#x\n",
	    address, peak, ramp, coeff, slope);
	if (!cap_boost) {
		return true;
	}
	uint32_t power, readback;
	uint32_t expected = peak;

	if (!venus_cs35_register_read(address, 0x2014, &power) || (power & 1) != 0) {
		return false;
	}
	if ((peak & VENUS_CS35_BOOST_PEAK_MASK) > VENUS_CS35_BOOST_PEAK_STOCK) {
		expected = (peak & ~VENUS_CS35_BOOST_PEAK_MASK) | VENUS_CS35_BOOST_PEAK_STOCK;
		if (!venus_cs35_control_write(address, VENUS_CS35_BOOST_PEAK_REG, expected)) {
			return false;
		}
	}
	if (!venus_cs35_register_read(address, VENUS_CS35_BOOST_PEAK_REG, &readback) ||
	    readback != expected ||
	    !venus_cs35_register_read(address, 0x2014, &power) || (power & 1) != 0) {
		return false;
	}
	say("venus cs35: 0x%02x boost peak %#x -> %#x, GLOBAL_EN clear\n", address, peak, readback);
	return true;
}

/* Check the firmware memory apertures with HALO stopped and the output disabled. */
static bool
venus_cs35_halo_preflight(uint8_t address)
{
	uint32_t power, core, xm, ym, pm, coeff;
	uint32_t coeff_reg = address == 0x40 ? 0x03401480u : 0x034014d0u;

	if (!venus_cs35_register_read(address, 0x2014, &power) || (power & 1) != 0 ||
	    !venus_cs35_control_write(address, VENUS_CS35_HALO_CORE_CTRL, 0) ||
	    !venus_cs35_register_read(address, VENUS_CS35_HALO_CORE_CTRL, &core) || core != 0 ||
	    !venus_cs35_register_read(address, 0x02000000u, &xm) ||
	    !venus_cs35_register_read(address, 0x02c00000u, &ym) ||
	    !venus_cs35_register_read(address, 0x03800000u, &pm) ||
	    !venus_cs35_register_read(address, coeff_reg, &coeff) ||
	    !venus_cs35_register_read(address, 0x2014, &power) || (power & 1) != 0) {
		say("venus cs35: 0x%02x HALO stop or memory aperture check failed\n", address);
		return false;
	}
	say("venus cs35: 0x%02x HALO stopped, memory XM=%#x YM=%#x PM=%#x coeff=%#x GLOBAL_EN clear\n",
	    address, xm, ym, pm, coeff);
	return true;
}

static bool
venus_cs35_halo_stopped(uint8_t address)
{
	uint32_t power, core;

	return venus_cs35_register_read(address, 0x2014, &power) && (power & 1) == 0 &&
	       venus_cs35_register_read(address, VENUS_CS35_HALO_CORE_CTRL, &core) && core == 0;
}

static bool
venus_cs35_write_word_verified(uint8_t address, uint32_t reg, uint32_t word)
{
	uint8_t bytes[] = { word >> 24, word >> 16, word >> 8, word };
	uint32_t value = 0;
	bool wrote = venus_cs35_write(address, reg, bytes, sizeof(bytes));
	bool read = wrote && venus_cs35_register_read(address, reg, &value);
	if (!read || value != word)
		say("venus cs35: 0x%02x register verify reg=%#x expected=%#x actual=%#x wrote=%u read=%u\n",
		    address,reg,word,value,wrote,read);
	return read && value == word;
}

static bool
venus_cs35_stage_rates(uint8_t address)
{
	static const uint32_t first[] = { 0x02b80080u, 0x02b80280u };

	for (unsigned block = 0; block < sizeof(first) / sizeof(first[0]); block++) {
		for (unsigned slot = 0; slot < 8; slot++) {
			uint32_t reg = first[block] + 8 * slot;
			uint32_t rate;

			if (!venus_cs35_register_read(address, reg, &rate) ||
			    !venus_cs35_write_word_verified(address, reg, (rate & ~0x1fu) | 1u)) {
				say("venus cs35: 0x%02x HALO rate readback failed at reg=%#x\n", address, reg);
				return false;
			}
		}
	}
	say("venus cs35: 0x%02x HALO eight RX and eight TX rate fields verified\n", address);
	return venus_cs35_halo_stopped(address);
}

static bool
venus_cs35_stage_calibration(uint8_t address)
{
	/* The stock HALO ID and CSPL algorithm table are in unpacked XM. */
	static const struct {
		uint32_t reg, value;
	} identity[] = {
		{ 0x0280000cu, 0x400a4u },
		{ 0x02800010u, 0x0d00u },
		{ 0x02800024u, 1 },
		{ 0x02800028u, 0xcdu },
		{ 0x0280002cu, 0x1d2100u },
		{ 0x02800030u, 0x94u },
	};
	/* This unit has no per-device persist calibration. Use its Android defaults. */
	static const struct {
		uint32_t reg, value;
	} calibration[] = {
		{ 0x0280026cu, 0x20c8u }, /* CAL_R */
		{ 0x02800274u, 1 },      /* CAL_STATUS */
		{ 0x02800278u, 0x20c9u }, /* CAL_CHECKSUM */
	};
	uint32_t value;

	if (!venus_cs35_halo_stopped(address)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(identity) / sizeof(identity[0]); i++) {
		if (!venus_cs35_register_read(address, identity[i].reg, &value)) {
			return false;
		}
		if (value != identity[i].value) {
			say("venus cs35: 0x%02x HALO identity reg=%#x got=%#x expected=%#x\n",
			    address, identity[i].reg, value, identity[i].value);
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(calibration) / sizeof(calibration[0]); i++) {
		if (!venus_cs35_write_word_verified(address, calibration[i].reg, calibration[i].value)) {
			say("venus cs35: 0x%02x HALO calibration readback failed at reg=%#x\n",
			    address, calibration[i].reg);
			return false;
		}
	}
	if (!venus_cs35_halo_stopped(address)) {
		return false;
	}
	say("venus cs35: 0x%02x HALO FW 0x400a4 v0.13.0, CSPL 0xcd, default calibration verified\n", address);
	return true;
}

static bool
venus_cs35_stage_image(uint8_t address, const struct redwood_venus_cs35_image *image)
{
	/* End addresses are exclusive. Keep packed transfers aligned to complete groups. */
	static const struct {
		uint32_t start, end, alignment;
	} windows[] = {
		{ 0x02000000u, 0x02002ff4u, 12 },
		{ 0x02c00000u, 0x02c017f4u, 12 },
		{ 0x03400000u, 0x03401ff8u, 4 },
		{ 0x03800000u, 0x03804fecu, 20 },
	};
	uint8_t readback[60]; /* Multiple of every window's transfer alignment. */

	if (image->count != sizeof(image->regions) / sizeof(image->regions[0])) {
		return false;
	}
	for (unsigned i = 0; i < image->count; i++) {
		const struct redwood_venus_cs35_region *region = &image->regions[i];
		bool bounded = false;

		for (unsigned j = 0; j < sizeof(windows) / sizeof(windows[0]); j++) {
			if (region->reg >= windows[j].start && region->reg < windows[j].end &&
			    region->length <= windows[j].end - region->reg &&
			    (region->reg - windows[j].start) % windows[j].alignment == 0 &&
			    region->length % windows[j].alignment == 0) {
				bounded = true;
				break;
			}
		}
		if (!bounded || region->data == NULL || region->length == 0 || !venus_cs35_halo_stopped(address)) {
			return false;
		}
		for (uint32_t offset = 0; offset < region->length; ) {
			if (venus_digital.owned && venus_digital.cancelled) return false;
			uint32_t length = MIN((uint32_t)sizeof(readback), region->length - offset);
			uint32_t reg = region->reg + offset;
			const uint8_t *data = region->data + offset;

			if (!venus_cs35_write(address, reg, data, length) ||
			    !venus_cs35_read(address, reg, readback, length) ||
			    memcmp(data, readback, length) != 0) {
				say("venus cs35: 0x%02x HALO write/readback failed at reg=%#x bytes=%u\n", address, reg, length);
				return false;
			}
			offset += length;
		}
		say("venus cs35: 0x%02x HALO staged reg=%#x bytes=%u, readback matched\n",
		    address, region->reg, region->length);
	}
	return venus_cs35_halo_stopped(address);
}

/* Match the Venus driver's SRAM-bank layout and restrict HALO's register windows. */
static bool
venus_cs35_halo_mpu(uint8_t address)
{
	static const uint32_t closed_windows[] = {
		0x43008, 0x4300c, 0x43014, 0x43018, 0x4301c,
		0x43020, 0x43024, 0x4302c, 0x43030, 0x43034,
		0x43038, 0x4303c, 0x43044, 0x43048, 0x4304c,
		0x43050, 0x43054, 0x4305c,
	};
	const uint32_t base = 0x02b80000u, lock = base + 0x43140u;
	const uint8_t key1[] = { 0, 0, 0x55, 0x55 }, key2[] = { 0, 0, 0xaa, 0xaa }, zero[4] = { 0 };
	uint32_t xm, ym, xbank, ybank;

	if (!venus_cs35_register_read(address, 0x025e0010u, &xm) || xm != 0x1000 ||
	    !venus_cs35_register_read(address, 0x025e0018u, &ym) || ym != 0x800 ||
	    !venus_cs35_register_read(address, 0x025e0038u, &xbank) || xbank != 0x800 ||
	    !venus_cs35_register_read(address, 0x025e003cu, &ybank) || ybank != 0x800) {
		say("venus cs35: 0x%02x HALO SRAM layout check failed\n", address);
		return false;
	}
	bool ready = venus_cs35_write(address, lock, key1, sizeof(key1)) &&
	    venus_cs35_write(address, lock, key2, sizeof(key2)) &&
	    venus_cs35_write_word_verified(address, base + 0x43000u, 3) &&
	    venus_cs35_write_word_verified(address, base + 0x43004u, 1);
	for (unsigned i = 0; ready && i < sizeof(closed_windows) / sizeof(closed_windows[0]); i++) {
		ready = venus_cs35_write_word_verified(address, base + closed_windows[i], 0);
	}
	/* The key register is distinct from the bank masks. Always attempt to relock. */
	return venus_cs35_write(address, lock, zero, sizeof(zero)) && ready;
}

static bool
venus_cs35_halo_outputs_off(uint8_t address)
{
	uint32_t power, blocks, irq;

	return venus_cs35_register_read(address, 0x2014, &power) && (power & 1) == 0 &&
	       venus_cs35_register_read(address, 0x2018, &blocks) && (blocks & 0x31u) == 0 &&
	       venus_cs35_register_read(address, VENUS_CS35_IRQ_STATUS1, &irq) && (irq & 0x800281c0u) == 0;
}

enum venus_cs35_cspl_command {
	VENUS_CS35_CSPL_PAUSE = 1,
	VENUS_CS35_CSPL_RESUME = 2,
};

static bool venus_output_command_allowed(uint8_t address);

/* Use the vendor's IRQ2 mailbox route and require a fresh IRQ1 acknowledgement. */
static bool
venus_cs35_cspl_command(uint8_t address, enum venus_cs35_cspl_command command)
{
	const uint32_t irq1_status2 = 0x10014u, irq2_status2 = 0x10814u, irq2_mask2 = 0x10914u;
	const uint32_t ack_bit = 0x00200000u, route_bit = 0x00100000u;
	const uint8_t ack_bytes[4] = { 0, 0x20, 0, 0 }, route_bytes[4] = { 0, 0x10, 0, 0 };
	const uint8_t request[4] = { 0, 0, 0, command };
	uint32_t expected = command == VENUS_CS35_CSPL_PAUSE ? 1u : 0u;
	uint32_t mask, irq = 0, mailbox = UINT32_MAX;
	bool acknowledged = false;

	if (!venus_output_command_allowed(address) ||
	    !venus_cs35_register_read(address, irq2_mask2, &mask)) {
		return false;
	}
	if (venus_cs35_write(address, irq2_status2, route_bytes, sizeof(route_bytes)) &&
	    venus_cs35_write(address, irq1_status2, ack_bytes, sizeof(ack_bytes)) &&
	    venus_cs35_register_read(address, irq1_status2, &irq) && (irq & ack_bit) == 0 &&
	    venus_cs35_write_word_verified(address, irq2_mask2, mask & ~route_bit) &&
	    venus_cs35_write(address, 0x13020u, request, sizeof(request))) {
		for (unsigned attempt = 0; attempt < 5; attempt++) {
			spin_us(1000);
			if (!venus_output_command_allowed(address) ||
			    !venus_cs35_register_read(address, irq1_status2, &irq)) {
				break;
			}
			if ((irq & ack_bit) != 0) {
				acknowledged = venus_cs35_register_read(address, 0x13004u, &mailbox) && mailbox == expected;
				break;
			}
		}
	}
	bool cleared = venus_cs35_write(address, irq1_status2, ack_bytes, sizeof(ack_bytes));
	bool restored = venus_cs35_write_word_verified(address, irq2_mask2, mask);

	say("venus cs35: 0x%02x CSPL %s ack=%#x mailbox=%#x mask %s\n",
	    address, command == VENUS_CS35_CSPL_PAUSE ? "PAUSE" : "RESUME",
	    irq & ack_bit, mailbox, restored ? "restored" : "not restored");
	return acknowledged && cleared && restored && venus_output_command_allowed(address);
}

static bool venus_cs35_restore_reset(unsigned reset);
static bool venus_cs35_bench_reset_attempted;

static bool
venus_cs35_bench_power_requested(void)
{
	uint32_t enable = 0;

	return PE_parse_boot_argn("adsp_venus_afe_bench_power", &enable, sizeof(enable)) && enable == 1;
}

static bool venus_cs35_tdm_pll(uint8_t address, uint32_t state_reg, const struct redwood_venus_cs35_live_pcm *live);

/* Stock wm_halo_clear_stream_arb (vendor module fbf8..fd18), before core EN.
 * Clear boot-ROM stream descriptors so firmware can construct its 96k multirate graph. */
/* Vendor writes FC; this silicon reads TX/RX descriptor reset as 3C.
 * Check the implemented low six bits instead of reserved upper bits. */
static bool
venus_cs35_arb_disable_stream(uint8_t address, uint32_t reg)
{
    const uint8_t bytes[4]={0,0,0,0xfc};
    uint32_t value=0;
    bool ready=venus_cs35_write(address,reg,bytes,sizeof(bytes)) &&
        venus_cs35_register_read(address,reg,&value) && (value&0x3fu)==0x3cu;
    if (!ready) say("venus cs35: 0x%02x arbiter disable reg=%#x got=%#x failed\n",address,reg,value);
    return ready;
}

/* INTPCTL_IRQ_FLUSH is an action register: vendor writes24 ones, readback is zero. */
static bool
venus_cs35_arb_flush_irq(uint8_t address)
{
    const uint8_t flush[4]={0,0xff,0xff,0xff};
    uint32_t value;
    return venus_cs35_write(address,0x02bc6020u,flush,sizeof(flush)) &&
        venus_cs35_register_read(address,0x02bc6020u,&value) && value==0;
}

static bool
venus_cs35_halo_clear_stream_arb(uint8_t address)
{
    if (!venus_cs35_halo_stopped(address) || !venus_cs35_halo_outputs_off(address)) return false;
    for (unsigned i=0; i<6; i++) {
        uint32_t reg=0x02bc5000u+16*i, value;
        if (!venus_cs35_register_read(address,reg,&value) ||
            !venus_cs35_write_word_verified(address,reg,value&~1u)) return false;
    }
    for (unsigned i=0; i<6; i++) {
        for (unsigned word=0; word<3; word++)
            if (!venus_cs35_write_word_verified(address,0x02bc5000u+16*i+4*word,0)) return false;
    }
    for (unsigned i=0; i<8; i++) {
        if (!venus_cs35_arb_disable_stream(address,0x02bc5200u+8*i) ||
            !venus_cs35_arb_disable_stream(address,0x02bc5400u+8*i) ||
            !venus_cs35_arb_disable_stream(address,0x02bc5600u+16*i) ||
            !venus_cs35_write_word_verified(address,0x02bc5604u+16*i,0)) return false;
    }
    bool ready=venus_cs35_arb_flush_irq(address) &&
        venus_cs35_halo_stopped(address) && venus_cs35_halo_outputs_off(address);
    say("venus cs35: 0x%02x stock stream arbiter clear %s\n",address,ready?"verified":"failed");
    return ready;
}

static bool venus_cs35_tdm_receive(uint8_t address);

/* The firmware latches GLOBAL_FS at its first boot, not on a later PLL handoff. */
static bool
venus_cs35_halo_rate_verified(uint8_t address, uint32_t state_reg)
{
    uint32_t fs=0, frames=0, rate=0, bands=0;
    bool ready=venus_cs35_register_read(address,0x2c0cu,&fs) && (fs&0x1fu)==4 &&
        venus_cs35_register_read(address,state_reg+8,&frames) && frames==64 &&
        venus_cs35_register_read(address,state_reg+28,&rate) && rate==96000 &&
        venus_cs35_register_read(address,state_reg+32,&bands) && bands==2;
    say("venus cs35: 0x%02x HALO sample clock global=%u, firmware=%u, block=%u, bands=%u %s\n",
        address,fs&0x1fu,rate,frames,bands,ready?"verified":"failed");
    return ready;
}

/* Run only the DSP, with both output enables and the boost mode cleared. */
static bool
venus_cs35_halo_run_test(uint8_t address, bool check_tdm, const struct redwood_venus_cs35_live_pcm *live)
{
	uint32_t blocks, xm_base, state = 0, heartbeat = 0, mailbox = 0, error = 0;
	bool ready = false;

	if (!venus_cs35_halo_stopped(address) ||
	    !venus_cs35_register_read(address, 0x2018, &blocks) ||
	    !venus_cs35_write_word_verified(address, 0x2018, blocks & ~0x31u) ||
	    !venus_cs35_halo_outputs_off(address) ||
	    !venus_cs35_register_read(address, 0x02800014u, &xm_base) ||
	    xm_base < 0x11 || xm_base >= 0x1000 - 1 ||
	    !venus_cs35_halo_clear_stream_arb(address) ||
	    !venus_cs35_halo_mpu(address) ||
	    (check_tdm && !venus_cs35_tdm_receive(address))) {
		say("venus cs35: 0x%02x HALO execution prerequisites failed\n", address);
		return false;
	}
	uint32_t state_reg = 0x02800000u + 4 * xm_base;

	if (venus_cs35_register_read(address, state_reg, &state) && state != 2 &&
	    venus_cs35_write_word_verified(address, VENUS_CS35_HALO_CORE_CTRL, 1)) {
		for (unsigned attempt = 0; attempt < 8; attempt++) {
			if (!venus_cs35_halo_outputs_off(address) ||
			    !venus_cs35_register_read(address, state_reg, &state)) {
				break;
			}
			if (state == 2) {
				ready = venus_cs35_register_read(address, state_reg + 4, &heartbeat) &&
				    venus_cs35_register_read(address, 0x00013004u, &mailbox) && mailbox <= 1 &&
				    venus_cs35_register_read(address, 0x0280025cu, &error) && error == 0 &&
				    (!check_tdm || venus_cs35_halo_rate_verified(address,state_reg)) &&
				    venus_cs35_cspl_command(address, VENUS_CS35_CSPL_PAUSE);
				break;
			}
			spin_us(2000);
		}
	}
	if (ready && check_tdm) {
		ready = venus_cs35_tdm_pll(address, state_reg, live);
	}
	if (ready && venus_digital.owned && !venus_digital.cancelled) {
		unsigned index = address == 0x40 ? 0 : 1;
		venus_digital.state[index] = state_reg;
		venus_digital.prepared |= 1u << index;
		say("venus cs35: 0x%02x digital HALO retained paused, state reg=%#x\n", address, state_reg);
		return true;
	}
	if (address == 0x40 && venus_cs35_bench_power_requested() && venus_cs35_bench_reset_attempted) {
		return false; /* No I2C cleanup after an emergency reset, even if its readback failed. */
	}
	bool stopped = venus_cs35_control_write(address, VENUS_CS35_HALO_CORE_CTRL, 0) &&
	    venus_cs35_halo_stopped(address) && venus_cs35_halo_outputs_off(address);

	say("venus cs35: 0x%02x HALO state=%#x heartbeat=%#x mailbox=%#x error=%#x, execution %s, stop %s\n",
	    address, state, heartbeat, mailbox, error, ready ? "verified" : "unconfirmed",
	    stopped ? "verified" : "unconfirmed");
	return ready && stopped;
}

#define VENUS_CS35_DIG_VOL 0x6000u
#define VENUS_CS35_DIG_VOL_MASK (0x7ffu << 3)
#define VENUS_CS35_DIG_VOL_MUTE (0x4cfu << 3)
#define VENUS_CS35_GAIN 0x6c04u
#define VENUS_CS35_PCM_GAIN_MASK 0x3e0u

static bool
venus_cs35_mute_verified(uint8_t address)
{
	uint32_t volume, gain;

	return venus_cs35_register_read(address, VENUS_CS35_DIG_VOL, &volume) &&
	       (volume & VENUS_CS35_DIG_VOL_MASK) == VENUS_CS35_DIG_VOL_MUTE &&
	       venus_cs35_register_read(address, VENUS_CS35_GAIN, &gain) &&
	       (gain & VENUS_CS35_PCM_GAIN_MASK) == 0;
}

/* Configure the codec's TDM receive side. Feedback transmitters stay disabled. */
static bool
venus_cs35_tdm_receive(uint8_t address)
{
	uint32_t slot = address == 0x40 ? 1 : 0;
    uint32_t stock_routes=0;
    PE_parse_boot_argn("venus_output_stock_routes",&stock_routes,sizeof(stock_routes));
	const struct {
		uint32_t reg, mask, value;
	} fields[] = {
		{ VENUS_CS35_DIG_VOL, VENUS_CS35_DIG_VOL_MASK, VENUS_CS35_DIG_VOL_MUTE },
		{ VENUS_CS35_GAIN, VENUS_CS35_PCM_GAIN_MASK, 0 },
		{ 0x2c0c, 0x1f, 4 },                              /* global sample rate: 96 kHz */
		{ 0x4808, 0xffff0755u, 0x20200000u },              /* 32-bit slots, DSP_A, clock consumer */
		{ 0x4840, 0x3f, 24 },                             /* receive sample width */
		{ 0x4820, 0x3f3f, slot | ((slot ^ 1u) << 8) },    /* RX1 = this amp's slot */
		{ 0x4c00, 0x7f, 0x32 },                          /* protected DSP feeds DAC */
		{ 0x4c40, 0x7f, 8 },                             /* ASP RX1 feeds DSP */
		{ 0x4c44, 0x7f, 8 },
		{ 0x4800, 0x3000f, 0x10000 },                     /* RX1 only */
	};
	if (!venus_cs35_halo_outputs_off(address)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		uint32_t value;
		if (!venus_cs35_register_read(address, fields[i].reg, &value) ||
		    !venus_cs35_write_word_verified(address, fields[i].reg,
		        (value & ~fields[i].mask) | fields[i].value)) {
			say("venus cs35: 0x%02x TDM RX setup failed at reg=%#x\n", address, fields[i].reg);
			return false;
		}
	}
    if (stock_routes==1) {
        /* Active Android readback; unused slots tri-state, TX1/2 occupy separate slots0/1. */
        const struct {uint32_t reg,mask,value;} stock[] = {
            {0x480cu,3u,3u},
            {0x4810u,0x3f3fu,0x0100u},
            {0x4820u,0x3f3fu,slot | ((address==0x40?4u:2u)<<8)},
            {0x4c20u,0x7fu,address==0x40?0u:0x32u},
            {0x4c24u,0x7fu,address==0x40?0x32u:0u},
            {0x4c44u,0x7fu,address==0x40?8u:0u},
            {0x4c50u,0x7fu,0x28u},
            {0x6404u,0xffffffffu,0x0200530eu},
            {0x4800u,0x3000fu,address==0x40?0x10002u:0x10001u},
        };
        for(unsigned i=0;i<sizeof(stock)/sizeof(stock[0]);i++) {
            uint32_t value;
            if (!venus_cs35_register_read(address,stock[i].reg,&value) ||
                !venus_cs35_write_word_verified(address,stock[i].reg,
                    (value&~stock[i].mask)|stock[i].value)) return false;
        }
        if (address==0x42 && !venus_cs35_write_word_verified(address,0x028006a8u,1)) return false;
        say("venus cs35: 0x%02x Android signal routes verified, separate TX slots and VP monitor enabled\n",address);
    }
	if (!venus_cs35_mute_verified(address) || !venus_cs35_halo_outputs_off(address)) {
		return false;
	}
	say("venus cs35: 0x%02x TDM RX slot %u, 96 kHz 24-in-32 verified, PCM mute and minimum gain confirmed\n",
	    address, slot);
	return true;
}

/* During the paused handoff, any lost prerequisite ends the probe immediately. */
static bool
venus_cs35_watch_paused(uint8_t address, uint32_t state_reg)
{
	uint32_t inject = 0;
	PE_parse_boot_argn("adsp_venus_afe_test_clock_loss", &inject, sizeof(inject));
	for (unsigned i = 0; i < 4; i++) {
		uint32_t state, error, mailbox, irq, peak;

		if (!venus_cs35_halo_outputs_off(address) || !venus_cs35_mute_verified(address) ||
		    !venus_cs35_register_read(address, 0x10098u, &irq) ||
		    !venus_cs35_register_read(address, state_reg, &state) || state != 2 ||
		    !venus_cs35_register_read(address, 0x0280025cu, &error) || error != 0 ||
		    !venus_cs35_register_read(address, 0x13004u, &mailbox) || mailbox != 1 ||
		    !venus_cs35_register_read(address, VENUS_CS35_BOOST_PEAK_REG, &peak) ||
		    (peak & VENUS_CS35_BOOST_PEAK_MASK) > VENUS_CS35_BOOST_PEAK_STOCK) {
			say("venus cs35: 0x%02x paused safety check failed at poll %u\n", address, i + 1);
			return false;
		}
		if (inject == 1 && address == 0x42 && i == 1) {
			irq &= ~2u;
			say("venus cs35: 0x42 injected PLL unlock observation\n");
		}
		if ((irq & 2u) == 0) {
			say("venus cs35: 0x%02x paused safety check lost PLL lock at poll %u\n", address, i + 1);
			return false;
		}
		spin_us(2000);
	}
	say("venus cs35: 0x%02x paused safety check passed (4 polls)\n", address);
	return true;
}

static bool
venus_cs35_runtime_safe(uint8_t address, uint32_t state_reg, uint32_t expected_mailbox)
{
	uint32_t irq, state, error, mailbox, peak;

	return tlmm != 0 && (address == 0x40 || address == 0x42) &&
	       state_reg >= 0x02800044u && state_reg < 0x02803ffcu && (state_reg & 3u) == 0 &&
	       venus_cs35_halo_outputs_off(address) && venus_cs35_mute_verified(address) &&
	       venus_cs35_register_read(address, 0x10098u, &irq) && (irq & 2u) != 0 &&
	       venus_cs35_register_read(address, state_reg, &state) && state == 2 &&
	       venus_cs35_register_read(address, 0x0280025cu, &error) && error == 0 &&
	       venus_cs35_register_read(address, 0x13004u, &mailbox) && mailbox == expected_mailbox &&
	       venus_cs35_register_read(address, VENUS_CS35_BOOST_PEAK_REG, &peak) &&
	       (peak & VENUS_CS35_BOOST_PEAK_MASK) <= VENUS_CS35_BOOST_PEAK_STOCK;
}

bool
redwood_speaker_venus_live_safe(uint8_t address, uint32_t state_reg)
{
	return venus_cs35_runtime_safe(address, state_reg, 0);
}

/* Exercise the digital protection mailbox while the output and boost remain inhibited. */
static bool
venus_cs35_test_cspl_cycle(uint8_t address, uint32_t state_reg, const struct redwood_venus_cs35_live_pcm *live)
{
	uint32_t enable = 0;

	if (live == NULL && (!PE_parse_boot_argn("adsp_venus_afe_test_cspl_cycle", &enable, sizeof(enable)) || enable != 1)) {
		return true;
	}
	bool resumed = venus_cs35_cspl_command(address, VENUS_CS35_CSPL_RESUME) &&
	    redwood_speaker_venus_live_safe(address, state_reg);
	bool transferred = resumed && (live == NULL || live->transfer(live->context, address, state_reg));
	if (transferred && live != NULL) {
		transferred = redwood_speaker_venus_live_safe(address, state_reg);
	}
	/* Send PAUSE even if a RESUME acknowledgement was missed. The caller also stops HALO. */
	bool paused = venus_cs35_cspl_command(address, VENUS_CS35_CSPL_PAUSE);
	say("venus cs35: 0x%02x output-inhibited CSPL cycle %s\n", address, transferred && paused ? "passed" : "failed");
	return transferred && paused;
}

/* Clear stale power-transition events before handing a muted path to a bench test. */
static bool
venus_cs35_bench_preflight(uint8_t address)
{
	const uint32_t power_events = 0x01800000u; /* PUP_DONE and PDN_DONE, W1C */
	const uint8_t clear_events[] = { 0x01, 0x80, 0, 0 };
	uint32_t requested = 0, before, after;

	if (!PE_parse_boot_argn("adsp_venus_afe_bench_preflight", &requested, sizeof(requested)) || requested != 1) {
		return true;
	}
	if (!venus_cs35_halo_outputs_off(address) ||
	    !venus_cs35_register_read(address, VENUS_CS35_IRQ_STATUS1, &before) ||
	    !venus_cs35_write(address, VENUS_CS35_IRQ_STATUS1, clear_events, sizeof(clear_events)) ||
	    !venus_cs35_register_read(address, VENUS_CS35_IRQ_STATUS1, &after) ||
	    (after & power_events) != 0 || !venus_cs35_halo_outputs_off(address)) {
		say("venus cs35: 0x%02x muted bench preflight failed\n", address);
		return false;
	}
	say("venus cs35: 0x%02x muted bench preflight passed, PUP/PDN flags %#x -> %#x\n",
	    address, before & power_events, after & power_events);
	return true;
}

/* The actual Venus DT/module and Android readback: 1 uH, 15 uF,
 * 4000 mA inductor peak, class-H selected. No converter/gain increase. */
static bool
venus_cs35_stock_boost_verified(uint8_t address)
{
	static const struct { uint32_t reg, mask, value; } stock[] = {
		{ 0x3800u, 0xffu, 0 }, { 0x3804u, 3u, 1 },
		{ 0x3808u, 0x7fu, 0x40u }, { 0x380cu, 0xffffffffu, 3 },
		{ 0x3810u, 0xffffu, 0x2424u }, { 0x3814u, 0xffffu, 0x7500u },
	};
	for (unsigned i = 0; i < sizeof(stock) / sizeof(stock[0]); i++) {
		uint32_t value;
		if (!venus_cs35_register_read(address, stock[i].reg, &value) ||
		    (value & stock[i].mask) != stock[i].value) {
			say("venus cs35: 0x%02x stock boost mismatch at %#x\n", address, stock[i].reg);
			return false;
		}
	}
	return true;
}

struct venus_cs35_bench_context {
	uint32_t state_reg;
};

static bool
venus_cs35_bench_read(__unused void *context, uint32_t reg, uint32_t *value)
{
	return venus_cs35_register_read(0x40, reg, value);
}

static bool
venus_cs35_bench_write(__unused void *context, uint32_t reg, uint32_t value)
{
	uint8_t bytes[] = { value >> 24, value >> 16, value >> 8, value };

	return venus_cs35_write(0x40, reg, bytes, sizeof(bytes));
}

static bool
venus_cs35_bench_guard(void *context)
{
	const struct venus_cs35_bench_context *bench = context;
	uint32_t irq, state, mailbox, error, peak;

	return level(15) && !level(42) && venus_cs35_mute_verified(0x40) &&
	       venus_cs35_register_read(0x40, VENUS_CS35_IRQ_STATUS1, &irq) &&
	       (irq & VENUS_CS35_POWER_FAULTS) == 0 &&
	       venus_cs35_register_read(0x40, 0x10098u, &irq) && (irq & 2u) != 0 &&
	       venus_cs35_register_read(0x40, bench->state_reg, &state) && state == 2 &&
	       venus_cs35_register_read(0x40, 0x13004u, &mailbox) && mailbox == 1 &&
	       venus_cs35_register_read(0x40, 0x0280025cu, &error) && error == 0 &&
	       venus_cs35_register_read(0x40, VENUS_CS35_BOOST_PEAK_REG, &peak) &&
	       (peak & VENUS_CS35_BOOST_PEAK_MASK) <= VENUS_CS35_BOOST_PEAK_STOCK;
}

static bool
venus_cs35_bench_reset(__unused void *context)
{
	venus_cs35_bench_reset_attempted = true;
	return venus_cs35_restore_reset(15);
}

static uint64_t
venus_cs35_bench_now(__unused void *context)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
venus_cs35_bench_delay(__unused void *context)
{
	spin_us(1000);
}

static enum venus_cs35_power_result
venus_cs35_bench_cycle(uint32_t state_reg)
{
	uint32_t threshold;
	if (!venus_cs35_stock_boost_verified(0x40) ||
	    !venus_cs35_register_read(0x40, 0x4220u, &threshold) ||
	    !venus_cs35_write_word_verified(0x40, 0x4220u, (threshold & ~3u) | 3u)) {
		say("venus cs35: stock muted power prerequisites refused\n");
		return VENUS_CS35_POWER_REFUSED;
	}
	say("venus cs35: stock boost and temperature threshold verified before muted power cycle\n");
	struct venus_cs35_bench_context bench = { .state_reg = state_reg };
	const struct venus_cs35_power_io io = {
		.context = &bench, .read = venus_cs35_bench_read, .write = venus_cs35_bench_write,
		.guard = venus_cs35_bench_guard, .reset = venus_cs35_bench_reset,
		.now_us = venus_cs35_bench_now, .delay_ms = venus_cs35_bench_delay,
	};

	return venus_cs35_muted_power_cycle(&io);
}

/* Check the real TERT TDM BCLK at each amp, with output and boost disabled. */
static bool
venus_cs35_tdm_pll(uint8_t address, uint32_t state_reg, const struct redwood_venus_cs35_live_pcm *live)
{
	const uint32_t pll = 0x2c04u, fields = 0x10fffu;
	static const uint32_t clock_steps[] = { 0xf70u, 0xf60u, 0x760u, 0x770u, 0x10770u };
	uint32_t blocks, saved, irq = 0;
	bool locked = false;

	if (state_reg == 0 &&
	    (!venus_cs35_control_write(address, VENUS_CS35_HALO_CORE_CTRL, 0) || !venus_cs35_halo_stopped(address))) {
		return false;
	}
	if (!venus_cs35_register_read(address, 0x2018u, &blocks) ||
	    !venus_cs35_write_word_verified(address, 0x2018u, blocks & ~0x31u) ||
	    !venus_cs35_halo_outputs_off(address) ||
	    !venus_cs35_register_read(address, pll, &saved)) {
		return false;
	}
	bool ready = state_reg == 0 || venus_cs35_tdm_receive(address);
	for (unsigned i = 0; ready && i < sizeof(clock_steps) / sizeof(clock_steps[0]); i++) {
		ready = venus_cs35_write_word_verified(address, pll, (saved & ~fields) | clock_steps[i]);
	}
	for (unsigned attempt = 0; ready && attempt < 8; attempt++) {
		if (!venus_cs35_halo_outputs_off(address) ||
		    !venus_cs35_register_read(address, 0x10098u, &irq)) {
			break;
		}
		if ((irq & 2) != 0) {
			locked = true;
			break;
		}
		spin_us(2000);
	}
	uint32_t state, error;
	bool verified = locked && (state_reg == 0 ||
	    (venus_cs35_register_read(address, state_reg, &state) && state == 2 &&
	     venus_cs35_register_read(address, 0x0280025cu, &error) && error == 0 &&
	     venus_cs35_cspl_command(address, VENUS_CS35_CSPL_PAUSE) && venus_cs35_test_cspl_cycle(address, state_reg, live) &&
	     venus_cs35_watch_paused(address, state_reg) && venus_cs35_bench_preflight(address)));
	if (state_reg != 0) {
		say("venus cs35: 0x%02x CSPL external clock handoff %s\n", address, verified ? "verified" : "failed");
	}
	if (verified && state_reg != 0 && address == 0x40 && venus_cs35_bench_power_requested()) {
		enum venus_cs35_power_result result = venus_cs35_bench_cycle(state_reg);
		say("venus cs35: 0x40 muted bench power result %u\n", result);
		if (result == VENUS_CS35_POWER_RESET || result == VENUS_CS35_POWER_RESET_FAILED) {
			return false;
		}
		verified = result == VENUS_CS35_POWER_OK;
	}
	if (verified && venus_digital.owned && !venus_digital.cancelled) {
		venus_digital.pll[address == 0x40 ? 0 : 1] = saved;
		return venus_cs35_halo_outputs_off(address);
	}
	bool restored = venus_cs35_write_word_verified(address, pll, saved);
	say("venus cs35: 0x%02x TDM BCLK PLL %s, raw IRQ3=%#x, PLL restore %s\n",
	    address, locked ? "locked" : "unconfirmed", irq, restored ? "verified" : "failed");
	return verified && restored && venus_cs35_halo_outputs_off(address);
}

/* OTP ID 8 uses the stock map 1, starting at word 2 bit 16. */
static bool
venus_cs35_apply_otp8(uint8_t address)
{
	static const struct {
		uint32_t reg;
		uint8_t shift, width;
	} trim[] = {
		{ 0x2030, 0, 4 },
		{ 0x2030, 7, 1 },
		{ 0x208c, 24, 6 },
		{ 0x2090, 14, 4 },
		{ 0x2090, 10, 4 },
		{ 0x300c, 11, 4 },
		{ 0x394c, 23, 2 },
		{ 0x3950, 0, 7 },
		{ 0x3950, 8, 7 },
		{ 0x3950, 16, 8 },
		{ 0x3950, 24, 8 },
		{ 0x3954, 0, 7 },
		{ 0x3954, 8, 7 },
		{ 0x3954, 16, 8 },
		{ 0x3954, 24, 8 },
		{ 0x3958, 0, 7 },
		{ 0x3958, 8, 7 },
		{ 0x3958, 16, 8 },
		{ 0x3958, 24, 8 },
		{ 0x395c, 0, 7 },
		{ 0x395c, 8, 7 },
		{ 0x395c, 16, 8 },
		{ 0x395c, 24, 8 },
		{ 0x416c, 0, 8 },
		{ 0x4160, 0, 7 },
		{ 0x416c, 8, 8 },
		{ 0x4160, 16, 10 },
		{ 0x416c, 16, 12 },
		{ 0x416c, 28, 1 },
		{ 0x4170, 0, 6 },
		{ 0x4170, 6, 1 },
		{ 0x4170, 8, 6 },
		{ 0x4170, 14, 1 },
		{ 0x4170, 16, 9 },
		{ 0x4360, 0, 5 },
		{ 0x4360, 6, 9 },
		{ 0x4448, 0, 8 },
		{ 0x4448, 8, 8 },
		{ 0x4448, 16, 8 },
		{ 0x4448, 24, 8 },
		{ 0x444c, 0, 3 },
		{ 0x6e30, 0, 5 },
		{ 0x6e30, 8, 5 },
		{ 0x6e30, 16, 5 },
		{ 0x6e30, 24, 5 },
		{ 0x6e34, 0, 5 },
		{ 0x6e34, 8, 5 },
		{ 0x6e34, 16, 5 },
		{ 0x6e34, 24, 5 },
		{ 0x6e38, 0, 5 },
		{ 0x6e38, 8, 5 },
		{ 0x6e38, 16, 5 },
		{ 0x6e38, 24, 5 },
		{ 0x6e3c, 0, 5 },
		{ 0x6e3c, 8, 5 },
		{ 0x6e3c, 16, 5 },
		{ 0x6e3c, 24, 5 },
		{ 0x6e40, 0, 5 },
		{ 0x6e40, 8, 5 },
		{ 0x6e40, 16, 5 },
		{ 0x6e40, 24, 5 },
		{ 0x6e44, 0, 5 },
		{ 0x6e48, 0, 10 },
		{ 0x6e48, 10, 10 },
		{ 0x6e48, 20, 10 },
		{ 0x6e4c, 0, 10 },
		{ 0x6e4c, 10, 10 },
		{ 0x6e4c, 20, 10 },
		{ 0x6e50, 0, 10 },
		{ 0x6e50, 10, 10 },
		{ 0x6e50, 20, 10 },
		{ 0x6e54, 0, 10 },
		{ 0x6e54, 10, 10 },
		{ 0x6e54, 20, 10 },
		{ 0x6e58, 0, 10 },
		{ 0x6e58, 10, 10 },
		{ 0x6e58, 20, 10 },
		{ 0x6e5c, 0, 10 },
		{ 0x6e5c, 10, 10 },
		{ 0x6e5c, 20, 10 },
		{ 0x6e60, 0, 10 },
		{ 0x6e60, 10, 10 },
		{ 0x6e60, 20, 10 },
		{ 0x6e64, 0, 10 },
		{ 0x7418, 7, 5 },
		{ 0x741c, 0, 5 },
		{ 0x741c, 11, 4 },
		{ 0x741c, 19, 4 },
		{ 0x7434, 17, 1 },
		{ 0x7434, 18, 7 },
		{ 0x7068, 0, 9 },
		{ 0x410c, 7, 1 },
		{ 0x400c, 0, 7 },
		{ 0x0000, 0, 1 },
		{ 0x17040, 0, 8 },
		{ 0x17040, 8, 8 },
		{ 0x17040, 16, 8 },
		{ 0x17040, 24, 8 },
		{ 0x17044, 0, 24 },
	};
	uint32_t words[32];
	uint32_t bit = 2 * 32 + 16;

	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i += 4) {
		uint8_t raw[16];
		if (!venus_cs35_read(address, 0x400u + 4 * i, raw, sizeof(raw))) {
			return false;
		}
		for (unsigned j = 0; j < 4; j++) {
			const uint8_t *b = raw + 4 * j;
			words[i + j] = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
		}
	}
	unsigned applied = 0;
	for (unsigned i = 0; i < sizeof(trim) / sizeof(trim[0]); i++) {
		const uint32_t word = bit / 32, shift = bit % 32;
		const uint32_t mask = (1u << trim[i].width) - 1;
		uint64_t source = words[word];
		if (shift + trim[i].width > 32) {
			source |= (uint64_t)words[word + 1] << 32;
		}
		bit += trim[i].width;
		if (trim[i].reg == 0) {
			continue;
		}
		uint32_t old, value = ((uint32_t)(source >> shift) & mask) << trim[i].shift;
		uint32_t reg_mask = mask << trim[i].shift;
		if (!venus_cs35_register_read(address, trim[i].reg, &old) ||
		    !venus_cs35_write_word_verified(address, trim[i].reg, (old & ~reg_mask) | value)) {
			say("venus cs35: 0x%02x OTP trim readback failed at reg=%#x field %u\n",
			    address, trim[i].reg, i);
			return false;
		}
		applied++;
	}
	say("venus cs35: 0x%02x OTP ID 8 verified, %u volatile trim fields applied\n", address, applied);
	return true;
}

/* The B2 errata sequence extracted from the matching Venus Android module. */
static bool
venus_cs35_apply_b2_errata(uint8_t address)
{
	static const struct {
		uint32_t reg, value;
	} patch[] = {
		{ 0x4100, 0 }, { 0x4310, 0 }, { 0x4400, 0 }, { 0x381c, 0x51 },
		{ 0x02bc20e0, 0 }, { 0x02bc2020, 0 }, { 0x4854, 0x01010000 },
	};
	const uint32_t key_reg = 0x40;
	const uint8_t key1[4] = { 0, 0, 0x55, 0x55 }, key2[4] = { 0, 0, 0xaa, 0xaa };
	const uint8_t lock1[4] = { 0, 0, 0xcc, 0xcc }, lock2[4] = { 0, 0, 0x33, 0x33 };
	uint32_t blocks, otp_id;

	if (!venus_cs35_control_write(address, VENUS_CS35_HALO_CORE_CTRL, 0) ||
	    !venus_cs35_halo_stopped(address) ||
	    !venus_cs35_register_read(address, 0x2018u, &blocks) ||
	    !venus_cs35_write_word_verified(address, 0x2018u, blocks & ~0x31u) ||
	    !venus_cs35_halo_outputs_off(address) ||
	    !venus_cs35_register_read(address, 0x10u, &otp_id)) {
		return false;
	}
	bool ready = venus_cs35_write(address, key_reg, key1, sizeof(key1)) &&
	    venus_cs35_write(address, key_reg, key2, sizeof(key2));
	for (unsigned i = 0; ready && i < sizeof(patch) / sizeof(patch[0]); i++) {
		ready = venus_cs35_write_word_verified(address, patch[i].reg, patch[i].value);
		if (!ready) {
			say("venus cs35: 0x%02x B2 errata readback failed at reg=%#x\n", address, patch[i].reg);
		}
	}
	uint32_t apply_otp = 0;
	PE_parse_boot_argn("adsp_venus_afe_otp_trim", &apply_otp, sizeof(apply_otp));
	if (ready && (apply_otp == 1 || venus_digital.owned)) {
		ready = otp_id == 8 && venus_cs35_halo_outputs_off(address) && venus_cs35_apply_otp8(address);
	}
	/* Always attempt the complete lock sequence, even when a previous transfer failed. */
	bool first_lock = venus_cs35_write(address, key_reg, lock1, sizeof(lock1));
	bool second_lock = venus_cs35_write(address, key_reg, lock2, sizeof(lock2));

	ready = ready && first_lock && second_lock && venus_cs35_halo_stopped(address) &&
	    venus_cs35_halo_outputs_off(address);
	say("venus cs35: 0x%02x B2 errata %s, OTP ID=%#x, test key lock %s\n",
	    address, ready ? "verified" : "incomplete", otp_id, first_lock && second_lock ? "sent" : "failed");
	return ready;
}

/* The opt-in reset probe accepts only the bootloader's GPIO input with pull-down. */
#define VENUS_RESET_BOOT_CFG 1u

static bool
venus_cs35_restore_reset(unsigned reset)
{
	wr(GPIO_IO(reset), 0);
	wr(GPIO_CFG(reset), VENUS_RESET_BOOT_CFG);
	if (rd(GPIO_CFG(reset)) != VENUS_RESET_BOOT_CFG || (rd(GPIO_IO(reset)) & 3) != 0) {
		say("venus cs35: reset GPIO%u did not restore\n", reset);
		return false;
	}
	say("venus cs35: reset GPIO%u restored low\n", reset);
	return true;
}

/* Called only from the Venus boot diagnostic, before any audio path starts. */
static bool
venus_cs35_probe(bool release_reset, bool keep_primary, bool cap_boost, bool halo_preflight,
    const struct redwood_venus_cs35_image *images, bool run_halo, bool check_tdm,
    const struct redwood_venus_cs35_live_pcm *live)
{
	static const struct {
		uint8_t address;
		unsigned reset;
	} amps[] = { { 0x40, 15 }, { 0x42, 42 } };
	bool ready = true;
	bool primary_held_high = false;
	uint32_t b2_preflight = 0;
	PE_parse_boot_argn("adsp_venus_afe_b2_preflight", &b2_preflight, sizeof(b2_preflight));

	uint32_t apply_otp = 0;
	PE_parse_boot_argn("adsp_venus_afe_otp_trim", &apply_otp, sizeof(apply_otp));
	uint32_t bench_preflight = 0;
	PE_parse_boot_argn("adsp_venus_afe_bench_preflight", &bench_preflight, sizeof(bench_preflight));

	bool bench_power = venus_cs35_bench_power_requested();
	if (bench_power && (!REDWOOD_VENUS_BENCH_POWER_BUILD || bench_preflight != 1 || live == NULL)) {
		say("venus cs35: muted bench power refused before GPIO access\n");
		return false;
	}
	if ((bench_preflight == 1 && (apply_otp != 1 || !check_tdm || !run_halo)) ||
	    (apply_otp == 1 && b2_preflight != 1) || (b2_preflight == 1 && images == NULL) || tlmm != 0 || (keep_primary && !release_reset) || (cap_boost && !keep_primary) ||
	    (halo_preflight && !cap_boost) || (images != NULL && !halo_preflight) ||
	    (run_halo && images == NULL)) {
		return false;
	}
	venus_cs35_bench_reset_attempted = false;
	tlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	if (tlmm == 0) {
		return false;
	}
	say("venus cs35: GPIO36 cfg=%#x io=%#x, GPIO37 cfg=%#x io=%#x\n",
	    rd(GPIO_CFG(36)), rd(GPIO_IO(36)), rd(GPIO_CFG(37)), rd(GPIO_IO(37)));
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		uint8_t address = amps[i].address;
		unsigned reset = amps[i].reset;
		uint32_t cfg = rd(GPIO_CFG(reset)), io = rd(GPIO_IO(reset));
		uint32_t id, rev, power, power2;
		bool held = (io & 1) == 0;

		say("venus cs35: 0x%02x reset GPIO%u cfg=%#x io=%#x\n", address, reset, cfg, io);
		if (release_reset && (cfg != VENUS_RESET_BOOT_CFG || io != 0 ||
		    level(amps[1 - i].reset) != primary_held_high)) {
			say("venus cs35: unexpected reset pin state; stopped\n");
			ready = false;
			break;
		}
		if (held && !release_reset) {
			say("venus cs35: 0x%02x reset asserted; transaction skipped\n", address);
			ready = false;
			continue;
		}
		if (release_reset) {
			/* Both rails use dummy regulator handles in the Venus Android driver. */
			wr(GPIO_IO(reset), 0);
			wr(GPIO_CFG(reset), cfg | CFG_OUTPUT);
			spin_us(2000);
			wr(GPIO_IO(reset), 2);
			spin_us(20000);
		}
		bool reset_high = level(reset);
		if (!reset_high) {
			say("venus cs35: GPIO%u stayed low\n", reset);
		}
		bool valid = reset_high && venus_cs35_register_read(address, 0x2014, &power) &&
		    venus_cs35_register_read(address, 0x2018, &power2);
		if (valid) {
			say("venus cs35: 0x%02x pwr_ctrl1=%#x pwr_ctrl2=%#x\n", address, power, power2);
			/* PWR_CTRL2 can select blocks while PWR_CTRL1.GLOBAL_EN is clear. */
			valid = (power & 1) == 0 &&
			    venus_cs35_register_read(address, 0, &id) && venus_cs35_register_read(address, 4, &rev);
		}
		if (valid) {
			say("venus cs35: 0x%02x id=%#x rev=%#x global_en=%u pwr_ctrl2=%#x\n",
			    address, id, rev, power & 1, power2);
			valid = id == 0x35a40 && (rev & 0xff) == 0xb2 && venus_cs35_check_otp(address) &&
			    venus_cs35_check_boost(address, cap_boost) &&
			    (!halo_preflight || venus_cs35_halo_preflight(address)) &&
			    (b2_preflight != 1 || venus_cs35_apply_b2_errata(address)) &&
			    (images == NULL || (venus_cs35_stage_image(address, &images[i]) &&
			    venus_cs35_stage_calibration(address) && venus_cs35_stage_rates(address))) &&
			    (run_halo ? venus_cs35_halo_run_test(address, check_tdm, live) : (!check_tdm || venus_cs35_tdm_pll(address, 0, NULL)));
		}
		if (bench_power) {
			ready = venus_cs35_restore_reset(reset) && valid;
			break; /* First bench cycle is limited to the main speaker. */
		}
		if (release_reset) {
			if (i == 0 && keep_primary && valid) {
				primary_held_high = true;
				say("venus cs35: keeping 0x40 out of reset with GLOBAL_EN clear for 0x42 read\n");
			} else if (!venus_cs35_restore_reset(reset)) {
				ready = false;
				break;
			}
		}
		if (!valid) {
			say("venus cs35: 0x%02x identification or disabled-power check failed\n", address);
			ready = false;
			if (release_reset) {
				break;
			}
		}
	}
	if (primary_held_high) {
		uint32_t power;
		if (ready) {
			ready = venus_cs35_register_read(amps[0].address, 0x2014, &power) && (power & 1) == 0;
			say("venus cs35: 0x40 final GLOBAL_EN check %s\n", ready ? "passed" : "failed");
		}
		ready = venus_cs35_restore_reset(amps[0].reset) && ready;
	}
	ml_io_unmap(tlmm, TLMM_SIZE);
	tlmm = 0;
	return ready;
}

bool
redwood_speaker_venus_identify(bool release_reset, bool keep_primary, bool cap_boost, bool halo_preflight,
    const struct redwood_venus_cs35_image *images, bool run_halo)
{
	return venus_cs35_probe(release_reset, keep_primary, cap_boost, halo_preflight, images, run_halo, false, NULL);
}

bool
redwood_speaker_venus_clock_probe(const struct redwood_venus_cs35_image *images,
    const struct redwood_venus_cs35_live_pcm *live)
{
	bool protected = images != NULL;

	return venus_cs35_probe(true, true, true, protected, images, protected, true, live);
}

/* Both HALOs stay loaded across stream pauses. Output and boost always remain off. */
static const uint8_t venus_digital_addresses[] = { 0x40, 0x42 };
static const unsigned venus_digital_resets[] = { 15, 42 };

static bool
venus_digital_reset_all(void)
{
	bool restored = true;

	venus_digital.reset_asserted = true;
	for (int i = 1; i >= 0; i--) {
		if (venus_digital.released & (1u << i)) {
			bool low = venus_cs35_restore_reset(venus_digital_resets[i]);
			restored = low && restored;
		}
	}
	venus_digital.running = 0;
	return restored;
}

#include "redwood_venus_cs35_output.inc"

void
redwood_speaker_venus_digital_cancel(void)
{
	venus_digital.cancelled = true;
}

bool
redwood_speaker_venus_digital_safe(void)
{
	if (!venus_digital.owned || venus_digital.reset_asserted) return false;
	if (venus_digital.cancelled ||
	    venus_digital.prepared != 3 || !level(15) || !level(42)) {
		venus_digital.cancelled = true;
		venus_digital_reset_all();
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		if ((venus_output.configured & (1u << i)) ? !venus_output_chip_safe(i) :
		    !venus_cs35_runtime_safe(venus_digital_addresses[i], venus_digital.state[i],
		    (venus_digital.running & (1u << i)) ? 0 : 1)) {
			venus_digital.cancelled = true;
			venus_digital_reset_all();
			return false;
		}
	}
	return true;
}

bool
redwood_speaker_venus_digital_running(bool run)
{
	if (venus_output.mask) return venus_output_run(run);
	if (!redwood_speaker_venus_digital_safe()) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		const unsigned bit = 1u << i;

		if (((venus_digital.running & bit) != 0) == run) {
			continue;
		}
		if (!venus_cs35_cspl_command(venus_digital_addresses[i],
		    run ? VENUS_CS35_CSPL_RESUME : VENUS_CS35_CSPL_PAUSE)) {
			venus_digital.cancelled = true;
			venus_digital_reset_all();
			return false;
		}
		if (run) venus_digital.running |= bit;
		else venus_digital.running &= ~bit;
	}
	bool safe = redwood_speaker_venus_digital_safe();
	say("venus cs35: dual digital %s %s, power output inhibited\n",
	    run ? "RESUME" : "PAUSE", safe ? "verified" : "failed");
	return safe;
}

bool
redwood_speaker_venus_digital_close(void)
{
	if (!venus_digital.owned) return true;
	bool alive;
	(void)redwood_speaker_venus_output_status(&alive);
	if (alive) panic("Venus output worker still owns amplifier I2C at close");
	bool closed = true;
	if (!venus_digital.reset_asserted) {
		for (int i = 1; i >= 0; i--) {
			if (!(venus_digital.prepared & (1u << i))) continue;
			const uint8_t address = venus_digital_addresses[i];
			bool stopped = venus_cs35_cspl_command(address, VENUS_CS35_CSPL_PAUSE) &&
			    venus_cs35_write_word_verified(address, 0x2c04u, venus_digital.pll[i]) &&
			    venus_cs35_control_write(address, VENUS_CS35_HALO_CORE_CTRL, 0) &&
			    venus_cs35_halo_stopped(address) && venus_cs35_halo_outputs_off(address);
			closed = stopped && closed;
			if (!stopped) break;
		}
	}
	closed = venus_digital_reset_all() && closed;
	vm_offset_t mapping = tlmm;
	tlmm = 0;
	venus_digital.owned = false;
	ml_io_unmap(mapping, TLMM_SIZE);
	say("venus cs35: dual digital close %s\n", closed ? "verified" : "failed");
	return closed;
}

bool
redwood_speaker_venus_digital_open(const struct redwood_venus_cs35_image *images)
{
	DTEntry entry;
	const void *board;
	unsigned size;

	if (tlmm != 0 || images == NULL || images[0].count != 8 || images[1].count != 8 ||
	    venus_cs35_bench_power_requested() ||
	    SecureDTLookupEntry(NULL, "/arm-io", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "device_type", &board, &size) != kSuccess ||
	    size != sizeof("venus") || memcmp(board, "venus", size) != 0) {
		return false;
	}
	/* cancelled may already be set by a simultaneous shutdown request. */
	if (venus_digital.cancelled || !venus_output_options()) return false;
	venus_digital.prepared = venus_digital.released = venus_digital.running = 0;
	venus_digital.reset_asserted = false;
	tlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	if (tlmm == 0) return false;
	venus_digital.owned = true;
	for (unsigned i = 0; i < 2; i++) {
		if (rd(GPIO_CFG(venus_digital_resets[i])) != VENUS_RESET_BOOT_CFG ||
		    rd(GPIO_IO(venus_digital_resets[i])) != 0) {
			redwood_speaker_venus_digital_close();
			return false;
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		const unsigned reset = venus_digital_resets[i];
		const uint8_t address = venus_digital_addresses[i];
		uint32_t id, revision, power;

		if (venus_digital.cancelled) break;
		venus_digital.released |= 1u << i;
		wr(GPIO_IO(reset), 0);
		wr(GPIO_CFG(reset), VENUS_RESET_BOOT_CFG | CFG_OUTPUT);
		spin_us(2000);
		wr(GPIO_IO(reset), 2);
		spin_us(20000);
		bool ready = level(reset) && venus_cs35_register_read(address, 0x2014u, &power) && (power & 1) == 0 &&
		    venus_cs35_register_read(address, 0, &id) && id == 0x35a40 &&
		    venus_cs35_register_read(address, 4, &revision) && (revision & 0xff) == 0xb2 &&
		    venus_cs35_check_otp(address) && venus_cs35_check_boost(address, true) &&
		    venus_cs35_halo_preflight(address) && venus_cs35_apply_b2_errata(address) &&
		    venus_cs35_stage_image(address, &images[i]) && venus_cs35_stage_calibration(address) &&
		    venus_cs35_stage_rates(address) && venus_cs35_halo_run_test(address, true, NULL);
		if (!ready) break;
	}
	if (!redwood_speaker_venus_digital_safe()) {
		redwood_speaker_venus_digital_close();
		return false;
	}
	if (venus_output.mask && (!venus_output_prepare() || !redwood_speaker_venus_digital_safe())) {
		redwood_speaker_venus_digital_close();
		return false;
	}
	say("venus cs35: dual digital session prepared, both HALOs paused, power output inhibited\n");
	return true;
}

static const char *
amplifier_name(uint16_t id)
{
	switch (id) {
	case 0x1852: return "AW88194/AW88195";
	case 0x2013: return "AW88264";
	case 0x2032: return "AW88266";
	case 0x2049: return "AW88395";
	case 0x2055: return "AW88256";
	case 0x2066: return "AW88166";
	case 0x2071: return "AW88350";
	case 0x2113: return "AW88261";
	case 0x2183: return "AW88399";
	case 0x2308: return "AW88230";
	}
	return "an amplifier this list does not know";
}

/* ---- the amplifiers, as Android's driver runs them -------------------------------------------------- */

#define AW_ID                   0x00            /* reads the chip ID; 0x55aa written resets the chip */
#define AW_ID_AW88261           0x2113
#define AW_SOFT_RESET           0x55aa
#define AW_SYSST                0x01
#define AW_SYSINT               0x02            /* read: cleared */
#define AW_SYSINTM              0x03
#define AW_SYSCTRL              0x04
#define AW_SYSCTRL2             0x05            /* [9:0] the volume, attenuation: [9:6] 6 dB steps, [5:0] eighths of a dB */
#define AW_I2SCTRL3             0x08
#define AW_VBAT                 0x21            /* [9:0] the battery's voltage as the amplifier sees it: x 6025 / 1023 mV */
#define AW_TEMP                 0x22            /* [9:0] the amplifier's temperature, degrees Celsius, two's complement */
#define AW_PVDD                 0x23            /* [9:0] its boosted supply */
#define AW_ISNDAT               0x24            /* the current and voltage sense, as last measured */
#define AW_VSNDAT               0x25
#define AW_DBGCTRL              0x14
#define AW_VSNTM1               0x50
#define AW_EFRH4                0x78
#define AW_EFRH3                0x79
#define AW_EFRL4                0x7c
#define AW_EFRL3                0x7d

#define SYSCTRL_PWDN            (1u << 0)
#define SYSCTRL_AMPPD           (1u << 1)
#define SYSCTRL_HMUTE           (1u << 8)
#define SYSCTRL_ULS_HMUTE       (1u << 14)
#define I2SCTRL3_TXEN           (1u << 6)
#define DBGCTRL_EF_OR           (1u << 3)       /* the fuses' two copies are or-ed; else and-ed */
#define SYSINTM_DEFAULT         0xffff

#define SYSST_PLLS              (1u << 0)
#define SYSST_OTHS              (1u << 1)
#define SYSST_OCDS              (1u << 3)
#define SYSST_CLKS              (1u << 4)
#define SYSST_NOCLKS            (1u << 5)
#define SYSST_WDS               (1u << 6)
#define SYSST_SWS               (1u << 8)
#define SYSST_BSTS              (1u << 9)
#define SYSST_BSTOCS            (1u << 11)
#define SYSST_UVLS              (1u << 14)
#define SYSST_OVP2S             (1u << 15)
#define SYSST_PLL_CHECK         (SYSST_CLKS | SYSST_PLLS)
#define SYSST_CHECK             (SYSST_BSTS | SYSST_SWS | SYSST_CLKS | SYSST_PLLS)
#define SYSST_FAULTS            (SYSST_OVP2S | SYSST_UVLS | SYSST_BSTOCS | SYSST_WDS | SYSST_NOCLKS | SYSST_OCDS | SYSST_OTHS)

#define VOLUME_LOWEST           (90 * 8)        /* the driver's "mute volume", in eighths of a dB */
#define VOLUME_STEP             (6 * 8)
#define STATUS_TRIES            10              /* the driver's AW_DEV_SYSST_CHECK_MAX, 2 ms apart */

/* Android's configuration file ("ACF", Awinic's format; Linux: sound/soc/codecs/aw88395/aw88395_lib.c) */
#define ACF_PROFILE_MUSIC       0
#define ACF_DATA_REGISTERS      4               /* a block with a header of its own and registers in it */
#define ACF_DATA_MONITOR        10              /* the driver's "monitor": what to do when the chip is cold or the battery low */
#define MONITOR_ROWS            8
#define ACF_BLOCK_HEADER        0x3c

struct amplifier {
	uint8_t address;
	unsigned device;                /* the file's device number for it (Android's sound-channel) */
	const char *where;
	uint16_t profile[64][2];        /* register, value */
	unsigned count;
	uint16_t sysctrl;               /* what the profile has for 0x04, 0x08 and 0x05 */
	uint16_t i2sctrl3;
	uint16_t volume_at;             /* Android's level: from the profile's 0x05, in eighths of a dB of attenuation */
	uint16_t volume;                /* where it is now */
	uint16_t volume_high;           /* the profile's bits 15:10 of 0x05, which the volume leaves as they are */
	bool prepared, started;
	/* the monitor's tables (from the file) and what was last seen */
	struct { int16_t min, max; uint16_t less; } temp_rows[MONITOR_ROWS], vbat_rows[MONITOR_ROWS];
	unsigned temp_count, vbat_count, looks;
	int temps[3], vbats[3];
	int temp_row, vbat_row;
};

static struct amplifier amplifiers[2] = {
	{ .address = 0x35, .device = 0, .where = "0x35, slot 0", .temp_row = -1, .vbat_row = -1 },
	{ .address = 0x34, .device = 1, .where = "0x34, slot 1", .temp_row = -1, .vbat_row = -1 },
};
static bool broken;                     /* something went wrong: both have been powered down, nothing more is done */

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * The driver's monitor for one amplifier (Android has it on: aw_dev_N_monitor), from the file: every second it
 * reads the chip's temperature and the battery's voltage, and a table says what to do for each range of them.
 * This file's: at 7 to 100 degrees nothing; at 2 to 5 degrees 3 dB less, at -2 to 0 4.5 dB, at -50 to -5 8 dB (and
 * a lower current limit for the boost, which is not done here); from 3.85 V nothing, at 3.65 to 3.75 V and up to
 * 3.55 V a lower "vmax" for the DSP's algorithm, which does not run here. In its temperature rows a vmax goes with
 * each reduction of the volume, so the battery rows' vmax is turned into a volume reduction in the same proportion
 * (the first temperature row's: 3 dB for its vmax): 1.6 and 3.1 dB. Between the ranges the last one holds.
 *   block: checksum (of the bytes after it), version, chip name[16], ui version, the time and count of looks,
 *   switches, then for the temperatures and for the voltages each: a value, the number of rows, a row's size and
 *   the rows' offset; a row: int16 lowest, int16 highest, uint16 boost current, uint16 reduction (eighths of a
 *   dB), int32 vmax
 */
static void
acf_monitor(uint16_t device, const uint8_t *block, uint32_t length)
{
	struct amplifier *a = NULL;
	uint32_t sum = 0, temp_count, temp_size, temp_at, vbat_count, vbat_size, vbat_at;
	int64_t per_eighth = 0;

	for (unsigned i = 0; i < 2; i++) {
		if (amplifiers[i].device == device) {
			a = &amplifiers[i];
		}
	}
	if (a == NULL || length < 0x48) {
		return;
	}
	for (uint32_t i = 4; i < length; i++) {
		sum += block[i];
	}
	temp_count = le32(block + 0x2c);
	temp_size = le32(block + 0x30);
	temp_at = le32(block + 0x34);
	vbat_count = le32(block + 0x3c);
	vbat_size = le32(block + 0x40);
	vbat_at = le32(block + 0x44);
	if (sum != le32(block) || temp_size < 12 || vbat_size < 12 || temp_count > MONITOR_ROWS || vbat_count > MONITOR_ROWS ||
	    temp_at + temp_count * temp_size > length || vbat_at + vbat_count * vbat_size > length) {
		say("speaker: device %u's monitor table is not of the shape known: not used\n", device);
		return;
	}
	for (uint32_t i = 0; i < temp_count; i++) {
		const uint8_t *row = block + temp_at + i * temp_size;
		int32_t vmax = (int32_t)le32(row + 8);

		a->temp_rows[i].min = (int16_t)(row[0] | row[1] << 8);
		a->temp_rows[i].max = (int16_t)(row[2] | row[3] << 8);
		a->temp_rows[i].less = (uint16_t)(row[6] | row[7] << 8);
		if (per_eighth == 0 && a->temp_rows[i].less != 0 && vmax < 0) {
			per_eighth = -(int64_t)vmax / a->temp_rows[i].less;
		}
	}
	for (uint32_t i = 0; i < vbat_count; i++) {
		const uint8_t *row = block + vbat_at + i * vbat_size;
		int32_t vmax = (int32_t)le32(row + 8);

		a->vbat_rows[i].min = (int16_t)(row[0] | row[1] << 8);
		a->vbat_rows[i].max = (int16_t)(row[2] | row[3] << 8);
		a->vbat_rows[i].less = (uint16_t)(row[6] | row[7] << 8);
		if (vmax < 0 && per_eighth > 0) {
			a->vbat_rows[i].less += (uint16_t)((-(int64_t)vmax + per_eighth / 2) / per_eighth);
		}
	}
	a->temp_count = temp_count;
	a->vbat_count = vbat_count;
	for (uint32_t i = 0; i < temp_count; i++) {
		say("speaker: the amplifier %s's monitor: at %d to %d degrees, %u.%03u dB less\n", a->where, a->temp_rows[i].min,
		    a->temp_rows[i].max, a->temp_rows[i].less / 8, a->temp_rows[i].less % 8 * 125);
	}
	for (uint32_t i = 0; i < vbat_count; i++) {
		say("speaker: the amplifier %s's monitor: at %d to %d mV, %u.%03u dB less\n", a->where, a->vbat_rows[i].min,
		    a->vbat_rows[i].max, a->vbat_rows[i].less / 8, a->vbat_rows[i].less % 8 * 125);
	}
}

/* The "Music" profile of every amplifier, from the file; false if the file is not of the shape known. */
static bool
acf_profiles(const uint8_t *acf, uint64_t size)
{
	uint32_t entry_size, entries, at;

	if (size < 0x50 || size > 0x100000) {
		say("speaker: the configuration file is %llu bytes: not one of Awinic's\n", size);
		return false;
	}
	entry_size = le32(acf + 0x34);
	entries = le32(acf + 0x38);
	at = le32(acf + 0x3c);
	if (entries == 0 || entries > 64 || entry_size % entries != 0 || entry_size / entries < 0x30 ||
	    (uint64_t)at + entry_size > size) {
		say("speaker: the configuration file's table (%u entries, %u bytes at %#x) is not of a shape known\n", entries,
		    entry_size, at);
		return false;
	}
	for (uint32_t e = 0; e < entries; e++) {
		const uint8_t *entry = acf + at + e * (entry_size / entries);
		char name[17];
		uint16_t device = (uint16_t)(entry[20] | entry[21] << 8), profile = (uint16_t)(entry[26] | entry[27] << 8);
		uint32_t kind = le32(entry + 28), length = le32(entry + 32), offset = le32(entry + 36);
		uint32_t checksum, sum = 0, data_length, count;
		const uint8_t *block = acf + offset;
		struct amplifier *a = NULL;

		memcpy(name, entry + 4, 16);
		name[16] = 0;
		if (kind == ACF_DATA_MONITOR) {
			acf_monitor(device, acf + offset, (uint64_t)offset + length <= size ? length : 0);
			continue;
		}
		if (kind != ACF_DATA_REGISTERS || profile != ACF_PROFILE_MUSIC || strcmp(name, "aw88261") != 0) {
			continue;
		}
		for (unsigned i = 0; i < 2; i++) {
			if (amplifiers[i].device == device) {
				a = &amplifiers[i];
			}
		}
		if (a == NULL || (uint64_t)offset + length > size || length < ACF_BLOCK_HEADER + 4) {
			say("speaker: the configuration file's entry %u (device %u) does not fit\n", e, device);
			return false;
		}
		checksum = le32(block);
		data_length = le32(block + 0x10);
		count = le32(block + ACF_BLOCK_HEADER);
		if (ACF_BLOCK_HEADER + data_length > length || data_length != 4 + 4 * count || count > 64 ||
		    le32(block + 0x20) != 2 || le32(block + 0x24) != 2) {
			say("speaker: device %u's Music profile is not a list of 16-bit registers (%u bytes, %u registers)\n", device,
			    data_length, count);
			return false;
		}
		for (uint32_t i = 4; i < ACF_BLOCK_HEADER + data_length; i++) {
			sum += block[i];
		}
		if (sum != checksum) {
			say("speaker: device %u's Music profile does not add up (%#x, the file says %#x)\n", device, sum, checksum);
			return false;
		}
		a->count = count;
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *pair = block + ACF_BLOCK_HEADER + 4 + 4 * i;

			a->profile[i][0] = (uint16_t)(pair[0] | pair[1] << 8);
			a->profile[i][1] = (uint16_t)(pair[2] | pair[3] << 8);
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		bool have[3] = { false, false, false };

		for (unsigned r = 0; r < a->count; r++) {
			uint16_t reg = a->profile[r][0], value = a->profile[r][1];

			if (reg == AW_SYSCTRL) {
				a->sysctrl = value;
				have[0] = true;
			} else if (reg == AW_I2SCTRL3) {
				a->i2sctrl3 = value;
				have[1] = true;
			} else if (reg == AW_SYSCTRL2) {
				/* the driver's aw_pid_2113_reg_val_to_db() */
				a->volume_at = (uint16_t)(((value & 0x3ff) >> 6) * VOLUME_STEP + (value & 0x3f));
				a->volume_high = value & 0xfc00;
				have[2] = true;
			} else if (reg == AW_ID || reg >= 0x70) {
				say("speaker: device %u's Music profile writes register %#x, which is not one of the configuration's\n",
				    a->device, reg);
				return false;
			}
		}
		if (a->count == 0 || !have[0] || !have[1] || !have[2] || a->volume_at >= VOLUME_LOWEST) {
			say("speaker: no usable Music profile for device %u (the amplifier at %#x) in the configuration file\n",
			    a->device, a->address);
			return false;
		}
		say("speaker: the amplifier %s: Music profile of %u registers; Android's volume there is -%u.%03u dB (%#x)\n", a->where,
		    a->count, a->volume_at / 8, a->volume_at % 8 * 125, a->volume_at);
	}
	return true;
}

/* Both powered down at once, without the driver's ceremony: a fault, or a value that did not stick. */
static void
power_down_now(const char *why)
{
	broken = true;
	say("speaker: %s: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE (0x04: amplifier power-down and power-down set)\n", why);
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t value = 0, after = 0;

		if (!a->prepared && !a->started) {
			continue;
		}
		if (!register_read(a->address, AW_SYSCTRL, &value)) {
			value = a->sysctrl;
		}
		value |= SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE;
		if (!register_write(a->address, AW_SYSCTRL, value) || !register_read(a->address, AW_SYSCTRL, &after)) {
			say("speaker: the amplifier %s: 0x04 <- %#06x NOT ACKNOWLEDGED\n", a->where, value);
		} else {
			say("speaker: the amplifier %s: 0x04 <- %#06x, reads %#06x\n", a->where, value, after);
		}
		a->started = false;
	}
}

/* One register written, said and read back; false (and both powered down) if it did not stick. */
static bool
put(struct amplifier *a, uint8_t reg, uint16_t value)
{
	uint16_t after = 0;

	if (broken) {
		return false;
	}
	if (!register_write(a->address, reg, value)) {
		say("speaker: the amplifier %s: %#04x <- %#06x NOT ACKNOWLEDGED\n", a->where, reg, value);
		power_down_now("a write was not acknowledged");
		return false;
	}
	if (!register_read(a->address, reg, &after)) {
		say("speaker: the amplifier %s: %#04x <- %#06x, NOT READ BACK\n", a->where, reg, value);
		power_down_now("a register could not be read back");
		return false;
	}
	say("speaker: the amplifier %s: %#04x <- %#06x, reads %#06x%s\n", a->where, reg, value, after, after == value ? "" :
	    ": NOT AS WRITTEN");
	if (after != value) {
		power_down_now("a register did not read back as written");
		return false;
	}
	return true;
}

static bool
get(struct amplifier *a, uint8_t reg, uint16_t *value)
{
	if (!register_read(a->address, reg, value)) {
		say("speaker: the amplifier %s: %#04x NOT READ\n", a->where, reg);
		return false;
	}
	return true;
}

/* The bits of 0x04 named in `bits' set or cleared, the rest as they read. */
static bool
sysctrl(struct amplifier *a, uint16_t bits, bool set)
{
	uint16_t value;

	if (broken || !get(a, AW_SYSCTRL, &value)) {
		return false;
	}
	return put(a, AW_SYSCTRL, set ? (value | bits) : (value & ~bits));
}

/* The volume register at `volume' eighths of a dB of attenuation (the driver's aw_pid_2113_set_volume()). */
static bool
set_volume(struct amplifier *a, uint16_t volume)
{
	uint16_t value;

	if (volume > VOLUME_LOWEST) {
		volume = VOLUME_LOWEST;
	}
	if (volume < a->volume_at) {
		volume = a->volume_at;  /* never louder than Android */
	}
	/* (the other bits as the profile has them and as they were written: not read first, for a quick fade) */
	value = (uint16_t)(a->volume_high | (volume / VOLUME_STEP) << 6 | volume % VOLUME_STEP);
	if (broken || !put(a, AW_SYSCTRL2, value)) {
		return false;
	}
	a->volume = volume;
	return true;
}

/* What the status register says, and whether it says a fault. */
static bool
faulty(struct amplifier *a, uint16_t *status)
{
	if (!get(a, AW_SYSST, status)) {
		return true;
	}
	return (*status & SYSST_FAULTS) != 0;
}

bool
redwood_speaker_supported(void)
{
	DTEntry entry;
	const void *value;
	unsigned size;

	/* Venus uses CS35L41; only Redwood has the AW88261 wiring below. */
	return SecureDTLookupEntry(NULL, "/arm-io", &entry) == kSuccess &&
	       SecureDTGetProperty(entry, "device_type", &value, &size) == kSuccess &&
	       size == sizeof("redwood") && memcmp(value, "redwood", size) == 0;
}

bool
redwood_speaker_prepare(const uint8_t *acf, uint64_t size)
{
	if (!redwood_speaker_supported()) {
		return false;
	}
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	broken = false;
	say("speaker: the amplifiers are prepared as Android's driver prepares them; the I2C pins as found:\n");
	show_pins();
	if (!acf_profiles(acf, size)) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t id = 0, h4 = 0, h3 = 0, l4 = 0, l3 = 0, icalk_bits, vcalk_bits;
		int icalk, vcalk;
		uint16_t vcalb, ef_or = 0;

		if (!get(a, AW_ID, &id)) {
			say("speaker: nobody at I2C address %#x: not powered, or not there\n", a->address);
			power_down_now("an amplifier does not answer");
			return false;
		}
		say("speaker: the amplifier %s: chip ID %#06x, %s\n", a->where, id, amplifier_name(id));
		if (id != AW_ID_AW88261) {
			return false;
		}
		if (!get(a, AW_EFRH4, &h4) || !get(a, AW_EFRH3, &h3) || !get(a, AW_EFRL4, &l4) || !get(a, AW_EFRL3, &l3)) {
			return false;
		}
		for (unsigned r = 0; r < a->count; r++) {
			if (a->profile[r][0] == AW_DBGCTRL) {
				ef_or = a->profile[r][1] & DBGCTRL_EF_OR;
			}
		}
		/* the driver's aw_dev_get_icalk(), aw_dev_get_vcalk() and aw_dev_set_vcalb() */
		icalk_bits = ef_or ? ((h4 & 0x3ff) | (l4 & 0x3ff)) : ((h4 & 0x3ff) & (l4 & 0x3ff));
		vcalk_bits = ef_or ? ((h3 & 0x3ff) | (l3 & 0x3ff)) : ((h3 & 0x3ff) & (l3 & 0x3ff));
		icalk = 1000 + ((icalk_bits & 0x200) ? (int)icalk_bits - 0x400 : (int)icalk_bits);
		vcalk = 1000 + ((vcalk_bits & 0x200) ? (int)vcalk_bits - 0x400 : (int)vcalk_bits);
		if (vcalk <= 0 || icalk <= 0) {
			say("speaker: the amplifier %s: fuses %#x %#x %#x %#x make no sense\n", a->where, h4, h3, l4, l3);
			return false;
		}
		vcalb = (uint16_t)(8192 * icalk / vcalk);
		say("speaker: the amplifier %s: fuses %#06x %#06x %#06x %#06x: icalk %d, vcalk %d, so 0x50 is to be %#06x\n", a->where,
		    h4, h3, l4, l3, icalk, vcalk, vcalb);

		say("speaker: the amplifier %s: soft reset (0x00 <- 0x55aa), as the driver does it first\n", a->where);
		a->prepared = true;
		if (!register_write(a->address, AW_ID, AW_SOFT_RESET)) {
			power_down_now("the soft reset was not acknowledged");
			return false;
		}
		wait_ms(2);
		for (unsigned r = 0; r < a->count; r++) {
			uint8_t reg = (uint8_t)a->profile[r][0];
			uint16_t value = a->profile[r][1], now;

			switch (reg) {
			case AW_SYSINTM:
				value = SYSINTM_DEFAULT;
				break;
			case AW_SYSCTRL:
				/* the three power and mute bits stay as they are; the power stage's mute on */
				if (!get(a, AW_SYSCTRL, &now)) {
					power_down_now("0x04 could not be read");
					return false;
				}
				value = (uint16_t)((value & ~(SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE)) |
				    (now & (SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE)) | SYSCTRL_ULS_HMUTE);
				break;
			case AW_I2SCTRL3:
				value &= (uint16_t)~I2SCTRL3_TXEN;
				break;
			case AW_VSNTM1:
				continue;
			}
			if (!put(a, reg, value)) {
				return false;
			}
		}
		if (!put(a, AW_VSNTM1, vcalb) || !set_volume(a, VOLUME_LOWEST)) {
			return false;
		}
	}
	say("speaker: BOTH AMPLIFIERS ARE CONFIGURED as Android's Music profile has them: powered down, muted, at the lowest volume\n");
	return true;
}

bool
redwood_speaker_start(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t status = 0, interrupts = 0;
		unsigned tries;

		if (broken || !a->prepared) {
			return false;
		}
		say("speaker: the amplifier %s: power-down off\n", a->where);
		a->started = true;
		if (!sysctrl(a, SYSCTRL_PWDN, false)) {
			return false;
		}
		wait_ms(2);
		for (tries = 0; tries < STATUS_TRIES; tries++) {
			if (!get(a, AW_SYSST, &status)) {
				power_down_now("the status could not be read");
				return false;
			}
			if ((status & SYSST_PLL_CHECK) == SYSST_PLL_CHECK) {
				break;
			}
			wait_ms(2);
		}
		say("speaker: the amplifier %s: status %#06x: its PLL %s\n", a->where, status, tries < STATUS_TRIES ?
		    "IS LOCKED to the bit clock" : "DOES NOT LOCK: no clock?");
		if (tries == STATUS_TRIES) {
			power_down_now("an amplifier's PLL does not lock");
			return false;
		}
		say("speaker: the amplifier %s: amplifier power-down off\n", a->where);
		if (!sysctrl(a, SYSCTRL_AMPPD, false)) {
			return false;
		}
		wait_ms(1);
		for (tries = 0; tries < STATUS_TRIES; tries++) {
			if (faulty(a, &status)) {
				break;
			}
			if ((status & SYSST_CHECK) == SYSST_CHECK) {
				break;
			}
			wait_ms(2);
		}
		say("speaker: the amplifier %s: status %#06x\n", a->where, status);
		if ((status & SYSST_FAULTS) != 0 || (status & SYSST_CHECK) != SYSST_CHECK) {
			power_down_now((status & SYSST_FAULTS) ? "an amplifier reports a fault" : "an amplifier's status is not as "
			    "the driver wants it");
			return false;
		}
		if (!put(a, AW_I2SCTRL3, a->i2sctrl3) ||        /* the feedback on, as the profile has it */
		    !sysctrl(a, SYSCTRL_ULS_HMUTE, false) || !sysctrl(a, SYSCTRL_HMUTE, false)) {
			return false;
		}
		if (!get(a, AW_SYSINT, &interrupts) || !get(a, AW_SYSINT, &interrupts) || !put(a, AW_SYSINTM, SYSINTM_DEFAULT)) {
			power_down_now("the interrupts could not be read");
			return false;
		}
		if (faulty(a, &status)) {
			say("speaker: the amplifier %s: status %#06x\n", a->where, status);
			power_down_now("an amplifier reports a fault");
			return false;
		}
		say("speaker: THE AMPLIFIER %s RUNS, unmuted at the lowest volume (status %#06x)\n", a->where, status);
	}
	return true;
}

bool
redwood_speaker_volume(uint32_t below, uint32_t step_ms)
{
	uint16_t status;

	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint32_t target = a->volume_at + below;

		if (broken || !a->started) {
			return false;
		}
		if (target > VOLUME_LOWEST) {
			target = VOLUME_LOWEST;
		}
		say("speaker: the amplifier %s: the volume to -%u.%03u dB (Android's level less %u.%03u dB), 6 dB a step every %u ms\n",
		    a->where, target / 8, target % 8 * 125, (target - a->volume_at) / 8, (target - a->volume_at) % 8 * 125, step_ms);
	}
	/* both together, a step every step_ms, the status looked at after every step */
	for (;;) {
		bool moved = false;

		for (unsigned i = 0; i < 2; i++) {
			struct amplifier *a = &amplifiers[i];
			uint32_t target = a->volume_at + below, next;

			if (target > VOLUME_LOWEST) {
				target = VOLUME_LOWEST;
			}
			if (a->volume == target) {
				continue;
			}
			next = a->volume > target ? (a->volume - target > VOLUME_STEP ? a->volume - VOLUME_STEP : target) :
			    (target - a->volume > VOLUME_STEP ? a->volume + VOLUME_STEP : target);
			if (!set_volume(a, (uint16_t)next)) {
				return false;
			}
			/* (a slow fade looks at the status after every step, a quick one once it is over) */
			if (step_ms >= 10 && faulty(a, &status)) {
				say("speaker: the amplifier %s: status %#06x\n", a->where, status);
				power_down_now("an amplifier reports a fault");
				return false;
			}
			moved = true;
		}
		if (!moved) {
			break;
		}
		if (step_ms) {
			wait_ms(step_ms);
		}
	}
	return redwood_speaker_healthy();
}

bool
redwood_speaker_healthy(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t status;

		if (broken) {
			return false;
		}
		if (!a->started) {
			continue;
		}
		if (faulty(a, &status)) {
			say("speaker: the amplifier %s: status %#06x\n", a->where, status);
			power_down_now("an amplifier reports a fault");
			return false;
		}
	}
	return true;
}

/* the row of a monitor table that a value falls in; between rows, the one before */
static int
monitor_row(const struct amplifier *a, bool temperature, int value, int before)
{
	unsigned count = temperature ? a->temp_count : a->vbat_count;

	for (unsigned i = 0; i < count; i++) {
		int min = temperature ? a->temp_rows[i].min : a->vbat_rows[i].min;
		int max = temperature ? a->temp_rows[i].max : a->vbat_rows[i].max;

		if (value >= min && value <= max) {
			return (int)i;
		}
	}
	return before;
}

/*
 * The driver's monitor, once: the temperature and the battery voltage of every amplifier that runs (the average of
 * the last three looks, as the file's count says), and the reduction their tables ask for, the larger of the two
 * amplifiers'. With `tell', what was read (also the boosted supply and the current and voltage sense, raw).
 */
uint32_t
redwood_speaker_monitor(bool tell)
{
	uint32_t wanted = 0;

	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t vbat = 0, temp = 0, pvdd = 0, isn = 0, vsn = 0;
		int t = 0, v = 0;
		uint32_t less = 0;

		if (broken || !a->started) {
			continue;
		}
		if (!get(a, AW_VBAT, &vbat) || !get(a, AW_TEMP, &temp)) {
			continue;
		}
		a->temps[a->looks % 3] = (temp & 0x200) ? (int)(temp & 0x3ff) - 0x400 : (int)(temp & 0x3ff);
		a->vbats[a->looks % 3] = (int)(vbat & 0x3ff) * 6025 / 1023;
		a->looks++;
		for (unsigned k = 0; k < 3 && k < a->looks; k++) {
			t += a->temps[k];
			v += a->vbats[k];
		}
		t /= (int)(a->looks < 3 ? a->looks : 3);
		v /= (int)(a->looks < 3 ? a->looks : 3);
		a->temp_row = monitor_row(a, true, t, a->temp_row);
		a->vbat_row = monitor_row(a, false, v, a->vbat_row);
		less = (a->temp_row >= 0 ? a->temp_rows[a->temp_row].less : 0) + (a->vbat_row >= 0 ? a->vbat_rows[a->vbat_row].less : 0);
		if (less > wanted) {
			wanted = less;
		}
		if (tell) {
			(void)get(a, AW_PVDD, &pvdd);
			(void)get(a, AW_ISNDAT, &isn);
			(void)get(a, AW_VSNDAT, &vsn);
			say("speaker: the amplifier %s: %d degrees, the battery %d mV (%#05x, %#05x; supply %#05x, sense %#06x %#06x): "
			    "%u.%03u dB less\n", a->where, t, v, temp, vbat, pvdd, isn, vsn, less / 8, less % 8 * 125);
		}
	}
	return wanted;
}

void
redwood_speaker_stop(void)
{
	if (broken) {
		return;                 /* (already powered down) */
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];

		if (!a->started) {
			continue;
		}
		say("speaker: the amplifier %s is stopped as Android's driver stops it\n", a->where);
		if (!sysctrl(a, SYSCTRL_ULS_HMUTE, true)) {
			return;
		}
		while (a->volume < VOLUME_LOWEST) {
			if (!set_volume(a, (uint16_t)(a->volume + VOLUME_STEP > VOLUME_LOWEST ? VOLUME_LOWEST : a->volume +
			    VOLUME_STEP))) {
				return;
			}
			wait_ms(1);
		}
		if (!sysctrl(a, SYSCTRL_HMUTE, true) || !put(a, AW_I2SCTRL3, (uint16_t)(a->i2sctrl3 & ~I2SCTRL3_TXEN))) {
			return;
		}
		wait_ms(1);
		if (!sysctrl(a, SYSCTRL_AMPPD, true) || !sysctrl(a, SYSCTRL_PWDN, true)) {
			return;
		}
		a->started = false;
		say("speaker: THE AMPLIFIER %s IS POWERED DOWN\n", a->where);
	}
}

void
redwood_speaker_quiesce(void)
{
	if (venus_digital.owned && tlmm != 0) {
		leaving = true;
		venus_digital.cancelled = true;
		venus_digital_reset_all();
		return;
	}
	bool any = false;

	for (unsigned i = 0; i < 2; i++) {
		any |= amplifiers[i].started;
	}
	if (!any || tlmm == 0) {
		return;
	}
	leaving = true;
	power_down_now("the kernel is on its way out");
}

/* ---- speaker_probe=N: a look ------------------------------------------------------------------------ */

static void
show_amplifiers(void)
{
	/* the chip ID, then registers that every chip of the family has at the same place and that reading does not change */
	static const struct {
		uint8_t reg;
		const char *name;
	} more[] = { { 0x01, "status" }, { 0x04, "system control" }, { 0x06, "I2S control" } };

	for (unsigned i = 0; i < 2; i++) {
		uint16_t id = 0, value = 0;

		if (!register_read(amplifiers[i].address, AW_ID, &id)) {
			say("speaker: nobody at I2C address %#x: not powered, or not there\n", amplifiers[i].address);
			continue;
		}
		say("speaker: the amplifier %s is there: chip ID %#06x, %s\n", amplifiers[i].where, id, amplifier_name(id));
		for (unsigned r = 0; r < sizeof(more) / sizeof(more[0]); r++) {
			if (register_read(amplifiers[i].address, more[r].reg, &value)) {
				say("speaker:   register %#04x (%s) = %#06x\n", more[r].reg, more[r].name, value);
			}
		}
	}
}

/* ---- power names of the audio subsystem ------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static void
show_power(void)
{
	/* the subsystem's two supplies and, for the DSP, the processor's own two and the crystal */
	static const char *const supplies[] = { "lcx.lvl", "lmx.lvl", "cx.lvl", "mx.lvl", "xo.lvl" };

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		uint32_t address = rpmh_address(supplies[i]);

		if (address) {
			say("speaker: supply \"%s\" can be asked for at RPMh address %#x\n", supplies[i], address);
		} else {
			say("speaker: no supply \"%s\" in the command database\n", supplies[i]);
		}
	}
	/*
	 * (The audio subsystem's bus clock, GCC_CFG_NOC_LPASS_CBCR, is not read: on the phone that read
	 * stops the kernel with a synchronous external abort -- the register is not the main processors'
	 * to read while the audio subsystem belongs to its DSP.)
	 */
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	say("speaker: a look at the phone's sound hardware (level %u; nothing is switched on)\n", probe_level);
	show_pins();
	show_power();
	if (probe_level >= 2) {
		show_amplifiers();
	}
	say("speaker: done\n");
	thread_terminate(current_thread());
}

void
redwood_speaker_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("speaker_probe", &probe_level, sizeof(probe_level)) || probe_level == 0) {
		return;
	}
	if (!redwood_speaker_supported()) {
		say("speaker: AW88261 probe is supported only on Redwood\n");
		return;
	}
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
