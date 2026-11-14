/*
 * The Mi 11's ST FTS touch chip (touchscreen=3), beside redwood's FocalTech path.
 *
 * The chip is not FocalTech's FTS: it is ST's ("st,fts" in the phone's device tree, and
 * st_fts_k2.ftb in Android's /vendor/firmware), and its protocol is the raw one of ST's own
 * driver (research: touchscreen/fts_spi/fts.c, fts_lib/ftsIO.c, ftsHardware.h, ftsSoftware.h --
 * the driver whose probe prints Android's "[ FTS ] SPI interface", version 5.2.4.1):
 *
 *   read      [0xFB][address, 32 bits big endian]  then the chip sends 1 dummy byte followed
 *             by the data -- all inside ONE chip-select session, CS staying low throughout
 *   write     [0xFA][address, 32 bits big endian][data]
 *   chip id   2 bytes at 0x20000000, which are 0x36 0x39 (DCHIP_ID_0/1, FTM5)
 *   FIFO      [0x87], then 1 dummy byte + 8 bytes per event; the first event's byte 7,
 *             bits 4:0, say how many events are still queued (cap 31); the rest are read
 *             with the 0x87 command sent again, again behind one dummy byte
 *   events    8 bytes: [id][(touchId << 4) | type][...]
 *             enter 0x13, motion 0x23, leave 0x33; controller ready 0x03, error 0xf3
 *             x = ((e[3] & 0x0f) << 8) | e[2]      y = (e[4] << 4) | ((e[3] & 0xf0) >> 4)
 *             (12 bits, in the panel's own 1440 x 3200; type 1 = finger, 2 = glove)
 *   boot ROM  a chip with no application firmware answers in 3-wire SPI mode (its data comes
 *             back on MOSI, so 4-wire reads see nothing). It is switched to 4-wire with six
 *             writes (vendor ftsFlash.c hold_m3 / fts_spi4_mode_set):
 *             0x20000024=01 (hold the MCU), 0x20000032=10, 0x20000034=02, 0x2000003f=07,
 *             0x2000003d=30, then the interface register 0x2000002d=02, and 1 ms of quiet
 *
 * The bus is the SE11 GENI engine on pins 48..51 (the phone's own device tree:
 * soc/spi@a8c000 with pinctrl qupv3_se11_spi_active, function qup11 = touch_func=N), the
 * supply ldoc2 at 1.8 V and the analog one through gpio132 -- all of which the code above
 * already provides (power_on, clocks_on, use_engine_pins).
 *
 * Not here yet: downloading st_fts_k2.ftb into a chip whose flash holds no firmware
 * (vendor flashProcedure). The chip that boots Android normally has its firmware in its
 * own flash and answers at once.
 */

#define ST_CMD_W          0xfa          /* write an address */
#define ST_CMD_R          0xfb          /* read an address */
#define ST_CMD_FIFO       0x87          /* read all the events queued */
#define ST_CMD_SCAN_MODE  0xa0          /* opcode: set the scan mode, then mode and settings */
#define ST_SCAN_ACTIVE    0x00
#define ST_SCAN_MT_ON     0x01          /* ACTIVE_MULTI_TOUCH: MS/SS scan */
#define ST_ADDR_CHIP_ID   0x20000000u
#define ST_ADDR_RESET     0x20000024u
#define ST_ADDR_ICR       0x2000002du
#define ST_ADDR_GPIO_DIR  0x20000032u
#define ST_ADDR_GPIO_PU   0x20000034u
#define ST_ADDR_GPIO_CFG0 0x2000003du
#define ST_ADDR_GPIO_CFG2 0x2000003fu
#define ST_EVENT_SIZE     8
#define ST_EVENTS_MAX     32          /* the chip's FIFO depth */
#define ST_EVT_ENTER      0x13
#define ST_EVT_MOTION     0x23
#define ST_EVT_LEAVE      0x33
#define ST_TYPE_FINGER    0x01
#define ST_TYPE_GLOVE     0x02
#define ST_ID_HIGH        0x36        /* the chip id bytes, in the order the chip sends them */
#define ST_ID_LOW         0x39

#define ST_PANEL_X        1440   /* the panel the chip reports in (fts,x-max) */
#define ST_PANEL_Y        3200   /* (fts,y-max) */

static uint8_t st_tx[1 + 4 + 1 + ST_EVENT_SIZE * ST_EVENTS_MAX];
static uint8_t st_rx[1 + 4 + 1 + ST_EVENT_SIZE * ST_EVENTS_MAX];

/* One chip-select session: cmdlen bytes out, then datalen bytes in (zeros clocked out),
 * CS held low for the whole of it -- the vendor's fts_writeRead on one spi_message. */
static bool (*st_xfer)(const uint8_t *cmd, uint32_t cmdlen, uint8_t *data, uint32_t datalen);

static bool
st_engine_transfer(const uint8_t *cmd, uint32_t cmdlen, uint8_t *data, uint32_t datalen)
{
	uint32_t total = cmdlen + datalen;

	if (total > sizeof(st_tx)) {
		return false;
	}
	memcpy(st_tx, cmd, cmdlen);
	memset(st_tx + cmdlen, 0, datalen);
	if (!spi_transfer(st_tx, st_rx, total)) {
		return false;
	}
	memcpy(data, st_rx + cmdlen, datalen);
	sleep_us(10);           /* the vendor's CS-high time between sessions */
	return true;
}

static bool
st_bitbang_transfer(const uint8_t *cmd, uint32_t cmdlen, uint8_t *data, uint32_t datalen)
{
	uint32_t i;

	gpio_set(PIN_CS, false);
	for (i = 0; i < cmdlen; i++) {
		uint8_t byte = cmd[i];

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, byte >> bit & 1);
			gpio_set(PIN_CLK, true);
			sleep_us(1);
			gpio_set(PIN_CLK, false);
			sleep_us(1);
		}
	}
	sleep_us(10);           /* SPI_DELAY_CS: the chip needs this gap before it starts answering */
	for (i = 0; i < datalen; i++) {
		uint8_t byte = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, false);
			gpio_set(PIN_CLK, true);
			sleep_us(1);
			byte = (uint8_t)(byte << 1 | (gpio_get(bitbang_in) ? 1 : 0));
			gpio_set(PIN_CLK, false);
			sleep_us(1);
		}
		data[i] = byte;
	}
	gpio_set(PIN_CS, true);
	return true;
}

/* Read: [0xFB][address] then the chip's 1 dummy byte and the data, one session. */
static bool
st_read32(uint32_t address, uint8_t *buf, uint32_t length)
{
	uint8_t cmd[5] = { ST_CMD_R, (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8),
		(uint8_t)address };

	if (length + 1 > sizeof(st_rx)) {
		return false;
	}
	if (!st_xfer(cmd, sizeof(cmd), st_rx, length + 1)) {
		return false;
	}
	memcpy(buf, st_rx + 1, length);         /* the first byte back is the dummy */
	return true;
}

static bool
st_write_reg(uint32_t address, uint8_t value)
{
	uint8_t cmd[6] = { ST_CMD_W, (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8),
		(uint8_t)address, value };

	return st_xfer(cmd, sizeof(cmd), st_rx, 0);
}

/* A raw opcode frame, no address: [0xA0][mode][settings] and friends. */
static bool
st_raw_write(const uint8_t *cmd, uint32_t len)
{
	return st_xfer(cmd, len, st_rx, 0);
}

/* A chip that came up in 3-wire SPI is taken to 4-wire with these six writes. */
static void
st_switch_to_spi4(void)
{
	redwood_tl("st: switching the chip to 4-wire SPI\n", (uint64_t)0);
	st_write_reg(ST_ADDR_RESET, 0x01);      /* hold the MCU (vendor hold_m3) */
	st_write_reg(ST_ADDR_GPIO_DIR, 0x10);
	st_write_reg(ST_ADDR_GPIO_PU, 0x02);
	st_write_reg(ST_ADDR_GPIO_CFG2, 0x07);
	st_write_reg(ST_ADDR_GPIO_CFG0, 0x30);
	st_write_reg(ST_ADDR_ICR, 0x02);        /* 4-wire */
	sleep_ms(1);
}

static bool
st_chip_id(uint8_t *id)
{
	if (!st_read32(ST_ADDR_CHIP_ID, id, 2)) {
		return false;
	}
	return id[0] == ST_ID_HIGH && id[1] == ST_ID_LOW;
}

/* The events queued in the chip, up to ST_EVENTS_MAX; count = how many valid. */
static bool
st_fifo_events(uint8_t *events, unsigned *count)
{
	const uint8_t cmd = ST_CMD_FIFO;
	unsigned remaining;

	if (!st_xfer(&cmd, 1, st_rx, 1 + ST_EVENT_SIZE)) {
		return false;
	}
	memcpy(events, st_rx + 1, ST_EVENT_SIZE);
	remaining = events[7] & 0x1f;           /* events still queued behind this one */
	if (remaining > ST_EVENTS_MAX - 1) {
		remaining = ST_EVENTS_MAX - 1;
	}
	if (remaining) {
		/* the vendor sends the FIFO command again for the rest, one dummy byte first */
		if (!st_xfer(&cmd, 1, st_rx, 1 + ST_EVENT_SIZE * remaining)) {
			remaining = 0;
		} else {
			memcpy(events + ST_EVENT_SIZE, st_rx + 1, ST_EVENT_SIZE * remaining);
		}
	}
	*count = remaining + 1;
	return true;
}

/* One touch, scaled to the 0..65535 the /dev/redwood-touch records use. */
static void
st_post(unsigned type, unsigned id, unsigned x, unsigned y)
{
	if (id >= 10) {
		return;
	}
	if (x >= ST_PANEL_X) {
		x = ST_PANEL_X - 1;
	}
	if (y >= ST_PANEL_Y) {
		y = ST_PANEL_Y - 1;
	}
	x = (unsigned)((uint64_t)x * 65535 / (ST_PANEL_X - 1));
	y = (unsigned)((uint64_t)y * 65535 / (ST_PANEL_Y - 1));
	if (flip & 1) {
		x = 65535 - x;
	}
	if (flip & 2) {
		y = 65535 - y;
	}
	redwood_touch_post(type, id, x, y);
	seen.touches++;
	seen.last_x = x;
	seen.last_y = y;
}

static void
st_event(const uint8_t *e)
{
	unsigned id = (e[1] & 0xf0) >> 4;
	unsigned kind = e[1] & 0x0f;
	unsigned x = ((unsigned)(e[3] & 0x0f) << 8) | e[2];
	unsigned y = ((unsigned)e[4] << 4) | ((e[3] & 0xf0) >> 4);

	if (kind != ST_TYPE_FINGER && kind != ST_TYPE_GLOVE) {
		return;             /* stylus, palm, hover: not a touch for iOS */
	}
	switch (e[0]) {
	case ST_EVT_ENTER:
		st_post(REDWOOD_TOUCH_DOWN, id, x, y);
		break;
	case ST_EVT_MOTION:
		st_post(REDWOOD_TOUCH_MOVE, id, x, y);
		break;
	case ST_EVT_LEAVE:
		st_post(REDWOOD_TOUCH_UP, id, x, y);
		break;
	default:
		break;              /* controller ready, status, user report, error: nothing to post */
	}
}

static void
st_poll(void)
{
	uint8_t events[ST_EVENT_SIZE * ST_EVENTS_MAX];
	unsigned count = 0;

	seen.polls++;
	if (!st_fifo_events(events, &count)) {
		seen.bad++;
		return;
	}
	seen.reports++;
	if (count) {
		static int dump = -1;
		static unsigned shown;

		if (dump < 0) {
			uint32_t on = 0;

			dump = PE_parse_boot_argn("touch_dump", &on, sizeof(on)) && on != 0;
		}
		if (dump && shown < 6000) {
			for (unsigned i = 0; i < count; i++) {
				const uint8_t *e = events + i * ST_EVENT_SIZE;

				if (!(e[0] | e[1] | e[2] | e[3] | e[4] | e[5] | e[6] | e[7])) {
					continue;       /* an idle poll: the FIFO had nothing */
				}
				redwood_tl("st: raw %02x %02x %02x %02x %02x %02x %02x %02x\n",
				    (uint64_t)e[0], (uint64_t)e[1], (uint64_t)e[2], (uint64_t)e[3],
				    (uint64_t)e[4], (uint64_t)e[5], (uint64_t)e[6], (uint64_t)e[7]);
				shown++;
			}
		}
	}
	for (unsigned i = 0; i < count; i++) {
		st_event(events + i * ST_EVENT_SIZE);
	}
}

/* Look for the chip and its answer, on whichever transfer path st_xfer points at. */
static bool
st_probe(void)
{
	uint8_t id[2] = { 0, 0 };

	for (unsigned tries = 0; tries < 8; tries++) {
		if (st_chip_id(id)) {
			redwood_tl("st: chip answered, id %02x %02x (try %u)\n", (uint64_t)id[0],
			    (uint64_t)id[1], (uint64_t)tries);
			return true;
		}
		redwood_tl("st: id read %02x %02x (try %u)\n", (uint64_t)id[0], (uint64_t)id[1],
		    (uint64_t)tries);
		sleep_ms(100);
	}
	return false;
}

/* The chip is there and its firmware runs: reset it the way the vendor does (the reset GPIO,
 * fts_system_reset), wait for its CONTROLLER_READY event, and start its scanning (vendor
 * fts_init_sensing: setScanMode(ACTIVE, 0), a pause, setScanMode(ACTIVE, ACTIVE_MULTI_TOUCH)).
 */
static bool
st_start(void)
{
	uint8_t events[ST_EVENT_SIZE * ST_EVENTS_MAX];
	unsigned count;
	bool ready = false;

	gpio_set(PIN_RESET, false);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
	for (unsigned tries = 0; tries < 60 && !ready; tries++) {
		if (!st_fifo_events(events, &count)) {
			break;          /* the chip is rebooting; the bus will answer again in a moment */
		}
		for (unsigned i = 0; i < count; i++) {
			if (events[i * ST_EVENT_SIZE] == 0x03) {        /* EVT_ID_CONTROLLER_READY */
				ready = true;
				break;
			}
		}
		if (!ready) {
			sleep_ms(50);
		}
	}
	redwood_tl("st: controller %s\n", ready ? "ready" : "never said ready");
	{
		const uint8_t scan[3] = { ST_CMD_SCAN_MODE, ST_SCAN_ACTIVE, ST_SCAN_MT_ON };

		st_raw_write(scan, 2);          /* the vendor sends ACTIVE/0, a pause, then ACTIVE/MT */
		sleep_ms(30);
		st_raw_write(scan, sizeof(scan));
	}
	redwood_tl("st: active scan on\n", (uint64_t)0);
	return ready;
}

static bool
st_bring_up(void)
{
	const char *problem = NULL;
	uint32_t spi_hz = 1000000;

	redwood_tl("st: SE11 at %#x, pins %u/%u/%u/%u, reset %u, interrupt %u, mux %u\n",
	    (uint64_t)SE_PHYS, (uint64_t)PIN_MOSI, (uint64_t)PIN_MISO, (uint64_t)PIN_CLK, (uint64_t)PIN_CS,
	    (uint64_t)PIN_RESET, (uint64_t)PIN_INT, (uint64_t)pin_function_spi);
	power_on();
	gpio_output(PIN_RESET, true, PULL_UP, 8);       /* out of reset; the chip boots its firmware */
	sleep_ms(100);
	if (!clocks_on()) {
		redwood_tl("st: no clocks\n", (uint64_t)0);
		return false;
	}
	PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
	if (spi_hz < 100000 || spi_hz > 10000000) {
		spi_hz = 1000000;
	}

	/* the serial engine first */
	use_engine_pins();
	problem = geni_spi_init(spi_hz);
	if (problem == NULL) {
		spi_transfer = geni_transfer;
		st_xfer = st_engine_transfer;
		if (st_probe()) {
			return st_start();
		}
		redwood_tl("st: no answer on the serial engine; driving the pins by hand\n", (uint64_t)0);
	} else {
		redwood_tl("st: no serial engine: %s; driving the pins by hand\n", problem);
	}

	/* by hand; which of the two data pins is the input is not written down anywhere, so try both */
	st_xfer = st_bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		if (st_probe()) {
			redwood_tl("st: chip answers on pins driven by hand (data in on gpio%u)\n",
			    (uint64_t)bitbang_in);
			return st_start();
		}
	}

	/* still nothing: the chip probably sits in its boot ROM, which answers in 3-wire SPI (its
	 * data line is the MOSI one). Write it into 4-wire mode -- those writes leave on MOSI and
	 * reach a 3-wire chip -- and look again, on the engine and on the pins. */
	redwood_tl("st: no answer at all; switching the chip to 4-wire SPI and trying again\n", (uint64_t)0);
	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	st_switch_to_spi4();
	if (problem == NULL) {
		use_engine_pins();
		spi_transfer = geni_transfer;
		st_xfer = st_engine_transfer;
		if (st_probe()) {
			return st_start();
		}
	}
	st_xfer = st_bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		if (st_probe()) {
			redwood_tl("st: chip answers after the 4-wire switch (data in on gpio%u)\n",
			    (uint64_t)bitbang_in);
			return st_start();
		}
	}
	return false;
}

static void
st_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 8000, fn = PIN_FUNCTION_SPI;
	unsigned idle = 0;

	if (PE_parse_boot_argn("touch_func", &fn, sizeof(fn)) && fn < 16) {
		pin_function_spi = fn;
	}
	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (!st_bring_up()) {
		redwood_tl("st: no ST FTS touch screen\n", (uint64_t)0);
		thread_terminate(current_thread());
		return;
	}
	redwood_tl("st: touch screen ready; the interrupt line is looked at every %u us\n",
	    (uint64_t)poll_us);
	for (unsigned tick = 1;; tick++) {
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			st_poll();
		} else if (++idle >= 10) {              /* look anyway every so often, in case it is missed */
			idle = 0;
			st_poll();
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("st: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u;"
			    " interrupt line %s\n", seen.reports, seen.polls, seen.bad, seen.touches, seen.last_x,
			    seen.last_y, gpio_get(PIN_INT) ? "high" : "low");
		}
		sleep_us(poll_us);
	}
}
