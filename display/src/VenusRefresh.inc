/*
 * Venus K2_0A_MP refresh bring-up. Included by MiniIOMobileFramebuffer.cpp
 * after the Venus DSI clock and backlight support. Register layout follows
 * the SM8350 command-mode path in b1ackm4x/hackios-xnu-kernel (020124f).
 */
#include "VenusRefreshStock.inc"

static vm_offset_t gVenusIntf, gVenusTe;
static uint32_t gVenusRefreshSync, gVenusRefreshAuto;

static inline uint32_t
vrRead(vm_offset_t base, unsigned reg)
{
	return *(volatile uint32_t *)(base + reg);
}

static inline void
vrWrite(vm_offset_t base, unsigned reg, uint32_t value)
{
	*(volatile uint32_t *)(base + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

enum {
	vrFrame = 0xac, vrLine = 0xb0, vrPp = 0x25c,
	vrSync = 0x288, vrHeight = 0x28c, vrInit = 0x290,
	vrReadLine = 0x29c, vrTearLine = 0x2b0, vrAuto = 0x2b4,
};

/* Count the physical panel's GPIO82 TE and the independent DPU frame
 * counter together. The GPIO read does not change pin mux or IRQ state. */
static uint32_t
vrMeasure(const char *phase, unsigned edges, unsigned limitUs)
{
	const uint32_t firstFrame = vrRead(gVenusIntf, vrFrame);
	uint64_t start = mach_absolute_time(), first = 0, last = 0, elapsed;
	unsigned count = 0;
	bool high = vrRead(gVenusTe, 4) & 1;
	do {
		bool next = vrRead(gVenusTe, 4) & 1;
		uint64_t now = mach_absolute_time();
		if (next && !high) {
			if (!first) first = now;
			else if (++count == edges) { last = now; break; }
			last = now;
		}
		high = next;
		elapsed = micros(now - start);
		delay(4);
	} while (elapsed < limitUs);
	const uint64_t span = first && last ? micros(last - first) : 0;
	const uint32_t mhz = span ? (uint32_t)(1000000000ULL * count / span) : 0;
	const uint32_t frames = vrRead(gVenusIntf, vrFrame) - firstFrame;
	venusBacklightLog("venus-120: %s gpio_edges=%u time_us=%llu millihz=%u dpu_frames=%u line=%u tear_line=%u\n",
	    phase, count, span, mhz, frames, vrRead(gVenusIntf, vrLine), vrRead(gVenusIntf, vrTearLine) & 0xffff);
	/* The panel can pulse even if command transfers stall. Allow two frames
	 * for the start/end sample phases, but require the DPU to follow TE. */
	return count == edges && frames >= count - 2 && frames <= count + 2 ? mhz : 0;
}

static bool
vrStopAutorefresh(uint32_t sync, uint32_t automatic)
{
	vrWrite(gVenusIntf, vrSync, sync & ~(1u << 20));
	vrWrite(gVenusIntf, vrAuto, automatic & ~(1u << 31));
	for (unsigned i = 0; i < 550; ++i) {
		const uint32_t line = vrRead(gVenusIntf, vrTearLine) & 0xffff;
		if ((!line || line >= 3200) && !(vrRead(gVenusDsi, 8) & (1u << 2))) return true;
		delay(50);
	}
	return false;
}

static bool
vrSend(const VRPacket *packets, unsigned count)
{
	const vm_offset_t dsi = gVenusDsi;
	const uint32_t offsets[] = {0x3c, 0x4c, 0x84, 0x11c, 0x15c};
	uint32_t saved[sizeof(offsets) / sizeof(offsets[0])];
	for (unsigned i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) saved[i] = vrRead(dsi, offsets[i]);
	const uint32_t mask = vrRead(dsi, 0x110) & 0xaaaaaaaaU;
	vrWrite(dsi, 0x11c, saved[3] | VENUS_DSI_CLK_FORCE_MASK);
	vrWrite(dsi, 0x3c, (saved[0] & ~0xc4000000U) | 0x14000000U); /* LP embedded FIFO */
	vrWrite(dsi, 0x84, (saved[2] & ~7U) | 4U);
	bool ok = true;
	/* Keep the lock commands at the end even if an earlier transfer failed. */
	for (unsigned n = 0; n < count; ++n) {
		if (!ok && n < count - 2) continue;
		const VRPacket &packet = packets[n];
		uint32_t header = 0x80000000U | (packet.type << 16);
		if (packet.type == 0x39) header |= 0x40000000U | packet.length;
		else header |= packet.payload & 0xffffU;
		vrWrite(dsi, 0x110, mask | 1U);
		vrWrite(dsi, 0x15c, 0);
		vrWrite(dsi, 0x1ec, 1); delay(1); vrWrite(dsi, 0x1ec, 0);
		vrWrite(dsi, 0x15c, 0x30006);
		vrWrite(dsi, 0x17c, header);
		vrWrite(dsi, 0x17c, packet.type == 0x39 ? packet.payload : 0); /* even DWORDs in FIFO */
		vrWrite(dsi, 0x4c, packet.type == 0x39 ? 8 : 4);
		vrWrite(dsi, 0x90, 1);
		unsigned waited = 0;
		while (!(vrRead(dsi, 0x110) & 1U) && waited < 5000) { delay(10); waited += 10; }
		uint32_t intr = vrRead(dsi, 0x110), fifo = vrRead(dsi, 0xc);
		bool done = (intr & 1U) && !(intr & (1u << 24)) && !(fifo & 0xcccc0480U) &&
		    !vrRead(dsi, 0x68) && !(vrRead(dsi, 0xb4) & 0x11111) && !(vrRead(dsi, 0xc0) & 0x111);
		vrWrite(dsi, 0x110, mask | 1U);
		if (!done) venusBacklightLog("venus-120: command failed n=%u header=%#x intr=%#x fifo=%#x wait=%u\n",
		    n, header, intr, fifo, waited);
		ok &= done;
		if (packet.waitMs) delay(packet.waitMs * 1000);
	}
	vrWrite(dsi, 0x15c, 0);
	vrWrite(dsi, 0x1ec, 1); delay(1); vrWrite(dsi, 0x1ec, 0);
	for (unsigned i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) vrWrite(dsi, offsets[i], saved[i]);
	for (unsigned i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) ok &= vrRead(dsi, offsets[i]) == saved[i];
	gVenusBacklightFifoReady = false;
	return ok;
}

static bool
vrResumeAutorefresh(uint32_t sync)
{
	vrWrite(gVenusIntf, vrSync, sync & ~(1u << 20));
	vrWrite(gVenusIntf, vrAuto, gVenusRefreshAuto);
	vrWrite(gVenusIntf, vrSync, sync);
	return vrRead(gVenusIntf, vrSync) == sync && vrRead(gVenusIntf, vrAuto) == gVenusRefreshAuto;
}

/* loader_return first parks the other processors. The operation cannot take
 * an IOLock there. Mask interrupts around the shared DSI transaction on the
 * live path so the return handler sees either side of it, never half a FIFO. */
extern "C" void venus_refresh_quiesce(void);
extern "C" void
venus_refresh_quiesce(void)
{
	if (!gVenusRefreshOwned) return;
	bool enabled = ml_set_interrupts_enabled(false);
	gVenusMeasuredHz = 0;
	bool idle = vrStopAutorefresh(vrRead(gVenusIntf, vrSync), gVenusRefreshAuto);
	bool sent = idle && vrSend(vrStock60, sizeof(vrStock60) / sizeof(vrStock60[0]));
	bool restored = vrResumeAutorefresh(gVenusRefreshSync);
	gVenusRefreshOwned = !sent || !restored;
	ml_set_interrupts_enabled(enabled);
	venusBacklightLog("venus-120: restored requested=60 idle=%u commands=%u registers=%u\n", idle, sent, restored);
	if (sent && restored) (void)vrMeasure("restored", 4, 150000);
}

static void
vrPresentSample(uint64_t now, uint64_t compose, uint64_t copy)
{
	static uint64_t begin, composeSum, copySum;
	static uint32_t frames, firstDpu;
	const uint32_t dpu = vrRead(gVenusIntf, vrFrame);
	if (!begin) { begin = now; firstDpu = dpu; return; }
	++frames; composeSum += compose; copySum += copy;
	uint64_t us = micros(now - begin);
	if (us < 2000000) return;
	venusBacklightLog("venus-120: presenter swaps=%u dpu_frames=%u time_us=%llu millifps=%llu compose_us=%llu copy_us=%llu at_us=%llu\n",
	    frames, dpu - firstDpu, us, frames * 1000000000ULL / us,
	    micros(composeSum) / frames, micros(copySum) / frames, micros(now));
	begin = now; firstDpu = dpu; frames = 0; composeSum = copySum = 0;
}

#include "VenusDpu.inc"

static void
venusRefreshStart(void)
{
	if (!gVenusRefreshMode || gVenusRefreshMode > 10 || !gVenusDsi ||
	    vinfo.v_width != 1440 || vinfo.v_height != 3200) return;
	vm_offset_t mdss = ml_io_map(0x0ae00000, 0x1000);
	gVenusIntf = ml_io_map(0x0ae36000, 0x1000);
	gVenusTe = ml_io_map(0x0f152000, 0x1000); /* SM8350 TLMM tile + 82 * 0x1000 */
	if (!mdss || !gVenusIntf || !gVenusTe) return;
	const uint32_t dpu = vrRead(mdss, 0);
	venusBacklightLog("venus-120: survey mode=%u dpu=%#x dsi=%#x ctrl=%#x gpio82=%#x\n",
	    gVenusRefreshMode, dpu, vrRead(gVenusDsi, 0), vrRead(gVenusDsi, 4), vrRead(gVenusTe, 0));
	if ((dpu >> 28) != 7) return;
	venusBacklightLog("venus-120: intf1 sync=%#x height=%u init=%u rd=%u auto=%#x pp=%#x frame=%u line=%u tear=%u\n",
	    vrRead(gVenusIntf, vrSync), vrRead(gVenusIntf, vrHeight), vrRead(gVenusIntf, vrInit),
	    vrRead(gVenusIntf, vrReadLine), vrRead(gVenusIntf, vrAuto), vrRead(gVenusIntf, vrPp),
	    vrRead(gVenusIntf, vrFrame), vrRead(gVenusIntf, vrLine), vrRead(gVenusIntf, vrTearLine));
	venusBacklightLog("venus-120: dsi stream=%#x tot=%#x dsc=%#x status=%#x fifo=%#x dma=%#x dma_trigger=%#x tpg=%#x int=%#x\n",
	    vrRead(gVenusDsi, 0x58), vrRead(gVenusDsi, 0x5c), vrRead(gVenusDsi, 0x2a8),
	    vrRead(gVenusDsi, 8), vrRead(gVenusDsi, 0xc), vrRead(gVenusDsi, 0x3c),
	    vrRead(gVenusDsi, 0x84), vrRead(gVenusDsi, 0x15c), vrRead(gVenusDsi, 0x110));
	const uint32_t baseline = vrMeasure("baseline", 32, 850000);
	if (gVenusRefreshMode < 2) return;
	/* K2_0A_MP selected by the host; these checks pin the exact verified
	 * Venus firmware handoff, including the inherited GOP autorefresh. */
	if (baseline < 58000 || baseline > 62000 || dpu != 0x70000000 || vrRead(gVenusTe, 0) != 4 ||
	    vrRead(gVenusDsi, 0) != 0x20050000 || vrRead(gVenusDsi, 4) != 0x1f5 ||
	    vrRead(gVenusDsi, 0x58) != 0x2d10039 || vrRead(gVenusDsi, 0x5c) != 0x0c8000f0 ||
	    vrRead(gVenusDsi, 0x2a8) != 0x3941 || vrRead(gVenusDsi, 0x15c) ||
	    vrRead(gVenusIntf, vrPp) != 0xf0000 || vrRead(gVenusIntf, vrSync) != 0x180064 ||
	    vrRead(gVenusIntf, vrHeight) != 65535 || vrRead(gVenusIntf, vrAuto) != 0x80000001) {
		venusBacklightLog("venus-120: K2_0A_MP firmware handoff guard refused\n");
		return;
	}
	IOLockLock(gVenusBacklightLock);
	bool enabled = ml_set_interrupts_enabled(false);
	gVenusRefreshSync = vrRead(gVenusIntf, vrSync);
	gVenusRefreshAuto = vrRead(gVenusIntf, vrAuto);
	gVenusRefreshOwned = true;
	bool idle = vrStopAutorefresh(gVenusRefreshSync, gVenusRefreshAuto);
	bool sent = idle && vrSend(vrStock120, sizeof(vrStock120) / sizeof(vrStock120[0]));
	/* Rescale the 19.2 MHz / 3200 line TE counter; preserve the geometry,
	 * PLL/PHY, routing, DSC and existing framebuffer DMA configuration. */
	bool resumed = vrResumeAutorefresh(sent ? (gVenusRefreshSync & ~0x7ffffU) | 50U : gVenusRefreshSync);
	ml_set_interrupts_enabled(enabled);
	venusBacklightLog("venus-120: switch requested=120 idle=%u commands=%u resumed=%u sync=%#x\n",
	    idle, sent, resumed, vrRead(gVenusIntf, vrSync));
	if (sent && resumed) {
		/* GPIO pulses may be missed while a CPU services an interrupt. A
		 * rejected observation may be repeated, never treated as 120 Hz. */
		for (unsigned attempt = 0; attempt < 3 && !gVenusMeasuredHz; ++attempt) {
			uint32_t rate = vrMeasure("switched", 64, 850000);
			if (rate >= 118000 && rate <= 122000) gVenusMeasuredHz = 120;
		}
	}
	if (!gVenusMeasuredHz) venus_refresh_quiesce();
	IOLockUnlock(gVenusBacklightLock);
	venusBacklightLog("venus-120: publish measured_hz=%u\n", gVenusMeasuredHz);
	if (gVenusMeasuredHz && gVenusRefreshMode >= 5) {
		vdSurvey();
		if (gVenusRefreshMode >= 6) vdAcquire();
	}
}
