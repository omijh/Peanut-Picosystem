#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#ifndef GB_AUDIO_HOST_TEST
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/time.h"

namespace picosystem {
extern struct repeating_timer _audio_update_timer;
}
#endif

static constexpr uint32_t GB_CPU_HZ = 4194304u;
static constexpr uint32_t GB_AUDIO_SAMPLE_RATE = 32768u;
static constexpr uint32_t GB_AUDIO_CYCLES_PER_SAMPLE = GB_CPU_HZ / GB_AUDIO_SAMPLE_RATE; // 128
static constexpr uint32_t GB_AUDIO_SAMPLES_PER_SEQ_STEP = GB_AUDIO_SAMPLE_RATE / 512u;   // 64
static constexpr uint16_t GB_AUDIO_PWM_WRAP = 255u;         // 8-bit level range (0..255)
static constexpr float    GB_AUDIO_PWM_CARRIER_HZ = 61035.15625f; // 61.0 kHz ultrasonic carrier (250 MHz / (16 * 256))
static constexpr int32_t  GB_AUDIO_PWM_MID = 128;
static constexpr uint32_t GB_AUDIO_PIN = 11u;               // PicoSystem piezo buzzer (PWM Slice 5B)
static constexpr uint32_t GB_AUDIO_TIMER_SLICE = 0u;        // Unused internal PWM slice for 32.8 kHz IRQ pacing
static constexpr uint16_t GB_AUDIO_PACER_WRAP = 3814u;      // ~32,765.4 Hz sample IRQ at 250 MHz / 2

struct GbApu {
    struct PulseChannel {
        bool active = false;
        bool length_enabled = false;
        bool env_up = false;
        uint8_t duty = 0;
        uint8_t volume = 0;
        uint8_t env_period = 0;
        uint8_t env_timer = 0;
        uint8_t pos = 0;
        uint16_t length = 0;
        uint16_t freq_raw = 0;
        uint16_t trigger_age = 0xFFFFu;
        int32_t timer = 0;

        bool dac_enabled(uint8_t nrx2) const {
            return (nrx2 & 0xF8u) != 0;
        }

        void trigger(uint8_t nrx1, uint8_t nrx2, uint8_t nrx3, uint8_t nrx4) {
            active = dac_enabled(nrx2);
            if (length == 0) {
                length = 64u;
            }
            duty = (nrx1 >> 6) & 0x03u;
            freq_raw = ((uint16_t)(nrx4 & 0x07u) << 8) | nrx3;
            timer = (int32_t)(2048u - freq_raw) * 4;
            volume = (nrx2 >> 4) & 0x0Fu;
            env_up = (nrx2 & 0x08u) != 0;
            env_period = nrx2 & 0x07u;
            env_timer = env_period ? env_period : 8u;
            trigger_age = 0;
        }

        void clock_length() {
            if (length_enabled && length > 0) {
                length--;
                if (length == 0) {
                    active = false;
                }
            }
        }

        void clock_envelope() {
            if (env_period == 0) {
                return;
            }
            if (env_timer > 0) {
                env_timer--;
            }
            if (env_timer == 0) {
                env_timer = env_period;
                if (env_up && volume < 15u) {
                    volume++;
                } else if (!env_up && volume > 0) {
                    volume--;
                }
            }
        }

        void step_cycles(uint32_t cycles) {
            if (!active) {
                return;
            }
            int32_t period = (int32_t)(2048u - freq_raw) * 4;
            if (period <= 0) {
                period = 4;
            }
            timer -= (int32_t)cycles;
            if (timer <= 0) {
                uint32_t steps = (uint32_t)(-timer) / (uint32_t)period + 1u;
                timer += (int32_t)(steps * (uint32_t)period);
                pos = (uint8_t)((pos + steps) & 0x07u);
            }
        }

        uint8_t sample(uint8_t nrx2) const {
            static constexpr uint8_t DUTY_TABLE[4][8] = {
                {0, 0, 0, 0, 0, 0, 0, 1}, // 12.5%
                {1, 0, 0, 0, 0, 0, 0, 1}, // 25%
                {1, 0, 0, 0, 0, 1, 1, 1}, // 50%
                {0, 1, 1, 1, 1, 1, 1, 0}  // 75%
            };
            if (!active || !dac_enabled(nrx2)) {
                return 0;
            }
            return DUTY_TABLE[duty & 0x03u][pos & 0x07u] ? volume : 0u;
        }
    };

    struct WaveChannel {
        bool active = false;
        bool length_enabled = false;
        uint8_t pos = 0;
        uint16_t length = 0;
        uint16_t freq_raw = 0;
        uint16_t trigger_age = 0xFFFFu;
        int32_t timer = 0;

        bool dac_enabled(uint8_t nr30) const {
            return (nr30 & 0x80u) != 0;
        }

        void trigger(uint8_t nr30, uint8_t nr33, uint8_t nr34) {
            active = dac_enabled(nr30);
            if (length == 0) {
                length = 256u;
            }
            freq_raw = ((uint16_t)(nr34 & 0x07u) << 8) | nr33;
            timer = (int32_t)(2048u - freq_raw) * 2;
            pos = 0;
            trigger_age = 0;
        }

        void clock_length() {
            if (length_enabled && length > 0) {
                length--;
                if (length == 0) {
                    active = false;
                }
            }
        }

        void step_cycles(uint32_t cycles) {
            if (!active) {
                return;
            }
            int32_t period = (int32_t)(2048u - freq_raw) * 2;
            if (period <= 0) {
                period = 2;
            }
            timer -= (int32_t)cycles;
            if (timer <= 0) {
                uint32_t steps = (uint32_t)(-timer) / (uint32_t)period + 1u;
                timer += (int32_t)(steps * (uint32_t)period);
                pos = (uint8_t)((pos + steps) & 0x1Fu);
            }
        }

        uint8_t sample(uint8_t nr30, uint8_t nr32, const uint8_t *wave_ram) const {
            if (!active || !dac_enabled(nr30)) {
                return 0;
            }
            uint8_t lvl = (nr32 >> 5) & 0x03u;
            if (lvl == 0) {
                return 0;
            }
            uint8_t byte = wave_ram[pos >> 1];
            uint8_t nibble = (pos & 1u) ? (byte & 0x0Fu) : (byte >> 4);
            return (uint8_t)(nibble >> (lvl - 1u));
        }
    };

    struct NoiseChannel {
        bool active = false;
        bool length_enabled = false;
        bool env_up = false;
        bool width_7bit = false;
        uint8_t volume = 0;
        uint8_t env_period = 0;
        uint8_t env_timer = 0;
        uint8_t clock_shift = 0;
        uint8_t divisor_code = 0;
        uint16_t length = 0;
        uint16_t lfsr = 0x7FFFu;
        uint16_t trigger_age = 0xFFFFu;
        int32_t timer = 0;

        bool dac_enabled(uint8_t nr42) const {
            return (nr42 & 0xF8u) != 0;
        }

        int32_t calc_period() const {
            uint32_t div = (divisor_code == 0u) ? 8u : ((uint32_t)divisor_code * 16u);
            return (int32_t)(div << (clock_shift & 0x0Fu));
        }

        void trigger(uint8_t nr42, uint8_t nr43) {
            active = dac_enabled(nr42);
            if (length == 0) {
                length = 64u;
            }
            volume = (nr42 >> 4) & 0x0Fu;
            env_up = (nr42 & 0x08u) != 0;
            env_period = nr42 & 0x07u;
            env_timer = env_period ? env_period : 8u;
            clock_shift = (nr43 >> 4) & 0x0Fu;
            width_7bit = (nr43 & 0x08u) != 0;
            divisor_code = nr43 & 0x07u;
            lfsr = 0x7FFFu;
            timer = calc_period();
            trigger_age = 0;
        }

        void clock_length() {
            if (length_enabled && length > 0) {
                length--;
                if (length == 0) {
                    active = false;
                }
            }
        }

        void clock_envelope() {
            if (env_period == 0) {
                return;
            }
            if (env_timer > 0) {
                env_timer--;
            }
            if (env_timer == 0) {
                env_timer = env_period;
                if (env_up && volume < 15u) {
                    volume++;
                } else if (!env_up && volume > 0) {
                    volume--;
                }
            }
        }

        void step_cycles(uint32_t cycles) {
            if (!active || clock_shift >= 14u) {
                return;
            }
            int32_t period = calc_period();
            if (period <= 0) {
                period = 8;
            }
            timer -= (int32_t)cycles;
            uint32_t steps = 0;
            while (timer <= 0 && steps < 16u) {
                timer += period;
                uint16_t xor_bit = (uint16_t)((lfsr & 1u) ^ ((lfsr >> 1) & 1u));
                lfsr = (uint16_t)((lfsr >> 1) | (xor_bit << 14));
                if (width_7bit) {
                    lfsr = (uint16_t)((lfsr & ~(1u << 6)) | (xor_bit << 6));
                }
                steps++;
            }
            if (timer <= 0) {
                uint32_t skip = (uint32_t)(-timer) / (uint32_t)period + 1u;
                timer += (int32_t)(skip * (uint32_t)period);
            }
        }

        uint8_t sample(uint8_t nr42) const {
            if (!active || !dac_enabled(nr42)) {
                return 0;
            }
            return ((~lfsr) & 1u) ? volume : 0u;
        }
    };

    bool power = false;
    uint8_t regs[0x16] = {};     // 0xFF10..0xFF25
    uint8_t wave_ram[0x10] = {}; // 0xFF30..0xFF3F

    PulseChannel ch1;
    PulseChannel ch2;
    WaveChannel  ch3;
    NoiseChannel ch4;

    // CH1 frequency sweep state
    bool sweep_enabled = false;
    bool sweep_negate = false;
    uint8_t sweep_period = 0;
    uint8_t sweep_timer = 0;
    uint8_t sweep_shift = 0;
    uint16_t sweep_shadow = 0;

    // Frame sequencer (512 Hz) & idle ramp state
    uint8_t seq_step = 0;
    uint8_t sample_counter = 0;
    uint8_t master_volume = 30; // 0..100%; intentionally preserved across reset()
    uint32_t silent_samples = 0;
    int32_t dc_bias = 0;
    int32_t lpf_acc = 0;

    void reset() {
        power = false;
        memset(regs, 0, sizeof(regs));
        memset(wave_ram, 0, sizeof(wave_ram));
        ch1 = PulseChannel{};
        ch2 = PulseChannel{};
        ch3 = WaveChannel{};
        ch4 = NoiseChannel{};
        sweep_enabled = false;
        sweep_negate = false;
        sweep_period = 0;
        sweep_timer = 0;
        sweep_shift = 0;
        sweep_shadow = 0;
        seq_step = 0;
        sample_counter = 0;
        silent_samples = 0;
        dc_bias = 0;
        lpf_acc = 0;
    }

    uint32_t calc_sweep_freq() const {
        uint16_t delta = (uint16_t)(sweep_shadow >> sweep_shift);
        int32_t next = sweep_negate
            ? ((int32_t)sweep_shadow - (int32_t)delta)
            : ((int32_t)sweep_shadow + (int32_t)delta);
        if (next < 0) {
            next = 0;
        }
        return (uint32_t)next;
    }

    void clock_sweep() {
        if (sweep_timer > 0) {
            sweep_timer--;
        }
        if (sweep_timer == 0) {
            sweep_timer = sweep_period ? sweep_period : 8u;
            if (sweep_enabled && sweep_period != 0u) {
                uint32_t next = calc_sweep_freq();
                if (next > 2047u) {
                    ch1.active = false;
                } else if (sweep_shift != 0u) {
                    sweep_shadow = (uint16_t)next;
                    ch1.freq_raw = (uint16_t)next;
                    regs[3] = (uint8_t)(next & 0xFFu);
                    regs[4] = (uint8_t)((regs[4] & ~0x07u) | ((next >> 8) & 0x07u));
                    if (calc_sweep_freq() > 2047u) {
                        ch1.active = false;
                    }
                }
            }
        }
    }

    void clock_frame_sequencer() {
        if ((seq_step & 1u) == 0u) {
            ch1.clock_length();
            ch2.clock_length();
            ch3.clock_length();
            ch4.clock_length();
        }
        if (seq_step == 2u || seq_step == 6u) {
            clock_sweep();
        }
        if (seq_step == 7u) {
            ch1.clock_envelope();
            ch2.clock_envelope();
            ch4.clock_envelope();
        }
        seq_step = (uint8_t)((seq_step + 1u) & 0x07u);
    }

    uint8_t read(uint16_t addr) const {
        static constexpr uint8_t ortab[0x16] = {
            0x80, 0x3F, 0x00, 0xFF, 0xBF,
            0xFF, 0x3F, 0x00, 0xFF, 0xBF,
            0x7F, 0xFF, 0x9F, 0xFF, 0xBF,
            0xFF, 0xFF, 0x00, 0x00, 0xBF,
            0x00, 0x00
        };

        if (addr >= 0xFF30u && addr <= 0xFF3Fu) {
            return wave_ram[addr - 0xFF30u];
        }
        if (addr == 0xFF26u) {
            uint8_t status = power ? 0x80u : 0x00u;
            status |= 0x70u;
            if (ch1.active) status |= 0x01u;
            if (ch2.active) status |= 0x02u;
            if (ch3.active) status |= 0x04u;
            if (ch4.active) status |= 0x08u;
            return status;
        }
        if (addr >= 0xFF10u && addr <= 0xFF25u) {
            uint16_t idx = (uint16_t)(addr - 0xFF10u);
            return (uint8_t)(regs[idx] | ortab[idx]);
        }
        return 0xFFu;
    }

    void write(uint16_t addr, uint8_t val) {
        if (addr >= 0xFF30u && addr <= 0xFF3Fu) {
            wave_ram[addr - 0xFF30u] = val;
            return;
        }

        if (addr == 0xFF26u) {
            bool next_power = (val & 0x80u) != 0;
            if (power && !next_power) {
                power = false;
                memset(regs, 0, sizeof(regs));
                ch1 = PulseChannel{};
                ch2 = PulseChannel{};
                ch3 = WaveChannel{};
                ch4 = NoiseChannel{};
                sweep_enabled = false;
                sweep_negate = false;
                sweep_period = 0;
                sweep_timer = 0;
                sweep_shift = 0;
                sweep_shadow = 0;
            } else if (!power && next_power) {
                power = true;
                seq_step = 0;
                sample_counter = 0;
            }
            return;
        }

        if (!power || addr < 0xFF10u || addr > 0xFF25u) {
            return;
        }

        uint16_t idx = (uint16_t)(addr - 0xFF10u);
        regs[idx] = val;

        switch (addr) {
            // Channel 1: Pulse + Sweep
            case 0xFF10u:
                sweep_period = (val >> 4) & 0x07u;
                sweep_negate = (val & 0x08u) != 0;
                sweep_shift = val & 0x07u;
                break;
            case 0xFF11u:
                ch1.duty = (val >> 6) & 0x03u;
                ch1.length = (uint16_t)(64u - (val & 0x3Fu));
                break;
            case 0xFF12u:
                if (!ch1.dac_enabled(val)) {
                    ch1.active = false;
                }
                break;
            case 0xFF13u:
                ch1.freq_raw = (uint16_t)((ch1.freq_raw & 0x0700u) | val);
                break;
            case 0xFF14u:
                ch1.freq_raw = (uint16_t)((ch1.freq_raw & 0x00FFu) | ((uint16_t)(val & 0x07u) << 8));
                ch1.length_enabled = (val & 0x40u) != 0;
                if (val & 0x80u) {
                    ch1.trigger(regs[1], regs[2], regs[3], regs[4]);
                    sweep_shadow = ch1.freq_raw;
                    sweep_period = (regs[0] >> 4) & 0x07u;
                    sweep_negate = (regs[0] & 0x08u) != 0;
                    sweep_shift = regs[0] & 0x07u;
                    sweep_timer = sweep_period ? sweep_period : 8u;
                    sweep_enabled = (sweep_period != 0u || sweep_shift != 0u);
                    if (sweep_shift != 0u && calc_sweep_freq() > 2047u) {
                        ch1.active = false;
                    }
                }
                break;

            // Channel 2: Pulse
            case 0xFF16u:
                ch2.duty = (val >> 6) & 0x03u;
                ch2.length = (uint16_t)(64u - (val & 0x3Fu));
                break;
            case 0xFF17u:
                if (!ch2.dac_enabled(val)) {
                    ch2.active = false;
                }
                break;
            case 0xFF18u:
                ch2.freq_raw = (uint16_t)((ch2.freq_raw & 0x0700u) | val);
                break;
            case 0xFF19u:
                ch2.freq_raw = (uint16_t)((ch2.freq_raw & 0x00FFu) | ((uint16_t)(val & 0x07u) << 8));
                ch2.length_enabled = (val & 0x40u) != 0;
                if (val & 0x80u) {
                    ch2.trigger(regs[6], regs[7], regs[8], regs[9]);
                }
                break;

            // Channel 3: Wave
            case 0xFF1Au:
                if (!ch3.dac_enabled(val)) {
                    ch3.active = false;
                }
                break;
            case 0xFF1Bu:
                ch3.length = (uint16_t)(256u - val);
                break;
            case 0xFF1Cu:
                break;
            case 0xFF1Du:
                ch3.freq_raw = (uint16_t)((ch3.freq_raw & 0x0700u) | val);
                break;
            case 0xFF1Eu:
                ch3.freq_raw = (uint16_t)((ch3.freq_raw & 0x00FFu) | ((uint16_t)(val & 0x07u) << 8));
                ch3.length_enabled = (val & 0x40u) != 0;
                if (val & 0x80u) {
                    ch3.trigger(regs[0x0A], regs[0x0D], regs[0x0E]);
                }
                break;

            // Channel 4: Noise
            case 0xFF20u:
                ch4.length = (uint16_t)(64u - (val & 0x3Fu));
                break;
            case 0xFF21u:
                if (!ch4.dac_enabled(val)) {
                    ch4.active = false;
                }
                break;
            case 0xFF22u:
                ch4.clock_shift = (val >> 4) & 0x0Fu;
                ch4.width_7bit = (val & 0x08u) != 0;
                ch4.divisor_code = val & 0x07u;
                break;
            case 0xFF23u:
                ch4.length_enabled = (val & 0x40u) != 0;
                if (val & 0x80u) {
                    ch4.trigger(regs[0x11], regs[0x12]);
                }
                break;

            default:
                break;
        }
    }

    void restore_from_hram(const uint8_t *hram_io) {
        reset();
        memcpy(wave_ram, hram_io + 0x30, sizeof(wave_ram));
        if ((hram_io[0x26] & 0x80u) == 0u) {
            return;
        }

        write(0xFF26u, 0x80u);
        write(0xFF24u, hram_io[0x24]);
        write(0xFF25u, hram_io[0x25]);

        for (uint16_t addr = 0xFF10u; addr <= 0xFF23u; ++addr) {
            uint8_t v = hram_io[addr - 0xFF00u];
            if (addr == 0xFF14u || addr == 0xFF19u || addr == 0xFF1Eu || addr == 0xFF23u) {
                v &= 0x7Fu; // Mask trigger bit so restored state does not emit stuck notes
            }
            write(addr, v);
        }
    }

    void advance_frame_ages() {
        auto bump = [](uint16_t &age) {
            if (age < 0xFFFFu) {
                age++;
            }
        };
        bump(ch1.trigger_age);
        bump(ch2.trigger_age);
        bump(ch3.trigger_age);
        bump(ch4.trigger_age);
    }

    bool channel_routed(uint8_t ch_idx) const {
        uint8_t nr51 = regs[0x15];
        return (nr51 & ((1u << ch_idx) | (1u << (ch_idx + 4u)))) != 0u;
    }

    static int32_t soft_clip_127(int32_t x) {
        int32_t mag = (x < 0) ? -x : x;
        if (mag <= 80) {
            return x;
        }
        int32_t excess = mag - 80;
        int32_t out = 80 + (47 * excess) / (excess + 47);
        return (x < 0) ? -out : out;
    }

    int32_t nr50_mono_sum() const {
        uint8_t nr50 = regs[0x14];
        return (int32_t)(((nr50 >> 4) & 0x07u) + 1u) + (int32_t)((nr50 & 0x07u) + 1u); // 2..16
    }

    uint16_t render_sample() {
        sample_counter++;
        if (sample_counter >= GB_AUDIO_SAMPLES_PER_SEQ_STEP) {
            sample_counter = 0;
            if (power) {
                clock_frame_sequencer();
            }
        }

        if (power) {
            ch1.step_cycles(GB_AUDIO_CYCLES_PER_SAMPLE);
            ch2.step_cycles(GB_AUDIO_CYCLES_PER_SAMPLE);
            ch3.step_cycles(GB_AUDIO_CYCLES_PER_SAMPLE);
            ch4.step_cycles(GB_AUDIO_CYCLES_PER_SAMPLE);
        }

        int32_t mix = 0;
        bool any_audible = false;

        if (power) {
            if (ch1.active && ch1.dac_enabled(regs[2]) && channel_routed(0) && ch1.volume > 0u) {
                uint8_t s = ch1.sample(regs[2]);
                mix += (s > 0u) ? (int32_t)ch1.volume : -(int32_t)ch1.volume;
                any_audible = true;
            }
            if (ch2.active && ch2.dac_enabled(regs[7]) && channel_routed(1) && ch2.volume > 0u) {
                uint8_t s = ch2.sample(regs[7]);
                mix += (s > 0u) ? (int32_t)ch2.volume : -(int32_t)ch2.volume;
                any_audible = true;
            }
            if (ch3.active && ch3.dac_enabled(regs[0x0A]) && channel_routed(2)) {
                uint8_t lvl = (regs[0x0C] >> 5) & 0x03u;
                if (lvl != 0u) {
                    uint8_t s = ch3.sample(regs[0x0A], regs[0x0C], wave_ram);
                    int32_t max_w = (int32_t)(15u >> (lvl - 1u));
                    mix += (int32_t)s * 2 - max_w;
                    any_audible = true;
                }
            }
            if (ch4.active && ch4.dac_enabled(regs[0x11]) && channel_routed(3) && ch4.volume > 0u) {
                uint8_t s = ch4.sample(regs[0x11]);
                mix += (s > 0u) ? (int32_t)ch4.volume : -(int32_t)ch4.volume;
                any_audible = true;
            }
        }

        if (master_volume == 0u) {
            any_audible = false;
        }

        if (any_audible) {
            silent_samples = 0;
        } else if (silent_samples < 100000u) {
            silent_samples++;
        }

        int32_t target_bias = (any_audible || silent_samples <= 512u) ? GB_AUDIO_PWM_MID : 0;
        if (dc_bias < target_bias) {
            dc_bias = std::min(dc_bias + 2, target_bias);
        } else if (dc_bias > target_bias) {
            dc_bias = std::max(dc_bias - 2, target_bias);
        }

        int32_t scaled_fp = (master_volume == 0u) ? 0 : ((mix * nr50_mono_sum()) << 4);
        if (scaled_fp == 0 && lpf_acc > -4 && lpf_acc < 4) {
            lpf_acc = 0;
        } else {
            lpf_acc += (scaled_fp - lpf_acc) >> 1;
        }
        int32_t driven = (lpf_acc * 12) >> 8;
        int32_t shaped = soft_clip_127(driven);
        int32_t amp = (shaped * (int32_t)master_volume) / 100;
        if (dc_bias < GB_AUDIO_PWM_MID) {
            amp = (amp * dc_bias) / GB_AUDIO_PWM_MID;
        }
        int32_t level = dc_bias + amp;
        if (level < 0) {
            level = 0;
        } else if (level > (int32_t)GB_AUDIO_PWM_WRAP) {
            level = (int32_t)GB_AUDIO_PWM_WRAP;
        }
        return (uint16_t)level;
    }

    static uint8_t scale_piezo_vol(uint8_t chan_vol, int32_t nr50_sum) {
        if (chan_vol == 0u || nr50_sum <= 0) {
            return 0u;
        }
        uint32_t master_scale = (uint32_t)(nr50_sum + 1) / 2u; // 1..8
        uint32_t v = ((uint32_t)chan_vol * master_scale + 4u) / 8u;
        if (v < 1u) v = 1u;
        if (v > 15u) v = 15u;
        return (uint8_t)v;
    }

    void compute_piezo_tone(uint32_t &out_freq, uint8_t &out_vol) const {
        out_freq = 0;
        out_vol = 0;
        if (!power) {
            return;
        }

        int32_t nr50_sum = nr50_mono_sum();
        if (nr50_sum <= 0) {
            return;
        }

        // Priority 1: Active CH1 with frequency sweep (classic GB sound effects)
        if (ch1.active && ch1.dac_enabled(regs[2]) && channel_routed(0) &&
            sweep_enabled && sweep_shift > 0u && sweep_period > 0u && ch1.volume > 0u) {
            uint32_t denom = 2048u - (uint32_t)ch1.freq_raw;
            if (denom == 0u) denom = 1u;
            uint32_t f = 131072u / denom;
            if (f < 100u) f = 100u;
            if (f > 8000u) f = 8000u;
            out_freq = f;
            out_vol = scale_piezo_vol(ch1.volume, nr50_sum);
            return;
        }

        // Priority 2: Freshly triggered loud CH4 percussion chirp
        if (ch4.active && ch4.dac_enabled(regs[0x11]) && channel_routed(3) &&
            ch4.trigger_age == 0u && ch4.volume >= 6u) {
            uint8_t nr43 = regs[0x12];
            uint32_t s = (nr43 >> 4) & 0x0Fu;
            uint32_t r = nr43 & 0x07u;
            uint32_t f = (s >= 7u) ? 750u : (4200u - s * 480u - r * 35u);
            f += (uint32_t)(ch4.lfsr & 0x0Fu) * 20u;
            if (f < 700u) f = 700u;
            if (f > 4500u) f = 4500u;
            out_freq = f;
            out_vol = scale_piezo_vol(ch4.volume, nr50_sum);
            return;
        }

        // Priority 3: Best active melodic channel (CH1, CH2, CH3)
        int32_t best_score = -1;
        uint32_t best_freq = 0;
        uint8_t best_vol = 0;

        auto consider = [&](uint32_t f, uint8_t vol, bool is_pulse, uint16_t age) {
            if (vol == 0u) {
                return;
            }
            int32_t score = (int32_t)vol * 4
                          + (is_pulse ? 10 : 0)
                          + ((f >= 220u && f <= 4500u) ? 12 : 0)
                          + ((age <= 2u) ? 8 : 0);
            if (score > best_score) {
                best_score = score;
                best_freq = f;
                best_vol = vol;
            }
        };

        if (ch1.active && ch1.dac_enabled(regs[2]) && channel_routed(0) && ch1.volume > 0u) {
            uint32_t denom = 2048u - (uint32_t)ch1.freq_raw;
            consider(131072u / (denom ? denom : 1u), ch1.volume, true, ch1.trigger_age);
        }
        if (ch2.active && ch2.dac_enabled(regs[7]) && channel_routed(1) && ch2.volume > 0u) {
            uint32_t denom = 2048u - (uint32_t)ch2.freq_raw;
            consider(131072u / (denom ? denom : 1u), ch2.volume, true, ch2.trigger_age);
        }
        if (ch3.active && ch3.dac_enabled(regs[0x0A]) && channel_routed(2)) {
            static constexpr uint8_t WAVE_VOL_MAP[4] = {0, 15, 8, 4};
            uint8_t wvol = WAVE_VOL_MAP[(regs[0x0C] >> 5) & 0x03u];
            if (wvol > 0u) {
                uint32_t denom = 2048u - (uint32_t)ch3.freq_raw;
                consider(65536u / (denom ? denom : 1u), wvol, false, ch3.trigger_age);
            }
        }

        if (best_score < 0) {
            return;
        }

        // Fold sub-180 Hz solo bass up by octaves so the piezo can reproduce it audibly
        while (best_freq < 180u) {
            best_freq <<= 1;
        }
        if (best_freq < 100u) best_freq = 100u;
        if (best_freq > 8000u) best_freq = 8000u;

        out_freq = best_freq;
        out_vol = scale_piezo_vol(best_vol, nr50_sum);
    }
};

enum AudioMode {
    AUDIO_PWM_APU    = 0, // 4-channel polyphonic APU over 61.0 kHz ultrasonic PWM
    AUDIO_PIEZO_BEEP = 1, // Direct frequency piezo beeper tones
    AUDIO_OFF        = 2  // Muted
};

struct GbAudioDriver;

#ifndef GB_AUDIO_HOST_TEST
static GbAudioDriver *g_gb_audio_driver = nullptr;
static void __not_in_flash_func(gb_audio_pwm_irq_handler)();
#endif

struct GbAudioDriver {
    static constexpr uint32_t RING_SIZE = 2048u;
    static constexpr uint32_t RING_MASK = RING_SIZE - 1u;
    static constexpr uint32_t TARGET_BUFFER_SAMPLES = 1500u;

    GbApu apu;
    AudioMode mode = AUDIO_PIEZO_BEEP;
    bool paused = false;
    uint16_t ring[RING_SIZE] = {};
    volatile uint32_t ring_read = 0;
    volatile uint32_t ring_write = 0;
    volatile uint16_t last_pwm_level = 0;
    uint32_t last_beep_freq = 0;

    uint8_t get_volume() const {
        return apu.master_volume;
    }

    void set_volume(uint8_t pct) {
        if (pct > 100u) {
            pct = 100u;
        }
        apu.master_volume = pct;
    }

    const char *mode_label() const {
        switch (mode) {
            case AUDIO_PWM_APU:    return "AUDIO: 4-CH PWM";
            case AUDIO_PIEZO_BEEP: return "AUDIO: PIEZO BEEP";
            case AUDIO_OFF:        return "AUDIO: MUTED";
            default:               return "AUDIO: UNKNOWN";
        }
    }

#ifndef GB_AUDIO_HOST_TEST
    void init() {
        // Stop PicoSystem's built-in 1 ms audio timer so we own PWM Slice 5 exclusively
        cancel_repeating_timer(&picosystem::_audio_update_timer);

        apu.reset();
        paused = false;
        g_gb_audio_driver = this;

        const uint audio_slice = pwm_gpio_to_slice_num(GB_AUDIO_PIN);
        pwm_config cfg = pwm_get_default_config();
        pwm_init(audio_slice, &cfg, true);
        gpio_set_function(GB_AUDIO_PIN, GPIO_FUNC_PWM);

        pwm_config pacer_cfg = pwm_get_default_config();
        pwm_init(GB_AUDIO_TIMER_SLICE, &pacer_cfg, false);

        irq_set_exclusive_handler(PWM_IRQ_WRAP, gb_audio_pwm_irq_handler);
        set_mode(AUDIO_PIEZO_BEEP);
    }

    void pause() {
        paused = true;
        const uint audio_slice = pwm_gpio_to_slice_num(GB_AUDIO_PIN);
        const uint chan = pwm_gpio_to_channel(GB_AUDIO_PIN);
        irq_set_enabled(PWM_IRQ_WRAP, false);
        pwm_set_irq_enabled(GB_AUDIO_TIMER_SLICE, false);
        pwm_set_enabled(GB_AUDIO_TIMER_SLICE, false);
        pwm_clear_irq(GB_AUDIO_TIMER_SLICE);
        last_pwm_level = 0;
        pwm_set_chan_level(audio_slice, chan, 0);
    }

    void resume() {
        paused = false;
        ring_read = 0;
        ring_write = 0;
        last_pwm_level = 0;
        last_beep_freq = 0;
        set_mode(mode);
    }

    void set_mode(AudioMode next_mode) {
        const uint audio_slice = pwm_gpio_to_slice_num(GB_AUDIO_PIN);
        const uint chan = pwm_gpio_to_channel(GB_AUDIO_PIN);

        irq_set_enabled(PWM_IRQ_WRAP, false);
        pwm_set_irq_enabled(GB_AUDIO_TIMER_SLICE, false);
        pwm_set_enabled(GB_AUDIO_TIMER_SLICE, false);
        pwm_clear_irq(GB_AUDIO_TIMER_SLICE);
        pwm_set_chan_level(audio_slice, chan, 0);

        mode = next_mode;
        ring_read = 0;
        ring_write = 0;
        last_pwm_level = 0;
        last_beep_freq = 0;

        if (paused) {
            return;
        }

        if (mode == AUDIO_PWM_APU) {
            float sys_hz = (float)clock_get_hz(clk_sys);
            // 61.0 kHz ultrasonic PWM carrier on Slice 5B for stronger piezo drive without switching loss
            float carrier_div = sys_hz / (GB_AUDIO_PWM_CARRIER_HZ * (float)(GB_AUDIO_PWM_WRAP + 1u));
            if (carrier_div < 1.0f) carrier_div = 1.0f;
            pwm_set_clkdiv(audio_slice, carrier_div);
            pwm_set_wrap(audio_slice, GB_AUDIO_PWM_WRAP);
            pwm_set_counter(audio_slice, 0);
            pwm_set_chan_level(audio_slice, chan, 0);

            // 32.8 kHz sample-rate pacer IRQ on internal Slice 0
            float pacer_div = sys_hz / ((float)GB_AUDIO_SAMPLE_RATE * (float)(GB_AUDIO_PACER_WRAP + 1u));
            if (pacer_div < 1.0f) pacer_div = 1.0f;
            pwm_set_clkdiv(GB_AUDIO_TIMER_SLICE, pacer_div);
            pwm_set_wrap(GB_AUDIO_TIMER_SLICE, GB_AUDIO_PACER_WRAP);
            pwm_set_counter(GB_AUDIO_TIMER_SLICE, 0);
            pwm_clear_irq(GB_AUDIO_TIMER_SLICE);
            pwm_set_irq_enabled(GB_AUDIO_TIMER_SLICE, true);
            pwm_set_enabled(GB_AUDIO_TIMER_SLICE, true);
            irq_set_enabled(PWM_IRQ_WRAP, true);
        } else if (mode == AUDIO_PIEZO_BEEP) {
            pwm_set_clkdiv(audio_slice, 10.0f);
            pwm_set_wrap(audio_slice, 5000u);
            pwm_set_counter(audio_slice, 0);
            pwm_set_chan_level(audio_slice, chan, 0);
        }
    }

    void cycle_mode() {
        set_mode((AudioMode)(((int)mode + 1) % 3));
    }

    void restore_from_hram(const uint8_t *hram_io) {
        bool was_paused = paused;
        pause();
        apu.restore_from_hram(hram_io);
        if (!was_paused) {
            resume();
        }
    }

    void update_frame() {
        if (paused) {
            return;
        }
        apu.advance_frame_ages();

        if (mode == AUDIO_PWM_APU) {
            uint32_t r = ring_read;
            uint32_t w = ring_write;
            uint32_t avail = (w - r) & RING_MASK;
            uint32_t free_space = (RING_SIZE - 1u) - avail;

            int32_t needed = (int32_t)TARGET_BUFFER_SAMPLES - (int32_t)avail;
            if (needed < 0) needed = 0;
            if (needed > 1200) needed = 1200;
            if ((uint32_t)needed > free_space) needed = (int32_t)free_space;

            for (int32_t i = 0; i < needed; ++i) {
                ring[w] = apu.render_sample();
                w = (w + 1u) & RING_MASK;
            }
            __dmb();
            ring_write = w;
        } else if (mode == AUDIO_PIEZO_BEEP) {
            for (uint32_t i = 0; i < 549u; ++i) {
                (void)apu.render_sample();
            }

            uint32_t freq = 0;
            uint8_t vol = 0;
            apu.compute_piezo_tone(freq, vol);

            const uint slice = pwm_gpio_to_slice_num(GB_AUDIO_PIN);
            const uint chan = pwm_gpio_to_channel(GB_AUDIO_PIN);

            if (freq == 0u || vol == 0u || apu.master_volume == 0u) {
                pwm_set_chan_level(slice, chan, 0);
            } else {
                uint16_t wrap = (freq < 250u) ? 25000u : 5000u;
                if (freq != last_beep_freq) {
                    float sys_hz = (float)clock_get_hz(clk_sys);
                    float clkdiv = sys_hz / ((float)wrap * (float)freq);
                    if (clkdiv < 1.0f) clkdiv = 1.0f;
                    if (clkdiv > 255.0f) clkdiv = 255.0f;
                    pwm_set_clkdiv(slice, clkdiv);
                    pwm_set_wrap(slice, wrap);
                    if (pwm_get_counter(slice) >= wrap) {
                        pwm_set_counter(slice, 0);
                    }
                    last_beep_freq = freq;
                }
                uint32_t max_level = (freq * (uint32_t)wrap) / 10000u;
                if (max_level > (uint32_t)wrap / 2u) max_level = (uint32_t)wrap / 2u;
                if (max_level < 1u) max_level = 1u;

                uint32_t level = ((uint32_t)vol * (uint32_t)vol * max_level) / 225u;
                level = (level * (uint32_t)apu.master_volume * (uint32_t)apu.master_volume) / 10000u;
                if (level < 1u) level = 1u;
                pwm_set_chan_level(slice, chan, (uint16_t)level);
            }
        } else {
            for (uint32_t i = 0; i < 549u; ++i) {
                (void)apu.render_sample();
            }
        }
    }
#else
    void init() {
        apu.reset();
        paused = false;
        set_mode(AUDIO_PIEZO_BEEP);
    }

    void pause() {
        paused = true;
    }

    void resume() {
        paused = false;
        ring_read = 0;
        ring_write = 0;
        last_pwm_level = 0;
        last_beep_freq = 0;
    }

    void set_mode(AudioMode next_mode) {
        mode = next_mode;
        ring_read = 0;
        ring_write = 0;
        last_pwm_level = 0;
        last_beep_freq = 0;
    }

    void cycle_mode() {
        set_mode((AudioMode)(((int)mode + 1) % 3));
    }

    void restore_from_hram(const uint8_t *hram_io) {
        bool was_paused = paused;
        pause();
        apu.restore_from_hram(hram_io);
        if (!was_paused) {
            resume();
        }
    }

    void update_frame() {
        if (paused) {
            return;
        }
        apu.advance_frame_ages();
        if (mode == AUDIO_PWM_APU) {
            uint32_t r = ring_read;
            uint32_t w = ring_write;
            uint32_t avail = (w - r) & RING_MASK;
            uint32_t free_space = (RING_SIZE - 1u) - avail;
            int32_t needed = (int32_t)TARGET_BUFFER_SAMPLES - (int32_t)avail;
            if (needed < 0) needed = 0;
            if (needed > 1200) needed = 1200;
            if ((uint32_t)needed > free_space) needed = (int32_t)free_space;
            for (int32_t i = 0; i < needed; ++i) {
                ring[w] = apu.render_sample();
                w = (w + 1u) & RING_MASK;
            }
            ring_write = w;
        } else {
            for (uint32_t i = 0; i < 549u; ++i) {
                (void)apu.render_sample();
            }
            if (mode == AUDIO_PIEZO_BEEP) {
                uint32_t freq = 0;
                uint8_t vol = 0;
                apu.compute_piezo_tone(freq, vol);
                last_beep_freq = freq;
            }
        }
    }
#endif
};

#ifndef GB_AUDIO_HOST_TEST
static void __not_in_flash_func(gb_audio_pwm_irq_handler)() {
    pwm_clear_irq(GB_AUDIO_TIMER_SLICE);

    GbAudioDriver *drv = g_gb_audio_driver;
    if (!drv) {
        return;
    }

    uint32_t r = drv->ring_read;
    uint16_t level = drv->last_pwm_level;
    if (r != drv->ring_write) {
        level = drv->ring[r];
        __dmb();
        drv->ring_read = (r + 1u) & GbAudioDriver::RING_MASK;
        drv->last_pwm_level = level;
    }
    const uint audio_slice = pwm_gpio_to_slice_num(GB_AUDIO_PIN);
    pwm_set_chan_level(audio_slice, pwm_gpio_to_channel(GB_AUDIO_PIN), level);
}
#endif
