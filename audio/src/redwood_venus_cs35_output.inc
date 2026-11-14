/* Venus stock-profile output. Included by redwood_speaker.c after the reset helper.
 * A single worker owns I2C; PCM feeding never waits for power events or fades. */
static LCK_GRP_DECLARE(venus_output_group, "venus-output");
static LCK_MTX_DECLARE(venus_output_lock, &venus_output_group);
static struct {
    unsigned mask, configured;
    uint32_t base_below;
    bool allow_unmute, hw_muted[2];
    uint32_t below[2];
    struct venus_cs35_power_state power[2];
    /* Only these intent/status fields are shared with the PCM thread. */
    bool alive, stop, failed, want_active, want_muted;
    uint32_t extra;
} venus_output;

static uint64_t
venus_output_now(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

static bool
venus_output_fault(void)
{
    if (venus_digital.reset_asserted) return false;
    venus_digital.cancelled = true;
    for (unsigned i = 0; i < 2; i++) venus_output.power[i].poisoned = true;
    venus_digital_reset_all();
    return false;
}

static bool
venus_output_options(void)
{
    uint32_t mask = 0, muted = 1, below = 192;
    PE_parse_boot_argn("venus_output_mask", &mask, sizeof(mask));
    PE_parse_boot_argn("venus_output_muted", &muted, sizeof(muted));
    PE_parse_boot_argn("venus_output_below", &below, sizeof(below));
    if (mask != 0 && mask != 1 && mask != 3) return false;
    bzero(&venus_output, sizeof(venus_output));
    venus_output.mask = mask;
    venus_output.allow_unmute = muted == 0;
    venus_output.base_below = below < 192 ? 192 : below > 816 ? 816 : below;
    return true;
}

static bool
venus_output_write_volume(unsigned i, uint32_t below, bool muted)
{
    uint32_t field = venus_cs35_output_field(below,muted);
    /* Stock 4 ms soft ramp, no polarity or amplification change. */
    if (!venus_cs35_write_word_verified(venus_digital_addresses[i], 0x6000u, 0x8004u | (field << 3))) {
        return venus_output_fault();
    }
    venus_output.hw_muted[i] = muted;
    venus_output.below[i] = below;
    return true;
}

static bool
venus_output_prepare(void)
{
    for (unsigned i = 0; i < 2; i++) {
        if (!(venus_output.mask & (1u << i))) continue;
        uint8_t address = venus_digital_addresses[i];
        uint32_t gain, threshold;
        if (!venus_cs35_halo_outputs_off(address) || !venus_cs35_stock_boost_verified(address) ||
            !venus_cs35_register_read(address, 0x6c04u, &gain) ||
            !venus_cs35_register_read(address, 0x4220u, &threshold) ||
            !venus_output_write_volume(i, 1024, true) ||
            !venus_cs35_write_word_verified(address, 0x6c04u, (gain & ~0x3e0u) | (18u << 5)) ||
            !venus_cs35_write_word_verified(address, 0x4220u, (threshold & ~3u) | 3u) ||
            !venus_cs35_write_word_verified(address, 0x2018u, 0x3700u) ||
            !venus_cs35_write_word_verified(address, 0x201cu, 0x01101010u) ||
            (i == 1 && !venus_cs35_write_word_verified(address, 0x4c44u, 0))) {
            return venus_output_fault();
        }
        venus_output.configured |= 1u << i;
    }
    say("venus cs35: stock output prepared mask=%u, analog gain=18, ceiling=-%u/8 dB, unmute=%u\n",
        venus_output.mask, venus_output.base_below, venus_output.allow_unmute);
    return true;
}

/* This guard also runs during GLOBAL_EN transitions and the muted shutdown
 * interval, where the sensor/amp gates have already been cleared. */
static bool
venus_output_guard(unsigned i, bool mailbox_check)
{
    uint8_t address = venus_digital_addresses[i];
    uint32_t irq, state, error, mailbox, volume, gain, peak, blocks, power;
    uint32_t field = venus_cs35_output_field(venus_output.below[i],venus_output.hw_muted[i]);
    return !venus_digital.cancelled && !venus_digital.reset_asserted && level(15) && level(42) &&
        venus_cs35_register_read(address, 0x2014u, &power) && (power & ~1u) == 0 &&
        venus_cs35_register_read(address, 0x2018u, &blocks) &&
        (blocks == 0x3700u || blocks == 0x3721u || blocks == 0x20u) &&
        venus_cs35_register_read(address, 0x6000u, &volume) && volume == (0x8004u | field << 3) &&
        venus_cs35_register_read(address, 0x6c04u, &gain) && (gain & 0x3e0u) == (18u << 5) &&
        venus_cs35_register_read(address, 0x10010u, &irq) && (irq & VENUS_CS35_POWER_FAULTS) == 0 &&
        venus_cs35_register_read(address, 0x10098u, &irq) && (irq & 2u) != 0 &&
        venus_cs35_register_read(address, venus_digital.state[i], &state) && state == 2 &&
        venus_cs35_register_read(address, 0x0280025cu, &error) && error == 0 &&
        venus_cs35_register_read(address, 0x3808u, &peak) && (peak & 0x7fu) <= 0x40u &&
        (!mailbox_check || (venus_cs35_register_read(address, 0x13004u, &mailbox) &&
        mailbox == ((venus_digital.running & (1u << i)) ? 0u : 1u)));
}

static bool
venus_output_chip_safe(unsigned i)
{
    uint32_t power, blocks;
    return !venus_output.power[i].poisoned && venus_output_guard(i, true) &&
        venus_cs35_register_read(venus_digital_addresses[i], 0x2014u, &power) &&
        power == (venus_output.power[i].active ? 1u : 0u) &&
        venus_cs35_register_read(venus_digital_addresses[i], 0x2018u, &blocks) &&
        blocks == (venus_output.power[i].active ? 0x3721u : 0x3700u);
}

static bool
venus_output_command_allowed(uint8_t address)
{
    unsigned i = address == 0x40 ? 0 : 1;
    if (!(venus_output.configured & (1u << i)) || !venus_output.power[i].active)
        return venus_cs35_halo_outputs_off(address);
    return venus_output.hw_muted[i] && venus_output_guard(i, false);
}

struct venus_output_context { unsigned index; };
static bool venus_output_read(void *context, uint32_t reg, uint32_t *value)
{
    uint8_t address=venus_digital_addresses[((struct venus_output_context *)context)->index];
    bool ok=venus_cs35_register_read(address,reg,value);
    if(ok && reg==VENUS_CS35_POWER_IRQ) say("venus cs35: stock event 0x%02x IRQ=%#x\n",address,*value);
    return ok;
}
static bool venus_output_write(void *context, uint32_t reg, uint32_t value)
{
    uint8_t bytes[] = {value >> 24, value >> 16, value >> 8, value};
    return venus_cs35_write(venus_digital_addresses[((struct venus_output_context *)context)->index], reg, bytes, 4);
}
static bool venus_output_power_guard(void *context)
{
    unsigned i = ((struct venus_output_context *)context)->index;
    return venus_output.hw_muted[i] && venus_output_guard(i, true);
}
static bool venus_output_reset(__unused void *context)
{
    venus_digital.cancelled=true;
    for(unsigned i=0;i<2;i++) venus_output.power[i].poisoned=true;
    return venus_digital_reset_all();
}
static uint64_t venus_output_clock(__unused void *context) { return venus_output_now(); }
static void venus_output_delay(__unused void *context) { delay_for_interval(1, NSEC_PER_MSEC); }

static bool
venus_output_transition(unsigned i, bool run)
{
    struct venus_output_context context = {i};
    const struct venus_cs35_power_io io = {.context=&context, .read=venus_output_read, .write=venus_output_write,
        .guard=venus_output_power_guard, .reset=venus_output_reset, .now_us=venus_output_clock, .delay_ms=venus_output_delay};
    enum venus_cs35_power_result result = run ? venus_cs35_muted_power_start(&io, &venus_output.power[i]) :
        venus_cs35_muted_power_stop(&io, &venus_output.power[i]);
    say("venus cs35: stock output 0x%02x %s result=%u\n", venus_digital_addresses[i], run ? "PUP" : "PDN", result);
    return result == VENUS_CS35_POWER_OK || venus_output_fault();
}

static bool
venus_output_prepare_stop(unsigned i)
{
    struct venus_output_context context={i};
    const struct venus_cs35_power_io io={.context=&context,.read=venus_output_read,.write=venus_output_write,
        .guard=venus_output_power_guard,.reset=venus_output_reset,.now_us=venus_output_clock,.delay_ms=venus_output_delay};
    enum venus_cs35_power_result result=venus_cs35_muted_power_prepare_stop(&io,&venus_output.power[i]);
    say("venus cs35: stock output 0x%02x fresh shutdown boundary result=%u\n",venus_digital_addresses[i],result);
    return result==VENUS_CS35_POWER_OK || venus_output_fault();
}

/* Optional one-shot snapshot on the I2C owner after HALO resumes, while muted. */
static bool
venus_output_snapshot(void)
{
    static const uint32_t regs[] = {
        0, 4, 0x2014, 0x2018, 0x201c, 0x2c04, 0x2c0c,
        0x3800, 0x3804, 0x3808, 0x380c, 0x3810, 0x3814,
        0x4800, 0x4804, 0x4808, 0x4820, 0x4840, 0x4c00, 0x4c20, 0x4c24,
        0x4c40, 0x4c44, 0x4c48, 0x4c4c, 0x4c50, 0x4c54, 0x4c58, 0x4c5c,
        0x640c, 0x02bc1000, 0x02bc5000, 0x02bc5004,
        0x02bc5200, 0x02bc5204, 0x02bc5400, 0x02bc5404,
        0x02800260, 0x02800264, 0x0280036c, 0x02800370,
        0x02800378, 0x0280037c, 0x02800380, 0x02800384,
        0x6000, 0x6400, 0x6404, 0x6c04, 0x10010, 0x10090, 0x10098,
        0x13004,
        0x02b80080, 0x02b80088, 0x02b80280, 0x02b80288,
        0x02800678, 0x02800680, 0x02800694, 0x02800698, 0x0280069c, 0x028006a8,
        0x02800250, 0x02800254, 0x02800258,
        0x02800014, 0x0280025c, 0x0280026c, 0x02800274, 0x02800278,
    };
    for (unsigned i=0; i<2; i++) {
        if (!(venus_output.mask & (1u<<i))) continue;
        for (unsigned r=0; r<sizeof(regs)/sizeof(regs[0]); r++) {
            uint32_t value;
            if (!venus_cs35_register_read(venus_digital_addresses[i],regs[r],&value)) return venus_output_fault();
            say("venus cs35: snapshot addr=%02x reg=%08x value=%08x\n",venus_digital_addresses[i],regs[r],value);
        }
    }
    return true;
}

static bool
venus_output_run(bool run)
{
    if (!redwood_speaker_venus_digital_safe()) return false;
    if (!run) {
        for (unsigned i=0; i<2; i++) {
            if ((venus_output.mask & (1u<<i)) && !venus_output.hw_muted[i] &&
                !venus_output_write_volume(i,1024,true)) return false;
        }
        /* Stock mute precedes power-down by at least 30 ms. */
        delay_for_interval(30,NSEC_PER_MSEC);
    }
    for (unsigned i=0; i<2; i++) {
        unsigned bit=1u<<i;
        if (((venus_digital.running & bit)!=0)==run) continue;
        if (run && (venus_output.mask & bit) && !venus_output_transition(i,true)) return false;
        if (!run && (venus_output.mask & bit) && venus_output.power[i].active &&
            (!venus_output_prepare_stop(i) || !venus_cs35_write_word_verified(venus_digital_addresses[i],0x2018u,0x21u) ||
             !venus_cs35_write_word_verified(venus_digital_addresses[i],0x201cu,0x01101000u) ||
             !venus_cs35_write_word_verified(venus_digital_addresses[i],0x2018u,0x20u) ||
             !venus_cs35_write_word_verified(venus_digital_addresses[i],0x201cu,0x01101000u))) return venus_output_fault();
        if (!venus_cs35_cspl_command(venus_digital_addresses[i],run?VENUS_CS35_CSPL_RESUME:VENUS_CS35_CSPL_PAUSE))
            return venus_output_fault();
        if (run) venus_digital.running |= bit; else venus_digital.running &= ~bit;
        if (!run && (venus_output.mask & bit) && venus_output.power[i].active &&
            (!venus_output_transition(i,false) ||
             !venus_cs35_write_word_verified(venus_digital_addresses[i],0x201cu,0x01101010u))) return venus_output_fault();
    }
    uint32_t dump=0;
    static bool dumped;
    PE_parse_boot_argn("venus_output_dump",&dump,sizeof(dump));
    if (run && dump==1 && !dumped) {
        if (!venus_output_snapshot()) return false;
        dumped=true;
    }
    bool safe=redwood_speaker_venus_digital_safe();
    say("venus cs35: stock output %s %s, mask=%u\n",run?"START":"STOP",safe?"verified":"failed",venus_output.mask);
    return safe;
}

static bool
venus_output_volume_step(uint32_t extra, bool muted)
{
    uint32_t target = venus_cs35_output_target(extra,venus_output.base_below);
    muted |= !venus_output.allow_unmute;
    for(unsigned i=0;i<2;i++) {
        if (!(venus_output.mask & (1u<<i))) continue;
        if (!venus_output.power[i].active) {
            if (!muted && !venus_output.hw_muted[i] && !venus_output_write_volume(i,1024,true)) return false;
            continue;
        }
        if(muted) {
            if(!venus_output.hw_muted[i] && !venus_output_write_volume(i,1024,true)) return false;
            continue;
        }
        uint32_t current=venus_output.hw_muted[i]?816u:venus_output.below[i];
        uint32_t next=venus_cs35_output_next(current,target,venus_output.hw_muted[i]);
        if((venus_output.hw_muted[i] || next!=current) && !venus_output_write_volume(i,next,false)) return false;
    }
    return true;
}

bool redwood_speaker_venus_output_selected(void) { return venus_output.mask!=0; }

void
redwood_speaker_venus_output_request(bool active, uint32_t extra, bool muted)
{
    lck_mtx_lock(&venus_output_lock);
    venus_output.want_active=active; venus_output.extra=extra; venus_output.want_muted=muted;
    lck_mtx_unlock(&venus_output_lock);
}

bool
redwood_speaker_venus_output_status(bool *alive)
{
    lck_mtx_lock(&venus_output_lock);
    *alive=venus_output.alive;
    bool good=!venus_output.failed;
    lck_mtx_unlock(&venus_output_lock);
    return good;
}

void
redwood_speaker_venus_output_stop(void)
{
    lck_mtx_lock(&venus_output_lock);
    venus_output.stop=true;
    lck_mtx_unlock(&venus_output_lock);
}

static void
venus_output_worker(__unused void *context,__unused wait_result_t result)
{
    bool good=true, active=false, traced=false;
    uint32_t dump=0;
    PE_parse_boot_argn("venus_output_dump",&dump,sizeof(dump));
    uint64_t checked=venus_output_now(), ramped=0, active_since=0;
    while(good) {
        lck_mtx_lock(&venus_output_lock);
        bool stop=venus_output.stop, want=venus_output.want_active && !venus_output.want_muted;
        bool muted=venus_output.want_muted;
        uint32_t extra=venus_output.extra;
        lck_mtx_unlock(&venus_output_lock);
        if(stop) break;
        if(want!=active) {
            good=redwood_speaker_venus_digital_running(want);
            if(good) {active=want;traced=false;active_since=venus_output_now();}
        }
        uint64_t now=venus_output_now();
        if(good && active && now-ramped>=8000u) {good=venus_output_volume_step(extra,muted);ramped=now;}
        if(good && now-checked>=250000u) {good=redwood_speaker_venus_digital_safe();checked=venus_output_now();}
        if(good && active && dump==1 && !traced && now-active_since>=500000u) {
            say("venus cs35: snapshot kind=playing begin, requested attenuation=%u\n",extra);
            good=venus_output_snapshot();
            say("venus cs35: snapshot kind=playing end, status=%u\n",good);
            traced=true;
        }
        delay_for_interval(2,NSEC_PER_MSEC);
    }
    if(!redwood_speaker_venus_digital_running(false)) good=false;
    lck_mtx_lock(&venus_output_lock);
    venus_output.failed=!good;
    venus_output.alive=false;
    lck_mtx_unlock(&venus_output_lock);
    say("venus cs35: stock output worker joined, status=%s\n",good?"passed":"failed");
    thread_terminate(current_thread());
}

bool
redwood_speaker_venus_output_start(void)
{
    thread_t thread;
    lck_mtx_lock(&venus_output_lock);
    venus_output.stop=venus_output.failed=venus_output.want_active=false;
    venus_output.want_muted=true;
    venus_output.alive=true;
    lck_mtx_unlock(&venus_output_lock);
    if(kernel_thread_start(venus_output_worker,NULL,&thread)!=KERN_SUCCESS) {
        lck_mtx_lock(&venus_output_lock);
        venus_output.alive=false;venus_output.failed=true;
        lck_mtx_unlock(&venus_output_lock);
        return false;
    }
    thread_set_thread_name(thread,"Venus stock amplifier control");
    thread_deallocate(thread);
    return true;
}
