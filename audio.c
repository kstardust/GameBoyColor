#include "audio.h"

/* audio is annoyingly obscure, a nightmare to implement */

/* every register has its own mask for reading
https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Register_Reading

     NRx0 NRx1 NRx2 NRx3 NRx4
    ---------------------------
NR1x  $80  $3F $00  $FF  $BF
NR2x  $FF  $3F $00  $FF  $BF
NR3x  $7F  $FF $9F  $FF  $BF
NR4x  $FF  $FF $00  $00  $BF
NR5x  $00  $00 $70

$FF27-$FF2F always read back as $FF
*/

static uint8_t _duty_waveform[] = {
    0b00000001, 0b10000001, 0b10000111, 0b01111110
};

static void
zero_channel(gbc_audio_channel_t *c)
{
    c->NRx0 = 0;
    c->NRx1 = 0;
    c->NRx2 = 0;
    c->NRx3 = 0;
    c->NRx4 = 0;
    c->sample_cycles = 0;
    c->lfsr = 0;
    c->waveform_idx = 0;
    c->volume = 0;
    c->volume_pace = 0;
    c->volume_pace_counter = 0;
    c->volume_dir = 0;
}

static uint8_t
read_nr10(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c1.NRx0 | 0x80;
}

static uint8_t
write_nr10(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c1.NRx0 = data;
    return data;
}

static uint8_t
read_nr11(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c1.NRx1 | 0x3f;
}

static uint8_t
write_nr11(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    gbc_audio_channel_t *ch = &(audio->c1);
    ch->NRx1 = data;
    ch->length_counter = CHANNEL_MAX_LENGTH - (data & CHANNEL_LENGTH_MASK);
    return data;
}

static uint8_t
read_nr12(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c1.NRx2;
}

static uint8_t
write_nr12(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c1.NRx2 = data;
    return data;
}

static uint8_t
read_nr13(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c1.NRx3 | 0xff;
}

static uint8_t
write_nr13(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c1.NRx3 = data;
    return data;
}

static uint8_t
read_nr14(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c1.NRx4 | 0xbf;
}

/* is the same for all channel except channel 3 */
static uint8_t
_write_nrx4(gbc_audio_t *audio, uint16_t addr, uint8_t data, gbc_audio_channel_t *ch)
{
    uint8_t length_enable = (data & CHANNEL_LENGTH_ENABLE_MASK) ? 1 : 0;
    ch->NRx4 = data;

    uint16_t max = (ch == &(audio->c3)) ? CHANNEL3_MAX_LENGTH : CHANNEL_MAX_LENGTH;

    /* https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Obscure_Behavior */
    uint8_t next_step_doesnt_update = ((audio->frame_sequencer & 1) == 0);
    if (next_step_doesnt_update) {
        if (length_enable && !ch->length_enabled && ch->length_counter > 0) {
            ch->length_counter--;
            if (ch->length_counter == 0)
                ch->on = 0;
        }
    }

    /* I used 'length_enable' instead of 'ch->length_enabled', because the doc says "the length counter is now enabled",
      which means the data will enable the length counter, not the current state of the length counter in the 5th rule:
      https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Obscure_Behavior
      but it failed Blargg's sound test 3 "Trigger shouldn't otherwise affect length"
      I have to check out the code of wasmboy to notice this:
      https://github.com/torch2424/wasmboy/blob/master/core/sound/channel1.ts
    */
    if ((data & CHANNEL_TRIGGER_MASK) && ch->length_enabled && ch->length_counter == max) {
        ch->length_counter -= 1;
    }

    /* I didnt find it in the docs(pandoc & gbdev), but is in Blargg's sound test 2 */
    if ((data & CHANNEL_TRIGGER_MASK) && ch->length_counter == 0) {
        ch->length_counter = max;
    }

    ch->length_enabled = length_enable;
    return data;
}

static uint8_t
write_nr14(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    return _write_nrx4(audio, addr, data, &(audio->c1));
}

static uint8_t
read_nr21(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c2.NRx1 | 0x3f;
}

static uint8_t
write_nr21(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    gbc_audio_channel_t *ch = &(audio->c2);
    ch->length_counter = CHANNEL_MAX_LENGTH - (data & CHANNEL_LENGTH_MASK);
    ch->NRx1 = data;
    return data;
}

static uint8_t
read_nr22(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c2.NRx2;
}

static uint8_t
write_nr22(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c2.NRx2 = data;
    return data;
}


static uint8_t
read_nr23(gbc_audio_t *audio,  uint16_t addr)
{
    return audio->c2.NRx3 | 0xff;
}

static uint8_t
write_nr23(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c2.NRx3 = data;
    return data;
}

static uint8_t
read_nr24(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c2.NRx4 | 0xbf;
}

static uint8_t
write_nr24(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    return _write_nrx4(audio, addr, data, &(audio->c2));
}

static uint8_t
read_nr30(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c3.NRx0 | 0x7f;
}

static uint8_t
write_nr30(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    gbc_audio_channel_t *ch = &(audio->c3);
    ch->on = (data & CH3_DAC_ON_MASK);
    ch->NRx0 = data;
    return data;
}

static uint8_t
read_nr31(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c3.NRx1 | 0xff;
}

static uint8_t
write_nr31(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    gbc_audio_channel_t *ch = &(audio->c3);
    ch->length_counter = CHANNEL3_MAX_LENGTH - data;
    ch->NRx1 = data;
    return data;
}

static uint8_t
read_nr32(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c3.NRx2 | 0x9f;
}

static uint8_t
write_nr32(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c3.NRx2 = data;
    return data;
}

static uint8_t
read_nr33(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c3.NRx3 | 0xff;
}

static uint8_t
write_nr33(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c3.NRx3 = data;
    return data;
}

static uint8_t
read_nr34(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c3.NRx4 | 0xbf;
}

static uint8_t
write_nr34(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    return _write_nrx4(audio, addr, data, &(audio->c3));
}

static uint8_t
read_nr41(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c4.NRx1 | 0xff;
}

static uint8_t
write_nr41(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    gbc_audio_channel_t *ch = &(audio->c4);
    ch->length_counter = CHANNEL_MAX_LENGTH - (data & CHANNEL_LENGTH_MASK);
    ch->NRx1 = data;
    return data;
}

static uint8_t
read_nr42(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c4.NRx2;
}

static uint8_t
write_nr42(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c4.NRx2 = data;
    return data;
}

static uint8_t
read_nr43(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c4.NRx3;
}

static uint8_t
write_nr43(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->c4.NRx3 = data;
    return data;
}

static uint8_t
read_nr44(gbc_audio_t *audio, uint16_t addr)
{
    return audio->c4.NRx4 | 0xbf;
}

static uint8_t
write_nr44(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    return _write_nrx4(audio, addr, data, &(audio->c4));
}

static uint8_t
read_nr50(gbc_audio_t *audio, uint16_t addr)
{
    return audio->NR50;
}

static uint8_t
write_nr50(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->NR50 = data;
    return data;
}

static uint8_t
read_nr51(gbc_audio_t *audio, uint16_t addr)
{
    return audio->NR51;
}

static uint8_t
write_nr51(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->NR51 = data;
    return data;
}

static uint8_t
read_nr52(gbc_audio_t *audio, uint16_t addr)
{
    return audio->NR52 | 0x70;
}

static uint8_t
write_nr52(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    /* https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Power_Control */
    /* lower 4 bits are read only */
    data &= 0xf0;
    if (!(data & NR52_AUDIO_ON)) {
        /* power off */
        zero_channel(&(audio->c1));
        zero_channel(&(audio->c2));
        zero_channel(&(audio->c3));
        zero_channel(&(audio->c4));
        audio->NR50 = 0;
        audio->NR51 = 0;
    } else {
        /* power on */
        audio->frame_sequencer = 0;
    }

    audio->NR52 = data;
    return data;
}

static uint8_t
read_waveform(gbc_audio_t *audio, uint16_t addr)
{
    return audio->waveforms[addr-IO_PORT_BASE-IO_PORT_WAVE_RAM];
}

static uint8_t
write_waveform(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    audio->waveforms[addr-IO_PORT_BASE-IO_PORT_WAVE_RAM] = data;
    return data;
}

static uint8_t
read_unused(gbc_audio_t *audio, uint16_t addr)
{
    return 0xff;
}

static uint8_t
write_unused(gbc_audio_t *audio, uint16_t addr, uint8_t data)
{
    return 0xff;
};


static uint8_t (*_read_trampolines[])(gbc_audio_t*, uint16_t) = {
    read_nr10, read_nr11, read_nr12, read_nr13, read_nr14,

    read_unused, read_nr21, read_nr22, read_nr23, read_nr24,

    read_nr30, read_nr31, read_nr32, read_nr33, read_nr34,

    read_unused, read_nr41, read_nr42, read_nr43, read_nr44,

    read_nr50, read_nr51, read_nr52,
};

static uint8_t (*_write_trampolines[])(gbc_audio_t*, uint16_t, uint8_t) = {
    write_nr10, write_nr11, write_nr12, write_nr13, write_nr14,

    write_unused, write_nr21, write_nr22, write_nr23, write_nr24,

    write_nr30, write_nr31, write_nr32, write_nr33, write_nr34,

    write_unused, write_nr41, write_nr42, write_nr43, write_nr44,

    write_nr50, write_nr51, write_nr52,
};


void
gbc_audio_init(gbc_audio_t *audio)
{
    memset(audio, 0, sizeof(gbc_audio_t));
}

uint8_t
audio_read(void *udata, uint16_t addr)
{
    gbc_audio_t *audio = (gbc_audio_t*)udata;

    uint8_t port = IO_ADDR_PORT(addr);
    if (port >= IO_PORT_NR10 && port <= IO_PORT_NR52) {
        return _read_trampolines[port - IO_PORT_NR10](audio, addr);
    } else if (port >= IO_PORT_WAVE_RAM) {
        return read_waveform(audio, addr);
    }
    return read_unused(audio, addr);
}

uint8_t
audio_write(void *udata, uint16_t addr, uint8_t data)
{
    gbc_audio_t *audio = (gbc_audio_t*)udata;

    uint8_t port = IO_ADDR_PORT(addr);
    if (port >= IO_PORT_NR10 && port <= IO_PORT_NR52) {
        if (port != IO_PORT_NR52 && !(audio->NR52 & NR52_AUDIO_ON)) {
            /* power is off */
            return 0;
        }
        return _write_trampolines[port - IO_PORT_NR10](audio, addr, data);
    } else if (port >= IO_PORT_WAVE_RAM) {
        return write_waveform(audio, addr, data);
    }

    return write_unused(audio, addr, data);
}

void
gbc_audio_connect(gbc_audio_t *audio, gbc_memory_t *mem)
{
    memory_map_entry_t entry;
    entry.id = AUDIO_ID;
    entry.addr_begin = AUDIO_BEGIN;
    entry.addr_end = AUDIO_END;
    entry.read = audio_read;
    entry.write = audio_write;
    entry.udata = audio;

    register_memory_map(mem, &entry);

    audio->mem = mem;
}

static int8_t
ch1_audio(gbc_audio_t *audio)
{
    gbc_audio_channel_t *ch = &(audio->c1);

    uint8_t div_frame = ch->frame_sequencer_flag;
    ch->frame_sequencer_flag = 0;

    /* https://gbdev.io/pandocs/Audio_details.html#dacs */
    if (!((ch->NRx2) & DAC_ON_MASK)) {
        ch->on = 0;
        return 0;
    }

    uint8_t triggered =  0;
    if (ch->NRx4 & CHANNEL_TRIGGER_MASK) {
        /* since the game cannot read this field, i think we can change it */
        ch->NRx4 &= ~CHANNEL_TRIGGER_MASK;
        ch->on = 1;
        triggered = 1;

        /* TODO: we need to somehow retrigger this channel which I'm not exactly sure what it means */
        ch->sample_cycles = 0;
        ch->waveform_idx = 0;
        ch->sweep_pace = 0;
        ch->volume = CHANNEL_EVENVLOPE_VOLUME(ch);
        ch->volume_pace = CHANNEL_EVENVLOPE_PACE(ch);
        ch->volume_pace_counter = ch->volume_pace;
        ch->volume_dir = CHANNEL_EVENVLOPE_DIRECTION(ch);
    }

    /* disabled channel should still counting the length */
    if (div_frame && (audio->frame_sequencer % FRAME_SOUND_LENGTH == 0)) {
        /* sound length */
        if (ch->length_enabled && ch->length_counter > 0) {
            ch->length_counter--;
            if (ch->length_counter == 0)
                ch->on = 0;
        }
    }

    if (!ch->on) {
        return 0;
    }

    if (triggered || (div_frame && audio->frame_sequencer % FRAME_FREQ_SWEEP == 0)) {
        /* frequency sweep */

        if (CHANNEL_SWEEP_PACE(ch) == 0) {
            /* write 0 to sweep pace immediately disable the sweep */
            ch->sweep_pace = 0;
        } else {
            if (ch->sweep_pace == 0) {
                ch->sweep_pace = CHANNEL_SWEEP_PACE(ch);
                uint8_t steps = CHANNEL_SWEEP_STEPS(ch);
                uint16_t period = CHANNEL_PERIOD(ch);
                /* step shift needs be non-zero
                    which isn't mentioned in the PanDoc but in gg8 wiki
                    https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Frequency_Sweep
                */
                if (steps != 0) {
                    if (CHANNEL_SWEEP_DIRECTION(ch)) {
                        /* decrease */
                        period -= period >> steps;
                    } else {
                        /* increase */
                        period += period >> steps;
                    }
                    period &= 0x7ff;
                    CHANNEL_PERIOD_UPDATE(ch, period);
                    if (period >= 0x7ff) {
                        /* overflowed */
                        ch->on = 0;
                        return 0;
                    }
                }
            } else {
                ch->sweep_pace--;
            }
        }
    }

    if (ch->volume_pace != 0 && div_frame &&
        (audio->frame_sequencer % FRAME_ENVELOPE_SWEEP == 0)) {
        /* envelope sweep */
        ch->volume_pace_counter--;
        if (ch->volume_pace_counter == 0) {
            ch->volume_pace_counter = ch->volume_pace;
            if (ch->volume_dir) {
                ch->volume++;
            } else {
                if (ch->volume != 0)
                    ch->volume--;
            }
            ch->volume &= 0xf;
        }
    }

    uint16_t sample_period = CHANNEL_PERIOD(ch);
    ch->sample_cycles++;
    if (ch->sample_cycles == 0x7ff) {
        if (ch->waveform_idx == WAVEFORM_SAMPLES)
            ch->waveform_idx = 1;
        else
            ch->waveform_idx += 1;
        ch->sample_cycles = sample_period;
    }

    uint8_t on = WAVEFORM_SAMPLE(_duty_waveform[CHANNEL_DUTY(ch)], ch->waveform_idx-1);

    return on * ch->volume;
}

static int8_t
ch2_audio(gbc_audio_t *audio)
{
    gbc_audio_channel_t *ch = &(audio->c2);
    uint8_t div_frame = ch->frame_sequencer_flag;
    ch->frame_sequencer_flag = 0;

    /* https://gbdev.io/pandocs/Audio_details.html#dacs */
    if (!(ch->NRx2 & DAC_ON_MASK)) {
        ch->on = 0;
        return 0;
    }

    uint8_t triggered =  0;
    if (ch->NRx4 & CHANNEL_TRIGGER_MASK) {
        /* since the game cannot read this field, i think we can change it */
        ch->NRx4 &= ~CHANNEL_TRIGGER_MASK;
        ch->on = 1;
        triggered = 1;
        ch->sample_cycles = 0;
        ch->waveform_idx = 0;
        ch->sweep_pace = 0;
        ch->volume = CHANNEL_EVENVLOPE_VOLUME(ch);
        ch->volume_pace = CHANNEL_EVENVLOPE_PACE(ch);
        ch->volume_pace_counter = ch->volume_pace;
        ch->volume_dir = CHANNEL_EVENVLOPE_DIRECTION(ch);
    }

    if (div_frame && (audio->frame_sequencer % FRAME_SOUND_LENGTH == 0)) {
        /* sound length */
        if (ch->length_enabled && ch->length_counter > 0) {
            ch->length_counter--;
            if (ch->length_counter == 0)
                ch->on = 0;
        }
    }

    if (!ch->on) {
        return 0;
    }

    if (ch->volume_pace != 0 && div_frame &&
        (audio->frame_sequencer % FRAME_ENVELOPE_SWEEP == 0)) {
        /* envelope sweep */
        ch->volume_pace_counter--;
        if (ch->volume_pace_counter == 0) {
            ch->volume_pace_counter = ch->volume_pace;
            if (ch->volume_dir) {
                ch->volume++;
            } else {
                if (ch->volume != 0)
                    ch->volume--;
            }
            ch->volume &= 0xf;
        }
    }

    uint16_t sample_period = CHANNEL_PERIOD(ch);
    ch->sample_cycles++;
    if (ch->sample_cycles == 0x7ff) {
        if (ch->waveform_idx == WAVEFORM_SAMPLES)
            ch->waveform_idx = 1;
        else
            ch->waveform_idx += 1;
        ch->sample_cycles = sample_period;
    }

    uint8_t on = WAVEFORM_SAMPLE(_duty_waveform[CHANNEL_DUTY(ch)], ch->waveform_idx-1);

    return on * ch->volume;
}

static int8_t
ch3_audio(gbc_audio_t *audio)
{
    gbc_audio_channel_t *ch = &(audio->c3);
    uint8_t div_frame = ch->frame_sequencer_flag;
    ch->frame_sequencer_flag = 0;

    /* https://gbdev.io/pandocs/Audio_details.html#dacs */
    if (!(ch->NRx0 & CH3_DAC_ON_MASK)) {
        ch->on = 0;
        return 0;
    }

    uint8_t triggered =  0;
    if (ch->NRx4 & CHANNEL_TRIGGER_MASK) {
        /* since the game cannot read this field, i think we can change it */
        ch->NRx4 &= ~CHANNEL_TRIGGER_MASK;
        ch->on = 1;
        triggered = 1;
        ch->sample_cycles = 0;
        ch->waveform_idx = 0;
        ch->sweep_pace = 0;
    }

    /* disabled channel should still counting the length */
    if (div_frame && (audio->frame_sequencer % FRAME_SOUND_LENGTH == 0)) {
        /* sound length */
        if (ch->length_enabled && ch->length_counter > 0) {
            ch->length_counter--;
            if (ch->length_counter == 0)
                ch->on = 0;
        }
    }

    if (!ch->on) {
        return 0;
    }

    uint16_t sample_period = CHANNEL_PERIOD(ch);
    ch->sample_cycles += 1;
    if (ch->sample_cycles == 0x7ff) {
        if (ch->waveform_idx == CH3_WAVEFORM_SAMPLES)
            ch->waveform_idx = 1;
        else
            ch->waveform_idx += 1;
        ch->sample_cycles = sample_period;
    }

    uint8_t volume = CHANNEL3_OUTPUT_LEVEL(ch);
    if (volume == 0)
        return 0;

    uint8_t sample_offset = (ch->waveform_idx-1) / 2;
    /* According to Pandoc, when CH3 is started, the first sample read is the one at index 1, I didn't implement this */
    uint8_t wave_form = IO_PORT_READ(audio->mem, IO_PORT_WAVE_RAM + sample_offset);
    if (ch->waveform_idx % 2 == 0)
        wave_form &= 0xf;
    else
        wave_form >>= 4;

    return wave_form >> (volume-1);
}

static int8_t
ch4_audio(gbc_audio_t *audio)
{
    gbc_audio_channel_t *ch = &(audio->c4);
    uint8_t div_frame = ch->frame_sequencer_flag;
    ch->frame_sequencer_flag = 0;

    if (!(ch->NRx2 & DAC_ON_MASK)) {
        ch->on = 0;
        return 0;
    }

    uint8_t triggered =  0;
    if (ch->NRx4 & CHANNEL_TRIGGER_MASK) {
        /* since the game cannot read this field, i think we can change it */
        ch->NRx4 &= ~CHANNEL_TRIGGER_MASK;
        ch->on = 1;
        triggered = 1;

        ch->volume = CHANNEL_EVENVLOPE_VOLUME(ch);
        ch->volume_pace = CHANNEL_EVENVLOPE_PACE(ch);
        ch->volume_pace_counter = ch->volume_pace;
        ch->volume_dir = CHANNEL_EVENVLOPE_DIRECTION(ch);
        ch->lfsr = 0x7fff;

        ch->sample_cycles = 0;

    }

    /* disabled channel should still counting the length */
    if (div_frame && (audio->frame_sequencer % FRAME_SOUND_LENGTH == 0)) {
        /* sound length */
        if (ch->length_enabled && ch->length_counter > 0) {
            ch->length_counter--;
            if (ch->length_counter == 0)
                ch->on = 0;
        }
    }

    if (!ch->on) {
        return 0;
    }

    if (ch->volume_pace != 0 && div_frame &&
        (audio->frame_sequencer % FRAME_ENVELOPE_SWEEP == 0)) {
        /* envelope sweep */
        ch->volume_pace_counter--;
        if (ch->volume_pace_counter == 0) {
            ch->volume_pace_counter = ch->volume_pace;
            if (ch->volume_dir) {
                ch->volume++;
            } else {
                if (ch->volume != 0)
                    ch->volume--;
            }
            ch->volume &= 0xf;
        }
    }

    if (audio->cycles % FRAME_NOISE_TICK == 0) {

        if (ch->sample_cycles == 0) {
            uint8_t shift = CHANNEL4_CLOCK_SHIFT(ch);
            uint8_t divider = CHANNEL4_CLOCK_DIVIDER(ch);
            uint16_t lfsr = ch->lfsr;

            uint8_t b = (lfsr & 1) ^ ((lfsr >> 1) & 1);
            lfsr >>= 1;
            lfsr &= ~(1 << 14);
            lfsr |= b << 14;
            if (CHANNEL4_LFSR_SHORT_MODE(ch)) {
                lfsr &= ~(1 << 6);
                lfsr |= b << 6;
            }

            ch->lfsr = lfsr;

            uint32_t period = 1 << (shift + 1);
            if (divider == 0) {
                period >>= 1;
            } else {
                period *= divider;
            }

            ch->sample_cycles = period * 2;
        } else {
            ch->sample_cycles--;
        }
    }

    return (ch->lfsr & 1) * ch->volume;
}

void
gbc_audio_cycle(gbc_audio_t *audio)
{
    uint8_t div = IO_PORT_READ(audio->mem, IO_PORT_DIV);
    uint8_t mask = 0x10;

    if (IO_PORT_READ(audio->mem, IO_PORT_KEY1) & 0x80) {
        /* double speed mode */
        mask = 0x20;
    }

    if ((audio->div_apu & mask) && !(div & mask)) {
        audio->frame_sequencer++;
        /* rewind */
        if (audio->frame_sequencer == FRAME_ENVELOPE_SWEEP)
            audio->frame_sequencer = 0;

        audio->c1.frame_sequencer_flag = 1;
        audio->c2.frame_sequencer_flag = 1;
        audio->c3.frame_sequencer_flag = 1;
        audio->c4.frame_sequencer_flag = 1;

    }

    audio->div_apu = div;

    audio->m_cycles++;
    if (audio->m_cycles != AUDIO_CLOCK_CYCLES) {
        return;
    }

    audio->m_cycles = 0;
    audio->cycles++;

    if (!(audio->NR52 & NR52_AUDIO_ON))
        return;
    /*
    Actually frame sequencer is not a seperate timer in real GameBoy, it is tied to DIV register.
    I simplied it.
    https://gbdev.io/pandocs/Audio_details.html#div-apu */

    int8_t c1 = ch1_audio(audio);
    int8_t c2 = ch2_audio(audio);
    /* ch3 runs at 2097152 Hz, but our audio module updates at 1048576 Hz,
        I'm doing linear interpolation here
    */
    int8_t c3 = (ch3_audio(audio) + ch3_audio(audio)) / 2;
    int8_t c4 = ch4_audio(audio);

    audio->NR52 &= ~0xf;
    audio->NR52 |= (audio->c1.on)
                    | ((audio->c2.on) << 1)
                    | ((audio->c3.on) << 2)
                    | ((audio->c4.on) << 3);

    /* todo read NR50 */
    uint8_t volume = 0x7;

    audio->sample += (c1 + c2 + c3 + c4) * volume / 4;

    if (audio->output_sample_cycles == 0) {
        /* linear interpolation */
        int8_t sample = audio->sample / audio->sample_divider;

        audio->audio_write(sample, sample);
        audio->output_sample_cycles = SAMPLE_TO_AUDIO_CYCLES;
        audio->output_sample_cycles_remainder += SAMPLE_TO_AUDIO_CYCLES_REMAINDER;

        if (audio->output_sample_cycles_remainder >= REMAINDER_SCALING_FACTOR) {
            audio->output_sample_cycles++;
            audio->output_sample_cycles_remainder -= REMAINDER_SCALING_FACTOR;
        }
        audio->sample = 0;
        audio->sample_divider = audio->output_sample_cycles;
    }

    audio->output_sample_cycles--;

    /* TODO: volume panning */
}
