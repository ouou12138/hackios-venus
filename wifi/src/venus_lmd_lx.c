/*
 * Venus LMD: the kernel side of the independent ath11k object (ath11k-vlmd.o).
 * Port of the lemonade reference redwood_lx.c bridge for the wifi-lemonade-port
 * plan. All bridge symbols carry the vlmd_ prefix so the existing Wi-Fi track's
 * lx_os_* world is untouched. The radio answers come from venus_lmd_wifi.c;
 * frames and station state reuse the shared redwood_net/redwood_sta infrastructure.
 * Interrupts: the venus_lmd transport polls its own MSI mailbox, so no GIC SPI
 * is taken here (vlmd_lx_os_irq_msi reports "no interrupts").
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/redwood_net.h>
#include <sys/redwood_sta.h>
#include <sys/systm.h>
#include <sys/venus_lmd_lx.h>
#include <sys/venus_lmd_wifi.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern const uint8_t *redwood_image_file(const char *, const char *, uint64_t *, int *);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);

void venus_lmd_qmi_say(const char *format, ...) __printflike(1, 2);
void
venus_lmd_qmi_say(const char *format, ...)
{
	va_list ap;
	char text[512];
	va_start(ap, format);
	vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	redwood_tl("venus-lmd-qmi: %s", text);
}

static LCK_GRP_DECLARE(vlmd_lx_lock_group, "venus-lmd-lx");
static LCK_MTX_DECLARE(vlmd_lx_lock, &vlmd_lx_lock_group);

/* ---- memory ---- */

void *
vlmd_lx_os_alloc(unsigned long size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

void
vlmd_lx_os_free(void *memory, unsigned long size)
{
	kfree_data(memory, size);
}

void *
vlmd_lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	return venus_lmd_wifi_driver_memory(size, address);
}

unsigned vlmd_lx_os_radio_platform(void) { return venus_lmd_wifi_driver_mode() ? 1 : 0; }
void *vlmd_lx_os_pcie_bar(void) { return venus_lmd_wifi_driver_bar(); }
unsigned vlmd_lx_os_pcie_read(unsigned offset) { return venus_lmd_wifi_register_read(offset); }
void vlmd_lx_os_pcie_write(unsigned offset, unsigned value) { venus_lmd_wifi_register_write(offset, value); }

void *
vlmd_lx_os_map_registers(unsigned long long physical, unsigned long size)
{
	unsigned long long page = trunc_page(physical);
	vm_offset_t mapped = ml_io_map_unmappable(page, round_page(physical + size) - page, VM_WIMG_IO);
	return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}

/* ---- saying things ---- */

void
vlmd_lx_os_print(const char *text)
{
	redwood_tl("%s", text);
}

void
vlmd_lx_os_tell(const char *text)
{
	redwood_tl("%s", text);
}

void
vlmd_lx_os_panic(const char *text)
{
	panic("%s", text);
}

/* ---- time, threads, sleeping ---- */

unsigned long long
vlmd_lx_os_nanoseconds(void)
{
	uint64_t nanoseconds;
	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

void *
vlmd_lx_os_thread_self(void)
{
	return current_thread();
}

struct vlmd_lx_thread {
	void (*function)(void *);
	void *argument;
};

static void
vlmd_lx_thread_runs(void *parameter, __unused wait_result_t result)
{
	struct vlmd_lx_thread start = *(struct vlmd_lx_thread *)parameter;
	kfree_type(struct vlmd_lx_thread, parameter);
	start.function(start.argument);
	thread_terminate(current_thread());
}

int
vlmd_lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	struct vlmd_lx_thread *start = kalloc_type(struct vlmd_lx_thread, Z_WAITOK | Z_ZERO);
	thread_t thread;
	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (kernel_thread_start(vlmd_lx_thread_runs, start, &thread) != KERN_SUCCESS) {
		kfree_type(struct vlmd_lx_thread, start);
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
vlmd_lx_os_lock(void)
{
	lck_mtx_lock(&vlmd_lx_lock);
}

void
vlmd_lx_os_unlock(void)
{
	lck_mtx_unlock(&vlmd_lx_lock);
}

int
vlmd_lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t deadline;
	if (nanoseconds == 0) {
		return lck_mtx_sleep(&vlmd_lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
	}
	nanoseconds_to_absolutetime(nanoseconds, &deadline);
	deadline += mach_absolute_time();
	return lck_mtx_sleep_deadline(&vlmd_lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}

void
vlmd_lx_os_wakeup(const void *address)
{
	thread_wakeup((event_t)address);
}

/* ---- the rest of the machine ---- */

void
vlmd_lx_os_random(void *buffer, unsigned long size)
{
	read_random(buffer, (u_int)size);
}

unsigned
vlmd_lx_os_option(const char *name)
{
	unsigned mode = venus_lmd_wifi_driver_mode();
	if (mode) {
		if (!strcmp(name, "wifi_lx")) { return 2; }
		if (!strcmp(name, "wifi_quiet")) { return 1; }
		if (!strcmp(name, "wifi_irq")) { return 0; }
		if (!strcmp(name, "wifi_scan")) { return mode == 23; }
		/* Mode 24 defaults to the reference station-offer (iOS drives the link).
		 * The isolated probe has no iOS Wi-Fi userland, so venus_lmd_join=1 selects
		 * the autonomous join of fw/network instead; venus_lmd_hold=N stays joined
		 * N seconds and reports what passed before the probe is stopped. */
		unsigned join = 0, hold = 0;
		PE_parse_boot_argn("venus_lmd_join", &join, sizeof(join));
		PE_parse_boot_argn("venus_lmd_hold", &hold, sizeof(hold));
		if (!strcmp(name, "wifi_station")) { return mode == 24 && !join; }
		if (!strcmp(name, "wifi_join")) { return mode == 24 && join; }
		if (!strcmp(name, "wifi_hold")) { return hold; }
	}
	return 0;
}

int
vlmd_lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
	uint64_t length = 0;
	const uint8_t *file = venus_lmd_wifi_driver_file(name, &length);
	if (file == NULL) {
		return ENOENT;
	}
	*data = file;
	*size = (unsigned long)length;
	return 0;
}

int
vlmd_lx_os_rproc_boot(void)
{
	return venus_lmd_wifi_driver_mode() ? 0 : ENXIO;
}

int
vlmd_lx_os_radio_address(unsigned char *address)
{
	/* Locally administered random address for the probe; the real one is
	 * never read from the device. Station mode is not accepted yet. */
	static unsigned char local[6];
	static bool ready;
	if (!venus_lmd_wifi_driver_mode()) { return 0; }
	if (!ready) { read_random(local, sizeof(local)); local[0] = (local[0] & 0xfe) | 2; ready = true; }
	memcpy(address, local, sizeof(local));
	return 1;
}

int
vlmd_lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length)
{
	/* The station stage (mode 24) joins the network named in fw/network of the
	 * ramdisk: line 1 the name, line 2 the passphrase. Only the lengths are ever
	 * said; the name and passphrase themselves are not logged anywhere.
	 * Returns 1 when a network was given, 0 otherwise (lx_station_join tests !ret,
	 * the reference bool convention -- not an errno). */
	if (venus_lmd_wifi_driver_mode() != 24) { return 0; }
	uint64_t size = 0;
	const uint8_t *file = redwood_image_file("fw", "network", &size, NULL);
	if (file == NULL) {
		redwood_tl("venus-lmd-lx: which network to join: none was given (fw/network of the ramdisk)\n");
		return 0;
	}
	const uint8_t *end = file + size, *line = file;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	unsigned char *where[2] = { name, passphrase };
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;
		while (stop < end && *stop != '\n') { stop++; }
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) { lengths[n] = 0; }
		else { memcpy(where[n], line, lengths[n]); }
		line = stop + 1;
	}
	bool good = lengths[0] >= 1 && lengths[0] <= 32 && lengths[1] >= 8 && lengths[1] <= 63;
	redwood_tl("venus-lmd-lx: which network to join: a name of %u bytes and a passphrase of %u bytes were given%s\n",
	    lengths[0], lengths[1], good ? "" : ": NOT A NAME OF 1 TO 32 BYTES AND A PASSPHRASE OF 8 TO 63");
	if (!good) { return 0; }
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return 1;
}

/* ---- frames: the radio as the link behind en0 ---- */

#define VLMD_LX_QUEUE        256

struct vlmd_lx_frame {
	uint16_t length;
	uint8_t bytes[REDWOOD_NET_FRAME_MAX];
};

static bool vlmd_lx_link_attached;
static LCK_MTX_DECLARE(vlmd_lx_queue_lock, &vlmd_lx_lock_group);
static struct vlmd_lx_frame *vlmd_lx_queue;
static uint32_t vlmd_lx_queue_in, vlmd_lx_queue_out;
static uint64_t vlmd_lx_queue_dropped;

static void
vlmd_lx_queue_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		struct vlmd_lx_frame *frame;
		lck_mtx_lock(&vlmd_lx_queue_lock);
		while (vlmd_lx_queue_in == vlmd_lx_queue_out) {
			lck_mtx_sleep(&vlmd_lx_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&vlmd_lx_queue_in, THREAD_UNINT);
		}
		frame = &vlmd_lx_queue[vlmd_lx_queue_out % VLMD_LX_QUEUE];
		lck_mtx_unlock(&vlmd_lx_queue_lock);
		vlmd_lx_transmit(frame->bytes, frame->length);
		lck_mtx_lock(&vlmd_lx_queue_lock);
		vlmd_lx_queue_out++;
		lck_mtx_unlock(&vlmd_lx_queue_lock);
	}
}

static bool
vlmd_lx_link_transmit(mbuf_t packet, size_t length)
{
	struct vlmd_lx_frame *frame;
	lck_mtx_lock(&vlmd_lx_queue_lock);
	if (length > REDWOOD_NET_FRAME_MAX || vlmd_lx_queue_in - vlmd_lx_queue_out == VLMD_LX_QUEUE) {
		vlmd_lx_queue_dropped++;
		lck_mtx_unlock(&vlmd_lx_queue_lock);
		return false;
	}
	frame = &vlmd_lx_queue[vlmd_lx_queue_in % VLMD_LX_QUEUE];
	if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
		lck_mtx_unlock(&vlmd_lx_queue_lock);
		return false;
	}
	frame->length = (uint16_t)length;
	vlmd_lx_queue_in++;
	lck_mtx_unlock(&vlmd_lx_queue_lock);
	thread_wakeup((event_t)&vlmd_lx_queue_in);
	return true;
}

static struct redwood_net_link vlmd_lx_link = {
	.name = "the phone's radio (ath11k, venus-lmd)",
	.wireless = true,
	.transmit = vlmd_lx_link_transmit,
};

void
vlmd_lx_os_net_attach(const unsigned char *address)
{
	thread_t thread;
	if (vlmd_lx_link_attached) {
		return;
	}
	vlmd_lx_queue = kalloc_data(VLMD_LX_QUEUE * sizeof(*vlmd_lx_queue), Z_WAITOK | Z_ZERO);
	if (vlmd_lx_queue == NULL || kernel_thread_start(vlmd_lx_queue_runs, NULL, &thread) != KERN_SUCCESS) {
		redwood_tl("venus-lmd-lx: no memory or thread for the radio's frames\n");
		return;
	}
	thread_set_thread_name(thread, "vlmd-wifi-send");
	thread_deallocate(thread);
	memcpy(vlmd_lx_link.address, address, sizeof(vlmd_lx_link.address));
	vlmd_lx_link_attached = redwood_net_attach(&vlmd_lx_link);
	redwood_tl("venus-lmd-lx: %s\n", vlmd_lx_link_attached ? "the radio is the link behind en0" :
	    "EN0 HAS A LINK ALREADY: the radio's frames go nowhere");
}

void
vlmd_lx_os_net_link(int up)
{
	if (vlmd_lx_link_attached) {
		redwood_net_link_state(up != 0);
	}
}

void
vlmd_lx_os_net_input(const unsigned char *frame, unsigned long length)
{
	if (vlmd_lx_link_attached && length <= REDWOOD_NET_FRAME_MAX) {
		redwood_net_input(frame, length);
	}
}

/* ---- interrupts: none taken; the transport polls its MSI mailbox ---- */

int
vlmd_lx_os_irq_msi(unsigned long long *address, unsigned *data)
{
	(void)address; (void)data;
	return 0;
}

int
vlmd_lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument)
{
	(void)vector; (void)function; (void)argument;
	return EINVAL;
}

void vlmd_lx_os_irq_enable(unsigned vector) { (void)vector; }
void vlmd_lx_os_irq_disable(unsigned vector) { (void)vector; }
void vlmd_lx_os_irq_detach_all(void) {}
unsigned long long vlmd_lx_os_irq_count(unsigned vector) { (void)vector; return 0; }

/* ---- the station ---- */

void
vlmd_lx_os_station(const struct redwood_sta_ops *ops)
{
	redwood_sta_attach(ops);
}

void
vlmd_lx_os_station_lost(int error)
{
	redwood_sta_lost(error);
}