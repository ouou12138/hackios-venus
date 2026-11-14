/* SM8350 scanout survey and staged takeover. All offsets are MDP relative
 * unless the register belongs to the interface or the Apps SMMU. */
static vm_offset_t vdMdp, vdSmmu, vdCb;
static const unsigned vdPipes[] = {0x4000, 0x6000, 0x8000, 0xa000, 0x24000, 0x26000, 0x28000, 0x2a000};
static const unsigned vdCbRegs[] = {0, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x38, 0x3c};
static constexpr uint32_t vdDone = 1u << 8;
static constexpr uint32_t vdUnderrun = 1u << 26;
static constexpr uint32_t vdFault = 0xc00001fe;
static constexpr uint64_t vdGop = 0xe2a00000;
static constexpr uint32_t vdGopBytes = 5760*3200;
static constexpr uint32_t vdPitch = 4544, vdWidth = 1125, vdHeight = 2436;
/* One repeated last row plus repeated right-edge pixels supplies the
 * bilinear taps of an upscaler without fetching outside the owned buffer. */
static constexpr uint32_t vdImageBytes = vdPitch * (vdHeight + 1);
static constexpr uint64_t vdIova[] = {0x10000000, 0x12000000};
static constexpr uint64_t vdPhysMask = 0x0000000ffffff000ULL;
static constexpr unsigned vdTableCount = 48;
static const unsigned vdScaleRegs[] = {0, 8, 0x24, 0x100, 0x104, 0x108, 0x110, 0x114, 0x118,
    0x120, 0x124, 0x128, 0xa10, 0xa14, 0xa18, 0xa1c, 0xa20, 0xa40, 0xa44, 0xa48,
    0xa90, 0xa94, 0xa98, 0xa9c, 0xa04};
static struct {
	IOBufferMemoryDescriptor *tables, *images[2];
	uint64_t *table;
	uint64_t pagePA[vdTableCount];
	unsigned pages;
	uint32_t cb[9], scale[2][sizeof(vdScaleRegs)/sizeof(vdScaleRegs[0])];
	uint32_t sync, automatic, misr, flush;
	uint32_t pendingFirst, pendingAddress;
	bool pending;
	bool owned, translated, scaled, ready, failed;
	unsigned next;
	uint64_t total, changes;
	uint32_t lastCrc;
} vd;

static void
vdSurvey(void)
{
	vdMdp = ml_io_map(0x0ae01000, 0x36000);
	vdSmmu = ml_io_map(0x15000000, 0x100000);
	if (!vdMdp || !vdSmmu) return;
	venusBacklightLog("venus-dpu: gop physical=%#llx bytes=%llu mdp route=%#x irq=%#x status=%#x\n",
	    kvtophys(vinfo.v_baseaddr), (uint64_t)vinfo.v_rowbytes * vinfo.v_height,
	    vrRead(vdMdp, 4), vrRead(vdMdp, 0x10), vrRead(vdMdp, 0x14));
	for (unsigned i = 0; i < 6; ++i) {
		vm_offset_t ctl = vdMdp + 0x15000 + i * 0x1000;
		venusBacklightLog("venus-dpu: ctl%u layer=%#x top=%#x flush=%#x start=%#x active=%#x fetch=%#x mask=%#x\n",
		    i, vrRead(ctl, 0), vrRead(ctl, 0x14), vrRead(ctl, 0x18), vrRead(ctl, 0x1c),
		    vrRead(ctl, 0xf4), vrRead(ctl, 0xfc), vrRead(ctl, 0xe8));
	}
	for (unsigned i = 0; i < sizeof(vdPipes)/sizeof(vdPipes[0]); ++i) {
		vm_offset_t p = vdMdp + vdPipes[i];
		venusBacklightLog("venus-dpu: pipe=%#x src=%#x xy=%#x out=%#x outxy=%#x addr=%#x pitch=%#x fmt=%#x unpack=%#x op=%#x\n",
		    vdPipes[i], vrRead(p, 0), vrRead(p, 8), vrRead(p, 0xc), vrRead(p, 0x10),
		    vrRead(p, 0x14), vrRead(p, 0x24), vrRead(p, 0x30), vrRead(p, 0x34), vrRead(p, 0x38));
		if (i < 2) venusBacklightLog("venus-dpu: scaler=%u revision=%#x op=%#x preload=%#x size=%#x dst=%#x\n",
		    i, vrRead(p, 0xa00), vrRead(p, 0xa04), vrRead(p, 0xa20), vrRead(p, 0xa40), vrRead(p, 0xa48));
	}
	venusBacklightLog("venus-dpu: interface pp=%#x sync=%#x auto=%#x misr_ctrl=%#x frame=%u\n",
	    vrRead(gVenusIntf, vrPp), vrRead(gVenusIntf, vrSync), vrRead(gVenusIntf, vrAuto),
	    vrRead(gVenusIntf, 0x180), vrRead(gVenusIntf, vrFrame));
	uint32_t id0 = vrRead(vdSmmu, 0x20), id1 = vrRead(vdSmmu, 0x24), control = vrRead(vdSmmu, 0);
	venusBacklightLog("venus-dpu: smmu control=%#x id0=%#x id1=%#x id2=%#x global_fault=%#x\n",
	    control, id0, id1, vrRead(vdSmmu, 0x28), vrRead(vdSmmu, 0x48));
	if (id0 != 0x4c017e59 || id1 != 0x6000004b || (control & 9)) return;
	const uint32_t sid[] = {0x820, 0x822, 0xc20, 0xc22};
	unsigned coverage[4] = {};
	for (unsigned i = 0; i < (id0 & 255); ++i) {
		uint32_t smr = vrRead(vdSmmu, 0x800 + 4*i), s2cr = vrRead(vdSmmu, 0xc00 + 4*i);
		if (!(smr & (1u << 31))) continue;
		uint32_t mask = (smr >> 16) & 0x7fff, hit = 0;
		for (unsigned j = 0; j < 4; ++j)
			if (!(((smr & 0x7fff) ^ sid[j]) & ~mask)) { ++coverage[j]; hit |= 1u << j; }
		if (!hit) continue;
		unsigned bank = s2cr & 255;
		venusBacklightLog("venus-dpu: match=%u smr=%#x s2cr=%#x covers=%#x\n", i, smr, s2cr, hit);
		if ((s2cr & 0x30000) || bank >= (id1 & 255)) continue;
		vm_offset_t cb = vdSmmu + 0x80000 + bank*0x1000;
		venusBacklightLog("venus-dpu: cb=%u cbar=%#x cba2r=%#x sctlr=%#x tcr2=%#x tw0=%#x:%#x tw1=%#x:%#x tcr=%#x mair=%#x:%#x fsr=%#x\n",
		    bank, vrRead(vdSmmu, 0x1000 + bank*4), vrRead(vdSmmu, 0x1800 + bank*4),
		    vrRead(cb, 0), vrRead(cb, 0x10), vrRead(cb, 0x24), vrRead(cb, 0x20), vrRead(cb, 0x2c),
		    vrRead(cb, 0x28), vrRead(cb, 0x30), vrRead(cb, 0x3c), vrRead(cb, 0x38), vrRead(cb, 0x58));
	}
	venusBacklightLog("venus-dpu: survey done coverage=%u/%u/%u/%u writes=0\n",
	    coverage[0], coverage[1], coverage[2], coverage[3]);
	if (coverage[0] == 1 && coverage[1] == 1 && coverage[2] == 1 && coverage[3] == 1 &&
	    vrRead(vdSmmu, 0x810) == 0x84020820 && vrRead(vdSmmu, 0xc10) == 4)
		vdCb = vdSmmu + 0x84000;
}

static inline vm_offset_t
vdCtl(void)
{
	return vdMdp + 0x15000;
}

static inline uint32_t
vdRound4K(uint32_t bytes)
{
	return (bytes + 4095) & ~4095u;
}

static void
vdClean(const void *bytes, uint32_t len)
{
	const uintptr_t begin = (uintptr_t)bytes & ~63UL;
	for (uintptr_t p = begin; p < (uintptr_t)bytes + len; p += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t *
vdLeaf(uint64_t iova, bool create)
{
	uint64_t *table = vd.table;
	for (unsigned shift = 30; shift >= 21; shift -= 9) {
		uint64_t &pte = table[(iova >> shift) & 511];
		if (!pte && create && vd.pages < vdTableCount) {
			pte = vd.pagePA[vd.pages++] | 3;
		}
		if ((pte & 3) != 3) return nullptr;
		unsigned n = 1;
		for (; n < vd.pages && vd.pagePA[n] != (pte & vdPhysMask); ++n) {}
		if (n == vd.pages) return nullptr;
		table = vd.table + n*512;
	}
	return &table[(iova >> 12) & 511];
}

/* Scanout only: no writes, no speculative data map beyond its last page. */
static bool
vdMap(uint64_t iova, const void *memory, uint32_t bytes)
{
	const uint32_t span = vdRound4K(bytes);
	for (unsigned off = 0; off < span; off += 4096) {
		const uint64_t pa = memory ? kvtophys((vm_offset_t)memory + off) : iova + off;
		uint64_t *entry = vdLeaf(iova + off, true);
		if (!pa || (pa & ~vdPhysMask) || !entry || *entry) return false;
		*entry = pa | 3ULL | (3ULL << 6) | (2ULL << 8) |
		    (1ULL << 10) | (1ULL << 11) | (1ULL << 53) | (1ULL << 54);
		if ((*entry & vdPhysMask) != pa) return false;
	}
	const uint64_t *left = vdLeaf(iova - 4096, false), *right = vdLeaf(iova + span, false);
	return (!left || !*left) && (!right || !*right);
}

static bool
vdTlb(void)
{
	vrWrite(vdCb, 0x610, 4); /* private non-global ASID in inherited display context */
	vrWrite(vdCb, 0x7f0, 0);
	for (unsigned i = 0; i < 1000; ++i) {
		if (!(vrRead(vdCb, 0x7f4) & 1)) return true;
		delay(5);
	}
	return false;
}

static bool
vdFaultFree(void)
{
	return !(vrRead(vdCb, 0x58) & vdFault) && !vrRead(vdSmmu, 0x48) &&
	    !(vrRead(vdMdp, 0x14) & vdUnderrun);
}

static bool
vdIdle(void)
{
	const uint32_t line = vrRead(gVenusIntf, vrTearLine) & 0xffff;
	return vrRead(gVenusIntf, vrLine) == 1 && (!line || line >= 3200) &&
	    !(vrRead(gVenusDsi, 8) & (1u << 2));
}

/* Caller guarantees the preceding DMA completed or was drained. With
 * autorefresh disabled CTL_START makes exactly one PP0 command transfer. */
static void
vdArm(uint32_t address)
{
	vd.pendingFirst = vrRead(gVenusIntf, vrFrame);
	vd.pendingAddress = address;
	vd.pending = true;
	vrWrite(vdMdp + 0x4014, 0, address);
	vrWrite(vdMdp + 0x6014, 0, address);
	vrWrite(vdMdp, 0x18, vdDone);
	vrWrite(vdCtl(), 0x18, 3);
	vrWrite(vdCtl(), 0x1c, 1);
}

static bool
vdComplete(uint32_t *crc)
{
	if (!vd.pending) return false;
	const uint32_t first = vd.pendingFirst, address = vd.pendingAddress;
	for (unsigned i = 0; i < 2200; ++i) {
		const uint32_t diff = vrRead(gVenusIntf, vrFrame) - first;
		if (diff > 1 || !vdFaultFree()) break;
		if (diff == 1 && (vrRead(vdMdp, 0x14) & vdDone) && vdIdle() &&
		    (vrRead(gVenusIntf, 0x180) & 0x300) == 0x300 &&
		    vrRead(vdMdp + 0x4014, 0) == address && vrRead(vdMdp + 0x6014, 0) == address) {
			*crc = vrRead(gVenusIntf, 0x184);
			vd.pending = false;
			return true;
		}
		delay(10);
	}
	venusBacklightLog("venus-dpu: transfer failed address=%#x frames=%u irq=%#x tear=%u line=%u misr=%#x fsr=%#x global=%#x\n",
	    address, vrRead(gVenusIntf, vrFrame) - first, vrRead(vdMdp, 0x14),
	    vrRead(gVenusIntf, vrTearLine), vrRead(gVenusIntf, vrLine), vrRead(gVenusIntf, 0x180),
	    vrRead(vdCb, 0x58), vrRead(vdSmmu, 0x48));
	return false;
}

static bool
vdTransfer(uint32_t address, uint32_t *crc)
{
	if (vd.pending) return false;
	vdArm(address);
	return vdComplete(crc);
}

static void
vdScaler(void)
{
	const uint32_t stepX = (1ULL << 21) * vdWidth / 1440, stepY = (1ULL << 21) * vdHeight / 3200;
	for (unsigned n = 0; n < 2; ++n) {
		const vm_offset_t p = vdMdp + vdPipes[n], sc = p + 0xa00;
		const uint32_t dims = ((vdHeight + 1) << 16) | (n ? 564u : 563u);
		const uint32_t phaseX = n ? 1u << 20 : 0; /* second half starts at 562.5 */
		vrWrite(p, 0, dims); vrWrite(p, 8, n * 562); vrWrite(p, 0x24, vdPitch);
		for (unsigned c = 0; c < 3; ++c) {
			vrWrite(p, 0x100 + c*0x10, 0);
			vrWrite(p, 0x104 + c*0x10, 0);
			vrWrite(p, 0x108 + c*0x10, dims);
		}
		vrWrite(sc, 0x10, stepX); vrWrite(sc, 0x14, stepY);
		vrWrite(sc, 0x18, stepX); vrWrite(sc, 0x1c, stepY);
		vrWrite(sc, 0x20, 0x03050305);
		vrWrite(sc, 0x40, dims); vrWrite(sc, 0x44, dims);
		vrWrite(sc, 0x48, (3200u << 16) | 720u);
		vrWrite(sc, 0x90, phaseX); vrWrite(sc, 0x94, 0);
		vrWrite(sc, 0x98, phaseX); vrWrite(sc, 0x9c, 0);
		vrWrite(sc, 4, 0x20034401);
		venusBacklightLog("venus-dpu: scale pipe=%u size=%#x xy=%#x pitch=%#x op=%#x step=%#x/%#x phase=%#x\n",
		    n, vrRead(p, 0), vrRead(p, 8), vrRead(p, 0x24), vrRead(sc, 4),
		    vrRead(sc, 0x10), vrRead(sc, 0x14), vrRead(sc, 0x90));
	}
}

/* No locks or memory frees on loader return. Keep both DMA buffers wired
 * for the whole session. If a transfer does not drain do not reconfigure
 * the context underneath it, or claim a successful recovery. */
static bool
vdRestore(void)
{
	if (!vd.owned) return true;
	uint32_t crc = 0;
	if (vd.pending && !vdComplete(&crc)) {
		venusBacklightLog("venus-dpu: restore refused: command completion unproven\n");
		return false;
	}
	vrWrite(gVenusIntf, vrSync, vd.sync & ~(1u << 20));
	vrWrite(gVenusIntf, vrAuto, vd.automatic & ~(1u << 31));
	bool idle = false;
	for (unsigned i = 0; i < 2500; ++i) {
		if (vdIdle()) { idle = true; break; }
		delay(10);
	}
	if (!idle) {
		venusBacklightLog("venus-dpu: restore refused: transfer did not drain\n");
		return false;
	}
	if (vd.scaled) {
		for (unsigned n = 0; n < 2; ++n)
			for (unsigned r = 0; r < sizeof(vdScaleRegs)/sizeof(vdScaleRegs[0]); ++r)
				vrWrite(vdMdp + vdPipes[n], vdScaleRegs[r], vd.scale[n][r]);
	}
	vrWrite(vdMdp + 0x4014, 0, vdGop);
	vrWrite(vdMdp + 0x6014, 0, vdGop);
	vrWrite(gVenusIntf, vrSync, vd.sync);
	bool proof = !vd.translated || vdTransfer(vdGop, &crc);
	/* A readback plus observed transfer proves hardware latched the GOP.
	 * If a fault prevents that proof, retain translation and the buffers. */
	if (!proof) return false;
	bool exact = true;
	for (unsigned n = 0; n < 2; ++n) {
		vm_offset_t pipe = vdMdp + vdPipes[n];
		exact &= vrRead(pipe, 0x14) == vdGop;
		if (vd.scaled)
			for (unsigned r = 0; r < sizeof(vdScaleRegs)/sizeof(vdScaleRegs[0]); ++r)
				exact &= vrRead(pipe, vdScaleRegs[r]) == vd.scale[n][r];
	}
	if (vd.translated) {
		vrWrite(vdCb, 0, 0);
		exact &= vdTlb();
		for (unsigned i = 1; i < sizeof(vdCbRegs)/sizeof(vdCbRegs[0]); ++i)
			vrWrite(vdCb, vdCbRegs[i], vd.cb[i]);
		vrWrite(vdCb, 0, vd.cb[0]);
		for (unsigned i = 0; i < sizeof(vdCbRegs)/sizeof(vdCbRegs[0]); ++i)
			exact &= vrRead(vdCb, vdCbRegs[i]) == vd.cb[i];
	}
	vrWrite(gVenusIntf, 0x180, 1u << 10);
	vrWrite(gVenusIntf, 0x180, vd.misr & 0x8f0001ff);
	exact &= (vrRead(gVenusIntf, 0x180) & 0x8f0001ff) == (vd.misr & 0x8f0001ff);
	vrWrite(vdCtl(), 0x18, vd.flush);
	exact &= vrResumeAutorefresh(vd.sync);
	vd.owned = vd.translated = vd.scaled = vd.ready = false;
	venusBacklightLog("venus-dpu: restore gop=%u registers=%u crc=%#x context=%#x total=%llu changes=%llu\n",
	    proof, exact, crc, vrRead(vdCb, 0), vd.total, vd.changes);
	return exact;
}

extern "C" void venus_dpu_quiesce(void);
extern "C" void
venus_dpu_quiesce(void)
{
	if (!vd.owned) return;
	bool enabled = ml_set_interrupts_enabled(false);
	if (!vdRestore() && vd.owned) {
		/* Returning to a loader that can reuse live DMA pages is unsafe.
		 * Use the SM8350 PSCI SYSTEM_RESET path and never release the pages. */
		venusBacklightLog("venus-dpu: unsafe DMA handback; cold reset\n");
		register uint64_t function asm("x0") = 0x84000009;
		__asm__ volatile("smc #0" : "+r"(function) :: "memory");
		for (;;) __asm__ volatile("wfi");
	}
	ml_set_interrupts_enabled(enabled);
}

static bool
vdPrepare(void)
{
	if (!vdCb || (vrRead(vdSmmu, 0x1010) != 0x1f000) || (vrRead(vdSmmu, 0x1810) != 1) ||
	    !vdFaultFree() || vrRead(vdCtl(), 0xf4) != 2 || vrRead(vdCtl(), 0xfc) != 0x30000 ||
	    vrRead(vdCtl(), 0xe8) != 3 || vrRead(vdCtl(), 0x18) || vrRead(vdMdp, 0x10) ||
	    vrRead(gVenusIntf, vrPp) != 0xf0000 || vrRead(gVenusIntf, vrAuto) != 0x80000001 ||
	    vrRead(gVenusIntf, vrSync) != 0x180032 || kvtophys(vinfo.v_baseaddr) != vdGop ||
	    vinfo.v_rowbytes != 5760 || (vrRead(gVenusIntf, 0x180) & 0x100)) return false;
	static const uint32_t original[] = {0xe0, 0x60, 0, 0, 0, 0, 0, 0, 0};
	for (unsigned i = 0; i < sizeof(vdCbRegs)/sizeof(vdCbRegs[0]); ++i) {
		vd.cb[i] = vrRead(vdCb, vdCbRegs[i]);
		if (vd.cb[i] != original[i]) return false;
	}
	for (unsigned n = 0; n < 2; ++n) {
		vm_offset_t p = vdMdp + vdPipes[n];
		if (vrRead(p, 0) != 0xc8002d0 || vrRead(p, 8) != n * 720 ||
		    vrRead(p, 0xc) != 0xc8002d0 || vrRead(p, 0x10) != n*720 ||
		    vrRead(p, 0x14) != vdGop || vrRead(p, 0x24) != 5760 ||
		    vrRead(p, 0x30) != 0x237ff || vrRead(p, 0x34) != 0x3020001 || vrRead(p, 0x38) != 0x80000000 ||
		    vrRead(p, 0xa00) != 0x3000 || vrRead(p, 0xa04)) return false;
		for (unsigned r = 0; r < sizeof(vdScaleRegs)/sizeof(vdScaleRegs[0]); ++r)
			vd.scale[n][r] = vrRead(p, vdScaleRegs[r]);
	}
	vd.tables = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
	    vdTableCount*4096, PAGE_SIZE);
	if (!vd.tables) return false;
	vd.table = (uint64_t *)vd.tables->getBytesNoCopy();
	if (!vd.table) return false;
	bzero(vd.table, vdTableCount*4096);
	for (unsigned i = 0; i < vdTableCount; ++i) {
		vd.pagePA[i] = kvtophys((vm_offset_t)vd.table + i*4096);
		if (!vd.pagePA[i] || (vd.pagePA[i] & ~vdPhysMask)) return false;
	}
	vd.pages = 1;
	if (!vdMap(vdGop, nullptr, vdGopBytes)) return false;
	for (unsigned n = 0; n < 2; ++n) {
		vd.images[n] = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
		    vdRound4K(vdImageBytes), PAGE_SIZE);
		if (!vd.images[n]) return false;
		uint32_t *p = (uint32_t *)vd.images[n]->getBytesNoCopy();
		if (!p || !vdMap(vdIova[n], p, vdImageBytes)) return false;
		/* Different spatial patterns, with explicit clamped edge taps. */
		for (unsigned y = 0; y < vdHeight; ++y)
			for (unsigned x = 0; x < vdPitch/4; ++x) {
				unsigned logicalX = x < vdWidth ? x : vdWidth-1;
				p[y*(vdPitch/4) + x] = 0xff000000 | ((logicalX/40+2*n) % 4)*0x300000 |
				    ((y/64+n)%4)*0x002500 | (n ? 0x60 : 0x28);
			}
		memcpy(p + vdHeight*(vdPitch/4), p + (vdHeight-1)*(vdPitch/4), vdPitch);
		vdClean(p, vdImageBytes);
	}
	vdClean(vd.table, vd.pages*4096);
	return true;
}

static bool
vdBenchmark(void)
{
	uint64_t first = 0, last = 0, shortest = ~0ULL, longest = 0;
	uint32_t completed = 0, changed = 0, previous = 0;
	for (unsigned i = 0; i < 180; ++i) {
		bool enabled = ml_set_interrupts_enabled(false);
		uint32_t crc = 0;
		bool ok = vdTransfer(vdIova[i & 1], &crc);
		ml_set_interrupts_enabled(enabled);
		if (!ok) break;
		uint64_t now = mach_absolute_time();
		if (!completed) first = now;
		else {
			uint64_t us = micros(now - last);
			if (us < shortest) shortest = us;
			if (us > longest) longest = us;
			changed += crc != previous;
		}
		last = now; previous = crc; ++completed;
	}
	uint64_t us = first ? micros(last - first) : 0;
	bool valid = completed == 180 && changed == 179 && us && vdFaultFree();
	venusBacklightLog("venus-dpu: backend-benchmark passed=%u completed=%u changed=%u time_us=%llu fps_milli=%llu min_us=%llu max_us=%llu\n",
	    valid, completed, changed, us, us ? (completed - 1)*1000000000ULL/us : 0, shortest, longest);
	return valid;
}

static void
vdAcquire(void)
{
	if (!vdPrepare()) {
		venusBacklightLog("venus-dpu: acquire refused before hardware writes\n");
		return;
	}
	bool enabled = ml_set_interrupts_enabled(false);
	vd.sync = vrRead(gVenusIntf, vrSync);
	vd.automatic = vrRead(gVenusIntf, vrAuto);
	vd.misr = vrRead(gVenusIntf, 0x180);
	vd.flush = vrRead(vdCtl(), 0x18);
	vd.owned = true;
	bool manual = vrStopAutorefresh(vd.sync, vd.automatic);
	vrWrite(gVenusIntf, vrSync, vd.sync);
	uint32_t frame = vrRead(gVenusIntf, vrFrame);
	delay(18000);
	manual &= (vrRead(gVenusIntf, vrFrame) == frame && vrRead(gVenusIntf, vrAuto) == 1 && vdIdle());
	venusBacklightLog("venus-dpu: manual=%u frame_delta=%u table_pages=%u\n", manual,
	    vrRead(gVenusIntf, vrFrame) - frame, vd.pages);
	if (!manual) goto finish;
	vrWrite(gVenusIntf, 0x180, 1u << 10);
	vrWrite(gVenusIntf, 0x180, 0x81000101);
	{
		uint32_t before = 0, after = 0, a = 0, b = 0, a2 = 0;
		if (!vdTransfer(vdGop, &before)) goto finish;
		vd.translated = true;
		vrWrite(vdCb, 0, 0xe0);
		vrWrite(vdCb, 0x10, 0x38011);
		vrWrite(vdCb, 0x30, 0x803519);
		uint64_t root = vd.pagePA[0] | (4ULL << 48);
		vrWrite(vdCb, 0x20, (uint32_t)root); vrWrite(vdCb, 0x24, root >> 32);
		vrWrite(vdCb, 0x38, 0xf404ff44);
		bool context = vrRead(vdCb, 0x20) == (uint32_t)root && vrRead(vdCb, 0x24) == (uint32_t)(root >> 32) &&
		    vrRead(vdCb, 0x30) == 0x803519 && (vrRead(vdCb, 0x10) & ~0x60u) == 0x38011 &&
		    vrRead(vdCb, 0x38) == 0xf404ff44 && vdTlb();
		if (!context) goto finish;
		vrWrite(vdCb, 0, 0x1127);
		bool identity = vdTransfer(vdGop, &after) && before == after;
		venusBacklightLog("venus-dpu: identity=%u crc=%#x/%#x fsr=%#x global=%#x\n", identity, before, after,
		    vrRead(vdCb, 0x58), vrRead(vdSmmu, 0x48));
		if (!identity) goto finish;
		vd.scaled = true;
		vdScaler();
		bool quiet = gVenusRefreshMode == 10;
		if (quiet) {
			/* The user boot never transmits diagnostic colour patterns.
			 * Three equal black transfers still validate both DMA buffers,
			 * their completion, and repeatable post-scaler output. */
			for (unsigned n = 0; n < 2; ++n) {
				bzero(vd.images[n]->getBytesNoCopy(), vdImageBytes);
				vdClean(vd.images[n]->getBytesNoCopy(), vdImageBytes);
			}
		}
		bool scaled = vdTransfer(vdIova[0], &a) && vdTransfer(vdIova[1], &b) &&
		    vdTransfer(vdIova[0], &a2) && a == a2 && (quiet ? a == b : a != b);
		if (quiet)
			venusBacklightLog("venus-dpu: quiet acquired=%u black_frames=3 no_test_pattern=1 crc=%#x/%#x/%#x fsr=%#x global=%#x\n",
			    scaled, a, b, a2, vrRead(vdCb, 0x58), vrRead(vdSmmu, 0x48));
		else venusBacklightLog("venus-dpu: patterns passed=%u crc=%#x/%#x/%#x fsr=%#x global=%#x\n",
		    scaled, a, b, a2, vrRead(vdCb, 0x58), vrRead(vdSmmu, 0x48));
		if (scaled && gVenusRefreshMode >= 9 && !quiet) {
			ml_set_interrupts_enabled(enabled);
			scaled = vdBenchmark();
			enabled = ml_set_interrupts_enabled(false);
		}
		if (scaled && gVenusRefreshMode >= 7) {
			bzero(vd.images[0]->getBytesNoCopy(), vdImageBytes);
			bzero(vd.images[1]->getBytesNoCopy(), vdImageBytes);
			vd.ready = true;
			ml_set_interrupts_enabled(enabled);
			return;
		}
	}
finish:
	venus_dpu_quiesce();
	ml_set_interrupts_enabled(enabled);
}

static bool
vdRetireFrame(void)
{
	if (!vd.pending) return true;
	uint32_t crc = 0;
	if (!vdComplete(&crc)) return false;
	static uint64_t begin;
	static uint32_t frames, changes;
	const uint64_t now = mach_absolute_time();
	const bool changed = !vd.total || crc != vd.lastCrc;
	++vd.total;
	vd.changes += changed;
	vd.lastCrc = crc;
	if (!begin) begin = now;
	else { ++frames; changes += changed; }
	uint64_t us = micros(now - begin);
	if (us >= 1000000) {
		venusBacklightLog("venus-dpu: new-window frames=%u changed=%u time_us=%llu fps_milli=%llu changed_milli=%llu at_us=%llu\n",
		    frames, changes, us, frames * 1000000000ULL / us, changes * 1000000000ULL / us, micros(now));
		begin = now; frames = changes = 0;
	}
	return true;
}

/* There is exactly one presenter. It may build the inactive buffer while
 * the last command transfer is in flight, but must retire it before arming
 * another and before the DMA buffer can cycle back to the writer. */
static bool
vdPresentOwned(void)
{
	if (!vd.ready) return false;
	vdClean(vd.images[vd.next]->getBytesNoCopy(), vdImageBytes);
	bool enabled = ml_set_interrupts_enabled(false);
	bool result = vdRetireFrame();
	if (result) {
		vdArm((uint32_t)vdIova[vd.next]);
		vd.next ^= 1;
	}
	ml_set_interrupts_enabled(enabled);
	if (!result) venus_dpu_quiesce();
	return result;
}

/* The presenter's GPU/display reader guard holds the source until this copy
 * is over. Only the two permanently mapped buffers ever go on the bus. */
static bool
vdPresentSwap(const uint8_t *swap, MiniIOSurface **surfaces)
{
	if (!vd.ready) return false;
	uint8_t *out = (uint8_t *)vd.images[vd.next]->getBytesNoCopy();
	const auto *s = surfaces[0];
	for (unsigned n = 1; n < SWAP_LAYERS; ++n)
		if (swapWord(swap, SWAP_SURFACE(n))) goto fallback;
	if (!s) {
		if (!(swapWord(swap, SWAP_LAYERS_SET) & 1)) return true;
		bzero(out, vdImageBytes);
	} else {
		int32_t src[4], dst[4];
		memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
		if (!(s->info.width == vdWidth && s->info.height == vdHeight &&
		    s->info.bytesPerElement == 4 && s->info.planeCount == 0 &&
		    s->info.pixelFormat == FOURCC('B', 'G', 'R', 'A') &&
		    s->info.bytesPerRow >= vdWidth*4 && s->info.offset <= s->info.allocSize &&
		    (uint64_t)s->info.bytesPerRow * vdHeight <= s->info.allocSize - s->info.offset &&
		    !src[0] && !src[1] && src[2] == vdWidth && src[3] == vdHeight &&
		    !dst[0] && !dst[1] && dst[2] == vdWidth && dst[3] == vdHeight)) goto fallback;
		for (unsigned y = 0; y < vdHeight; ++y) {
			uint8_t *line = out + y*vdPitch;
			memcpy(line, s->pixels() + (size_t)y*s->info.bytesPerRow, vdWidth*4);
			uint32_t edge;
			memcpy(&edge, line + (vdWidth-1)*4, 4);
			for (unsigned x = vdWidth; x < vdPitch/4; ++x)
				memcpy(line + x*4, &edge, 4);
		}
		memcpy(out + vdHeight*vdPitch, out + (vdHeight-1)*vdPitch, vdPitch);
	}
	return vdPresentOwned();
fallback:
	venusBacklightLog("venus-dpu: unsupported swap=%u; restoring GOP for compatibility\n", swapWord(swap, SWAP_ID));
	venus_dpu_quiesce();
	return false;
}
