#ifndef VENUS_BRIGHTNESS_STATE_H
#define VENUS_BRIGHTNESS_STATE_H
/* Panel state is separate from the user's setting: blanking/fades must never
 * overwrite the setting restored on the next wake or userspace restart. */
struct venus_brightness_state {
    int manual_level;
    int restored;
    int first_manual;
};
static void venus_brightness_state_init(struct venus_brightness_state *s,int saved)
{
    s->manual_level=saved>=4&&saved<=2047?saved:128;
    s->restored=saved>=4&&saved<=2047;
    s->first_manual=1;
}
static int venus_brightness_manual(struct venus_brightness_state *s,int level,int commit)
{
    if(level<4||level>2047)return -1;
    if(!s->first_manual||!s->restored||commit)s->manual_level=level;
    s->first_manual=0;
    return s->manual_level;
}
static int venus_brightness_factor(const struct venus_brightness_state *s,int factor)
{
    if(factor<4||factor>2047)return -1;
    return 4+((s->manual_level-4)*(factor-4)+1021)/2043;
}
#endif
