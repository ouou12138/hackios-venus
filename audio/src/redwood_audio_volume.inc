/* The exact volume snapshot helper from redwood_audio.c, also exercised on the host. */
uint32_t
redwood_audio_sink_attenuation(uint32_t minimum, uint32_t maximum)
{
	if (volume_generation == 0 || volume_muted) return REDWOOD_GUARD_SILENT;
	return volume_below > maximum - minimum ? maximum : minimum + volume_below;
}
