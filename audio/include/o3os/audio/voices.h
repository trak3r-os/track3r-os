// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sampler voices (OS-005d lite; ADR 0013 D3, D5; docs/specs/OS-005-audio-engine.md "005d-lite as built"): a fixed pool of kMaxVoices voices that play
// int16 mono samples from memory (SDRAM on the MK66), driven by the sequencer's note events at their sample offsets inside the block, one mono Q15 block a
// sounding track, which the graph takes as the strip's input (SourceKind::Feed, Graph::feed). Routing, level, pan, sends and the master stay the graph's.
//
//   ONE VOICE A TRACK: a new note on a track takes that track's voice over where it stands (the retrigger); a note on a track with no voice
//   takes a free one, and with none free the OLDEST playing voice is stolen (a fading one first: it only holds a click's tail). So a track past the pool's size costs
//   the track whose note is oldest, and nothing else. The pool (12, a pattern's tracks) is smaller than the graph's tracks (kVoiceTracks, 16: the live notes take
//   the spare ones), so the sequencer alone never steals, and a live note over 12 sounding tracks steals the oldest voice (the CPU cap, OS-STATUS OS-VOX CPU).
//
//   PITCH: 12-TET from the root note (kRootNote = 60 plays the sample at its own rate) as a Q32.32 increment of the
//   sample position, a table of the 12 semitones in Q30 and a shift for the octave (pitch_inc_q32). The instrument's tune shifts the note index and its fine
//   tune moves the pitch toward the neighbouring semitone (tuned_inc_q32; OS-007f). The sample is read with a linear interpolation whose
//   fraction is the top 15 bits of the position's fraction. The sample's own rate (SampleDesc::rate_q28, OS-RATEPITCH) scales the increment: 44,100 Hz or none recorded is the
//   engine's, any other rate plays at its true pitch. No correction between a 44,100 Hz file and the engine's 44,117.65 Hz is applied (open item V4 in the spec).
//
//   BEAT SLICE (OS-007h, the beat slice play mode): the note does not set the pitch, it picks the slice: slice = note - kSliceRootNote
//   (24, the note of the pad board's first pad), clamped to the instrument's slices, and the slice plays one shot at the root's pitch (note 60, the instrument's tune and
//   fine tune still apply). The slice boundaries are a table the loader leaves in the sample arena (SampleDesc::slices). WAVETABLE (mode 6): the sample is a
//   stack of windows of `wt_size` frames, the loader binds the current window as a loop over the whole of `data`, and the pitch law is the wavetable's: the
//   note table times wt_size x 523.2511306 / 44100 (this OS's wavetable pitch law), so note 60 is C-5 at 523.25 Hz whatever the window size. OS-GRAN: the descriptor also
//   says how many windows the stack has (wt_windows) and which one `data` is (wt_pos), so that the POSITION MODULATION below can move the voice between them.
//
//   GRANULAR (OS-GRAN; the granular play mode): the voice plays a GRAIN of the sample, a short window around the instrument's position, looped, with an
//   amplitude shape over it. The sample the descriptor gives is the WHOLE sample (the instrument's start, end and loop points are not used by this mode).
//   GEOMETRY (grain_geom, once a note and at a position refresh): positions are 16-bit fractions of the sample (0..65535); the grain's length in those units is
//   grain_length x 65535 / the sample's length, the grain is [pos - len/2, pos + len/2] held to 0..65535, its loop runs from start + 1 to end - 1, and the four points are made
//   sample indexes by the truncated float product point x (length / 65535). The voice reads from the grain's start; its position x counts from there. TYPE (SampleDesc::loop_mode, the
//   instrument's granular type): 0 FORWARD: x runs from 0 and, past the loop's end, wraps to loop1 + the excess; 1 BACKWARD: x runs backward and wraps from loop1 to loop2 (it starts
//   at 0, so it jumps to the end at once); 2 PING-PONG: forward from 0, reflecting about loop2 - 0.5 at the top and loop1 + 0.5 at the bottom (p' = 2 x loop2 - 1 - p). SHAPE (the
//   instrument's granular shape): 0 square (a constant 65535), 1 triangle, 2 gauss (a half sine), 1000-entry gain tables of 16 bits indexed by
//   (x - loop1) x 999 / (loop2 - loop1) (integer, exact here: a Q32 reciprocal per grain), the sample times the gain >> 16, then the voice's amplitude. The half sine is
//   round(65535 sin(pi i / 1000)) and the triangle round(65535 i / 500) up and round(65535 (1000 - i) / 500) down, with the last entry (index 999) 0; 501 half-sine entries
//   in flash, the triangle a formula.
//   The pitch is the note's, as for an ordinary sample (the note, tune, fine tune, and since OS-IFX1 the glide). The position of the grain (SampleDesc::gr_pos 0..65535) is the
//   instrument's, or the position modulator's (POSITION MODULATION): a new position takes effect at the grain's next wrap or turn, not at once.
//   Guards: reads never leave the sample (an index is held inside it, so the interpolation stays on near the end), the envelope index is held to 0..999 (for x < loop1 at the very
//   start of a grain), a degenerate grain (loop2 <= loop1) is one sample long, a step longer than the grain wraps modulo the grain, the interpolation is the plain 15-bit lerp at the
//   real position, and a ping-pong grain does not jump forward when its first step lands under loop1. Whether the official firmware's granular mode does the same at these edges is an
//   open research question (Q), as is its pitch near the sample's edges: the release notes (1.7.0, PAR-125 / PAR-126) mention a change there, no description of it is available, and
//   nothing of it is here. The grain loop runs from FLASH (O3OS_AUDIO_COLD, once a span a voice: SRAM_L has no room, and a loop of 128 iterations pays its cache misses once).
//
//   LOOPS: one shot, or a loop over [loop_start, loop_end) (at least 2 samples) that runs FORWARD (play mode 1), BACKWARD (2) or PING-PONG (3). Forward: the
//   interpolation reads across the loop point (the sample after loop_end - 1 is loop_start) and a one shot holds its last sample at the end, so no guard
//   sample is needed and no read leaves [0, length). Backward and ping-pong (OS-007k; the normal variants, the reversed-sample variants are not here): the note plays FORWARD from the start up to
//   loop_end, and there it turns: the position reflects about loop_end - 0.5 (p' = 2 x loop_end - 1 - p, so the last sample is played twice) and runs
//   backward. Backward loop (2): at loop_start (p <= loop_start) it jumps to the loop end (p' = p + loop length) and keeps running backward for ever; the
//   voice never plays forward again. Ping-pong (3): at loop_start it reflects about loop_start + 0.5 (p' = 2 x loop_start + 1 - p) and runs forward, and so on.
//   The interpolation is the plain lerp at the real position, so a backward read is the same lerp; its "next" sample is held at loop_end - 1 (a read never goes past the loop's end).
//   Whether the official firmware's backward and ping-pong loops turn and read the same way is an open research question (Q). A step so large that one reflection does not bring the
//   position back inside the loop (about 49x the loop length a sample, never in music) is clamped into it: no read ever leaves [0, loop_end).
//
//   SLICE FX (OS-007k): the step FX Slice (25) forces the slice a beat slice instrument plays: while it is on for a track, a note on that track plays slice
//   `value % slice_count` whatever its pitch names (the value is taken modulo the instrument's slice
//   count and the forced slice replaces the note's). The Engine hands ModSet / ModEnd of the group to
//   set_forced_slice() / clear_forced_slice(); FxReset clears every track. Only the beat slice mode reads it. (Whether the official firmware does the same is Q.)
//
//   AMP ENVELOPE (OS-007k): the instrument's envelope slot 0 (delay, attack, hold, decay, sustain, release and amount), when it is enabled and not an LFO
//   (an instrument whose slot 0 is an LFO keeps the plain gain: the tremolo is AMP LFO MODULATION below). It is the sample player's envelope (set up per note; NOT the generator that drives the other slots), with this
//   timing (whether the official firmware's stage timing is the same is Q): every stage is a whole number of 8-sample groups, at least one (a stage of 0 ms lasts 8 samples), at most 65535 (11.9 s); the stage values go
//   Delay (silent: the sample is already running under it) -> Attack (0 to 1) -> Hold (1) -> Decay (1 to sustain) -> Sustain (held until the note is
//   released) -> Release (from where it is to 0 over the release time, then the voice is gone). NoteOff releases over the release time; NoteFade over 300 ms
//   (kFadeEnvUnits); a NoteOff in the delay ends the note at once; a
//   second NoteOff starts the release again from where the level is; NoteCut stops it at once, as for any voice. The
//   envelope REPLACES the attack and release ramps of a note without one. `amount` multiplies the gain, the instrument's or the velocity's (the mixer level table's gain of the volume x amount)). A voice without an envelope pays nothing for it. The envelope's value is kept in Q30 and
//   the voice's amplitude in Q24 (a Q16 gain with 8 more bits, so that an 11 s attack moves by whole units a sample), walked in linear segments that end at
//   a stage boundary or the end of the render span; at most one division (SDIV) a segment, none in the sample loop.
//
//   NO CLICKS (all linear, in samples, no filters): a note starts with an attack ramp (kAttackSamples) from silence; NoteOff releases over
//   kReleaseSamples, NoteFade over kFadeSamples (300 ms, events.h); and whatever a voice was putting out when it stopped abruptly (a retrigger, a
//   NoteCut, the end of a one shot, a stolen voice's own track excepted) carries on as a residual that fades to zero over kDeclickSamples, so the output is
//   continuous at every cut. The residual lives in the voice, which keeps its track until it is gone.
//
//   LEVEL (OS-007i): a note's gain is a Q16 value that may exceed unity (the 1.2+ instrument law reaches +24 dB, 15.85x: smoothing.h, THE LEVEL LAWS). The
//   instrument's own gain (SampleDesc::gain) is computed by the loader from its stored volume 0..100 and the project's origin; kVelocityDefault takes it.
//   A VELOCITY a step names is linear, v / 127 (unity at 127) and REPLACES the instrument's gain, so a step velocity is not scaled by the instrument's volume:
//   OPEN ITEM, how the official firmware combines a velocity with its volume table is an open
//   research question, and no law is invented here. A voice's output is saturated to int16, never wrapped.
//
//   PAN (OS-007k): SampleDesc::pan 0..100 (50 centre). Where a note starts, the Engine gives its track the instrument's pan (Graph::set_voice_pan); whether the
//   official firmware does the same is an open research question (Q). The pan law is smoothing.h's. Without a strip pan the instrument's pan IS the track's pan; the graph's strip pan is a Community OS addition (a MED import's track
//   pan, the mixer's), and the two add around the centre: effective = clamp(strip pan + (instrument pan - 50), 0, 100). With the strip at the centre (unless the mixer or a MED import moved it)
//   this is the instrument's pan alone, and an instrument at the centre leaves the strip's own pan. The pan modulator (slot 1) is PAN MODULATION below; the step FX Pan is not there yet.
//
//   FILTER (OS-007l; svf.h has the maths and the citations): the instrument's low-pass, high-pass or band-pass (SampleDesc::filter: type, cutoff 0..1, resonance 0..4.3, enable),
//   a state variable filter, one a track, between the player and the track's amplifier (player -> filter -> amplifier): here, after the voice's gain,
//   envelope and declick residual, so the filter's input is the voice's output. A disabled filter is bypassed: no work and the output is bit for bit what it was.
//   The filter state is the voice's: a retrigger on the track keeps it when both notes filter (this OS's choice: the filter keeps its state across notes), any other start clears it.
//   When the voice is gone (its residual too) the filter's own ringing is not carried on: the filter stops being fed and so its tail is cut
//. The coefficients are set per note and per Cutoff FX event, not per block.
//   CUTOFF FX (steps FX 27 Low-pass, 28 High-pass, 29 Band-pass): value 0..100 is the cutoff (value / 100), the type is the FX's, and the
//   filter is ON whatever the instrument says. The Engine hands ModSet / ModEnd of the filter group to set_filter_fx() / clear_filter_fx(); a track's FX holds for its next notes and
//   acts on the voice sounding now; when it ends the voice goes back to the instrument's filter (cutoff, type, and off when the
//   instrument has it off). The resonance is the instrument's; an instrument whose filter is off has none to give: here Q = 0.7 (our choice, resonance 0). The Filter LFO FX (35) is below, under FILTER LFO FX. NOT here: the performance layer's LFO rate (the Perform screen).
//
//   PERFORMANCE FILTER (OS-011b; new Community OS behaviour): the Perform screen's low-pass, high-pass and band-pass slots hold the filter through the control
//   table's performance layer (seq/control.h), whose winner reaches the voices as a ModSet of the Filter group with layer kLayerPerf and a value that carries the FX's type and a cutoff
//   OFFSET (events.h perf_filter_value). The
//   filter is on whatever the instrument says; its type is the slot's (1 low, 2 high, 3 band); the cutoff is base + offset / 100 held to 0..1, over the base the
//   sequencer's Cutoff FX has when the track holds one, else the instrument's; the resonance is the instrument's, or
//   Q 0.7 when its filter is off (as for the Cutoff FX). It acts on the voice sounding now and on the track's next notes; clearing it returns the Cutoff FX or the instrument's
//   own filter. The layer is its own: the transport start (FxReset) ends the sequencer's FX and not this one.
//   The filter modulation's base is the performance cutoff while it holds (the envelope adds to it).
//
//   PERFORMANCE LAYER, THE REST OF THE GROUPS (OS-PERFFX P0 and P1, docs/specs/OS-PERFFX.md; new Community OS behaviour). Engine::dispatch hands fx_event() the whole ModSet / ModEnd, so
//   that layer_event() sees the layer (`c`) and the layer's own value (`value`) of every group it does not act on in flash by itself. For the groups the voices take from Perform
//   (kModGroupOverdrive, kModGroupBitDepth; the Perform screen's OVERDRV and BITDEPTH) it keeps the layer's value and a held flag per track in perf_, beside the step layer's (ifx_val_ / ifx_on_): a
//   ModSet whose layer is kLayerPerf holds the value, any other ModSet and a ModEnd let it go (the control table sends the step FX again when the layer ends over one). Like the performance filter,
//   the layer is its own: FxReset (clear_slide_fxs, clear_filter_fxs) and the Stopped event leave it; an export puts it away and brings it back (perf_save / perf_clear / perf_restore,
//   Engine::offline_enter and offline_leave): the export's sequencer has no layer, so the voices must neither carry the live one into the export's notes nor lose it to the export's own events.
//   A project load ends the layer by the ordinary road, not by a reset here: PerformBank::load_slots lets every pad go, its next apply() tells the sequencer to end each FX it had sent, and the
//   control table's ModEnd reaches layer_event (the slot lets go) and the Filter group's handler (the performance filter). P1 (OVERDRV, BITDEPTH): shape_set takes the layer's value over the step
//   FX's and the instrument's: OVERDRV 1..100 is the overdrive amount, BITDEPTH 1..15 is the shift (16 - bits), for the voice sounding now and for the track's next notes; the layer's end brings the
//   step FX back (the control table sends it again) or the instrument's value. Priority: Perform, then the step FX, then the instrument.
//   TUNE (OS-PERFFX P5; kModGroupTune, the Perform screen's TUNE, -48..48 semitones; 0 ends it; sampler voices only: the Amiga voices and the module instruments do not follow it). The layer's value is a
//   BASE of the pitch offset the Slide FX already moves (Voice::slide_q16 / slide_tgt, 1/65536 semitone): a note starts at the base, not at 0, and every writer keeps it as the offset's zero (the Slide FX's
//   ramp goes to its target + base and back to the base, the glide to the base, the tracker FX's offset lands on top of it); a TUNE that changes while a voice sounds adds the difference to the offset and
//   the target, so a ramp in progress goes on around the new base. The pitch itself is still made in flash by repitch() once a block (mod_block, kMmSlide), so the layer costs the voice nothing in the
//   render loops. The fine tune and its modulator add on top, as before; the pitch is held at the ends of the table (slid_inc_q32). The layer is its own: FxReset and Stopped leave it, the export puts it away.
//
//   SMP POS AND SMP END (OS-PERFFX P7; kModGroupPosition / kModGroupSampleEnd, the Perform screen's SMP POS and SMP END, -100..100; 0 ends them; docs/specs/OS-PERFFX.md section 10, new Community OS
//   behaviour). At a NOTE'S START only, in flash (note_on): the layer's value is a percent of the part the note plays (the slice the note names, else the whole sample; for a looped sample SMP POS reads
//   the frames up to loop_end and SMP END is ignored; a granular or wavetable instrument ignores both). A positive value is that percent from the start of the part, a negative one |v| percent back from
//   its end: SMP POS is the first frame (Voice::phase), SMP END the last (Voice::end). A start at or past the end frame plays nothing (note_on cuts the track's note and returns false, as for a beat slice
//   with no length). A voice that sounds is not moved, so no render loop, no closed-form advance and no mod_block line knows about them: the position is a plain phase < end, which both already take.
//   Sampler voices only, as TUNE; the tracker Retrigger FX restarts at the note's window start (Voice::gstart: the SMP POS frame, else 0).
//
//   PLAYBACK (OS-PERF-REVERSE, P8; kModGroupReverse, the Perform screen's PLAYBACK 0..1 and the step FX 26 with 1; docs/specs/OS-PERFFX.md section 10.5, new Community OS behaviour). At a NOTE'S START only: a one
//   shot or beat slice note (no loop, not a wavetable, not granular) of a track that holds the layer's PLAYBACK (perf_rev_) or the step FX 26 (step_rev_) plays its window [Voice::gstart, Voice::end) (the whole
//   part, the slice, or SMP POS .. SMP END) from the last frame down to the first and stops: mods_start (flash) makes the voice a "backward one shot" (mode kModeReverse, loop_len = the window, bwd from the
//   start), which render_seg already sends to run_dir; run<true, true> stops where a backward loop would wrap (the window's first frame is played, the position below it ends the note; a stop at the last sample of
//   a call ends at the next call, which returns 0). Looped samples, granular and wavetable instruments, Amiga voices, module instruments and MIDI tracks ignore it; a sounding voice is not turned round. A
//   backward voice has mode != 0, so the gate never advances it in closed form (it renders, like a backward loop), and the tracker Retrigger restarts it at the window's last frame. The layer's mask is the
//   layer's own (FxReset and Stopped leave it, the export puts it away); the step FX's ends with the transport start (tracker_reset) and with the export's start and end.
//
//   FILTER MODULATION (OS-007m): the instrument's slot 2 is the cutoff's modulator, ONE of two (the instrument's loop flag picks): an ADSR (enable, loop 0) or an LFO (enable, loop 1, shape,
//   rate and amount from the instrument's cutoff LFO fields); it runs only when the instrument's own filter is on, not for a filter that only the Cutoff FX
//   turned on. Its output (-1..1 around the base) is ADDED to the base cutoff, which is the Cutoff FX's value when the track has one, else the instrument's, and the sum is held to 0..1
//   (base + mod, clamped). This OS steps it at a CONTROL RATE inside the block: the cutoff is
//   re-evaluated at every 16th sample of the block (kFilterCtl, 4 times a block, 0.36 ms) and
//   held in between, so what a voice makes does not depend on how events split the block. ADSR (written out from its formulas, not copied): delay (the level of
//   the previous note's envelope when this is a retrigger, else 0), attack (linear from that level to `amount`, never below it), hold (amount), decay (linear to sustain x amount), sustain
//   (held while the note is), release (linear from the level it was at to 0, from a NoteOff, a NoteFade or the release; a NoteOff in the delay ends it at once), each stage a number of
//   milliseconds as stored (44.1 samples a ms at the engine's rate), the remainder of a stage carried into the next;
//   amount is -1..1 and the output is held to it. LFO: driven by the transport: its phase is not the note's but the SEQUENCER's tick
//   count (576 ticks a step, since the transport started) plus 576 x (the step the note started in % 36), over a period of (576 x rate) ticks (uint32 truncation:
//   a third of a step is 191 ticks), the rate being the .pti's 0..28 index into the 29 tempo-sync rates in steps, 128 .. 1/64. The engine hands the voices the tempo once a block
//   (set_tempo_x100), the Step events (on_step: they snap the tick count to the step grid, so swing does not move it) and the transport start (restart_clock); between Step events
//   the count runs by the tempo, held at the step's end. Shapes, each with u the phase 0..1 and A the amount: saw A(2u - 1), reverse saw A(1 - 2u), triangle A(4u - 1) then A(3 - 4u), square +A then -A, random a new uniform level in
//   [-A, A) every period (the first is drawn at the note). It lives while the voice does (it stops when the voice's release has ended). A voice
//   with no modulator costs one flag test a span and is bit for bit what it was. The tick count follows the song cursor's step, not track 0's; the ADSR of a retriggered note starts
//   from the previous level only while the old voice or its click tail is held. The transport stopped: LFOS WHILE STOPPED below (it was: the LFO held its position).
//
//   LFOS WHILE STOPPED (OS-BUG-WTLFO; all six LFO slots share this clock: the tremolo, the pan, the finetune, the cutoff, the wavetable window and the granular position): this OS held them while the transport
//   was stopped (what the official firmware does then is an open research question, Q: docs/specs/OS-005-audio-engine.md, OS-007m), so a pad or a MIDI key played with the transport
//   stopped sounded a frozen modulation. There is no unit capture and no phase or rate is recovered: the behaviour here is NEW Community OS behaviour that closes a gap of this OS,
//   not a parity claim. The rule: from the Stopped event until the next Step event the count is NOT held at the end of its step but goes on at the tempo in force (the sequencer's tempo_x100() is the project's
//   tempo while stopped), carrying into the next step (running_, tick_steps_), so the LFO is as it would be on a running transport that never stopped. A stop takes the count on from where it was; the transport
//   START (FxReset) restarts the count at 0 as before, and a voice that sounds when the transport had been stopped KEEPS ITS PHASE: its shift is made its phase at that moment (clock_restart_impl; a random level
//   already drawn is held), so the sweep does not jump. The same for the first Step event after a stop that no start preceded (a Continue, a resume: resume_phase), and for a live SPEED edit (lfo_live keeps u). A restart while the transport ran, and everything between Step events while it runs, is what it was: the count held at the step's last tick.
//   (The count is in 576-tick steps in a u32: it wraps after about 10 days of an idle stopped transport at 120 BPM, a phase jump of an LFO that is sounding then.)
//
//   FILTER LFO FX (OS-FU2; step FX 35, value 0..28 the tempo-sync rate index): while the track holds it
//   (ModSet / ModEnd of seq::FxGroup::LfoFilter, kModGroupLfoFilter) the cutoff modulator of the voices on the track is an LFO of the instrument's lfo[2] shape and amount at the FX's rate,
//   whatever the instrument's own modulator is (an ADSR, nothing, or an LFO of another rate: then only the rate changes and the LFO goes on at its phase of the transport). It still
//   needs the instrument's own filter on, so an instrument with its filter off makes no modulation of it. A note that starts under
//   the FX takes it; the voice sounding when it starts takes it at once (the LFO from its cursor step), and when it ends the voice goes back to the instrument's modulator: an LFO at the
//   instrument's rate, or its ADSR started over (resumed mid-phase with the other parameters: an APPROXIMATION here), or none. The FX holds the cutoff modulation only:
//   the base (the instrument's or the Cutoff FX's) is as before. Like the Cutoff FX it ends with the transport (FxReset). The transport-driven phase is the cursor step's.
//
//   PAN MODULATION, FINETUNE MODULATION (OS-MOD): each is ONE of two, as the cutoff's (FILTER MODULATION): an ADSR
//   (enable, loop 0) or an LFO (enable, loop 1; shape, rate (the 29 tempo-sync rates) and amount from lfo[slot]), the same generator and the same rules (ADSR in milliseconds, kill level at a retrigger,
//   a NoteOff releasing it; the LFO the transport's tick count plus the cursor step's shift, A(2u - 1) for the saw ...), over its own FilterMod: SampleDesc::pan_mod and tune_mod. The
//   output (-1..1) is applied times 100 (truncated), added to the instrument's pan or fine tune and held to 0..100 / -100..100 (an enabled pan or finetune
//   modulator always runs on it). Evaluated once a block per voice, at the voice's
//   first sample of the block (or of the note), where the voice's own control rate is. FINETUNE: the voice's
//   increment is made again from the note, the instrument's tune and the modulated fine tune (tuned_inc_q32: a linear law, 100 is the neighbouring semitone), the position continuous, so a vibrato
//   of amount 1 is a semitone each way. PAN (autopan): one voice is one track's, so the modulated pan is that TRACK's strip pan, which the Engine gives the graph (Graph::set_voice_pan, the
//   instrument's pan offset on the track, smoothed over the block, as a live pan edit is): not a per-voice stereo pair, which would cost a stereo mix a voice; the strip's own pan and the instrument's
//   pan add around the centre as for any note (PAN). A voice that is not modulated costs one flag test a span for all of this and is bit for bit what it was. NEW: the evaluation
//   is once a block; the pan is smoothed by the graph's block ramp; an instrument with the pan modulator moves its track's pan whichever
//   instrument plays on the track next (every note's start sets the pan again).
//
//   AMP LFO MODULATION (OS-MOD; the tremolo): the amp slot (0) with its loop byte set is an LFO, not an ADSR (it takes the
//   instrument's lfo[0] shape, speed and amount, and a rate table of 24 entries, the last 24 of the 29: rate r is kLfoRate[r + kAmpRateBase], held to 23). It runs as a looped envelope
//   of the player, whose level (0..1) multiplies the voice: the gain is the instrument's (or the velocity's) times amount x level, level the LFO's shape of the phase: saw u, reverse saw 1 - u,
//   triangle 2u then 2 - 2u, square 1 then 0, sine (1 - cos 2 pi u) / 2 (OS-LFOSINE, lfo_sine.h), random a level in [0, 1) held for a period (flfo_level: the cutoff's LFO is 2 x this - A, the same generator). The phase is the transport's (the same tick count as
//   the cutoff LFO, the step shift at the note). Here it is a pass over the voice's samples (trem_span), after the voice (its attack and release ramps, the declick residual) and before the filter,
//   inside the player stage: the multiplier is evaluated at every 16th sample of the block (kFilterCtl, with the cutoff's) and walked linearly between, so it does not depend
//   on how events split the block; Q23, one multiply a sample. A negative amount is silence. A NoteOff does not stop the voice at once: the voice's plain release ramp (5.8 ms) plays; the phase is the transport's tick count at each evaluation.
//   WAVETABLE POSITION and GRANULAR POSITION (slots 3 and 4) are POSITION MODULATION below.
//
//   POSITION MODULATION (OS-GRAN; the wavetable and granular position envelopes): slot 3 moves a
//   WAVETABLE's window, slot 4 a GRANULAR instrument's position, each ONE of an ADSR or an LFO as the other modulators (FILTER MODULATION), over SampleDesc::pos_mod (slot 3 runs
//   only for a wavetable instrument and slot 4 only for a granular one, so one descriptor serves both). The output (-1..1) times the number of windows (wavetable) or
//   65535 (granular), cut to a whole number toward zero, is added to the instrument's window (wt_pos) or position (gr_pos) and held to 0..windows-1 / 0..65535. WAVETABLE: the window
//   the voice reads moves at the voice's first sample of a block: no crossfade, a hard step between windows
//   (BASE-16). GRANULAR: the new position is the grain's next-wrap position (GRANULAR). Evaluated once a block per voice, in mod_block, like the pan
//   and the finetune. A voice that has no position modulator costs one flag test a span for it, as for the others.
//
//   SLIDE FX (OS-PFX; step FX 41 Slide up and 42 Slide down, 0..255 sixteenths of a semitone from the step's note, PAR-079: [MAN] 7.36-7.37, 16 is one semitone): while a track holds one
//   (ModSet / ModEnd of seq::FxGroup::Slide, kModGroupSlide: fx_event()) its sampler voice's pitch moves linearly from the note's to the distance the FX names, up or down, in ONE STEP of
//   the tempo in force (the voices' tempo, set_tempo_x100; how long the official firmware's slide takes is an open research question: NEW Community OS behaviour, one constant, slide_start()), and stays there until
//   the note ends or the FX does. A note that starts under the FX begins to slide at once; the voice sounding when the FX arrives (a step without a note) slides from where it is to the
//   distance the FX names FROM THE NOTE, so a second Slide on the next row goes on from the first one's pitch. When the FX ends the pitch goes back to the note's. The slide is evaluated
//   once a block per voice, in mod_block (flash), like the finetune modulator (the pitch is the block's first sample's, a staircase of at most 16/step_samples of a sixteenth a block),
//   and adds to the instrument's tune, its fine tune and the finetune modulator's offset (slid_inc_q32: the same linear law between the semitones). The Amiga voices and an SD module's
//   do not slide (their pitch has no such input), and a MIDI note cannot (it would need a pitch bend: OS-MIDI2). A voice that does not slide costs one flag test a span, as the modulators.
//
//   GLIDE (OS-IFX1; the instrument's GLIDE, 0..15000 ms, and the step FX Glide 19, 0..100): a note whose track played another note before starts at THAT note's pitch and goes to its own in the glide
//   time: ms x 44.1 samples. The pitch
//   starts at the last note the track's player was given, plus tune (it survives note ends and stops; only an audition clears it), a repeat of the same note does not glide,
//   the new note's fine tune stays on top; a beat slice plays note 60 whatever the key, so it never glides; the FX replaces the instrument's time while the track holds it (value 0
//   is no glide at all), mapped as: value <= 50 gives value x 20 ms, above it 1000 + (value - 50) x 280 ms (glide_fx_ms, events.h); the time is read when the note starts. The curve is the OS's
//   own (N): the pitch goes through the Slide FX's ramp (SLIDE FX: one ramp a voice, evaluated once a block in mod_block, no new per-sample or per-voice
//   state), so it is linear in semitones (the official firmware's own ramp is not known: docs/specs/OS-IFX1.md). It lands exactly at the end. A note that
//   starts under a held Slide FX takes the Slide's ramp (Slide wins: one ramp a voice). Sampler voices only (Modern engine: sample, loop modes, wavetable, granular); the Amiga voices, an SD module and
//   a MIDI instrument have no such input.
//
//   OVERDRIVE and BIT DEPTH (OS-IFX1; the instrument's OVERDRIVE 0..100 and BIT DEPTH 4..16, the step FX Overdrive 38 0..100 and Bit depth 39 1..16): two cheap passes over the voice's own block, after
//   its filter and before the track strip, SKIPPED ENTIRELY when neutral (overdrive 0 and 16 bits: a voice with defaults costs one test a span and is bit for bit what it was). The OVERDRIVE curve is NEW
//   Community OS behaviour (the official firmware's is an open research question: docs/specs/OS-IFX1.md, no parity claim); the bit depth is a mask (below). OVERDRIVE is a parabolic soft clipper y = u - u|u| / 4 of u = x g held to |u| <= 2 (unity slope at 0, full scale exactly at |u| = 2), the drive g =
//   1 + 15 (value / 100)^2 (1 to 16, +24 dB), Q11, one multiply for the gain and one for the shape a sample. BIT DEPTH clears the sample's low (16 - bits) bits (the sample is masked: a floor, so a mean offset of half a step
//   below zero; parity with the official firmware is an open research question, docs/specs/OS-IFX1.md) (the Bits12 / Bits8 engines are a cap on the depth, the larger shift applies; the engine's own reduction stays a ROUNDING, only the Bit Depth parameter / FX truncates). A step FX replaces the instrument's value while the track holds it and acts on the voice
//   sounding now; the Perform layer's OVERDRV / BITDEPTH replace both while a pad holds them (PERFORMANCE LAYER, THE REST OF THE GROUPS); a live edit of the parameter reaches the voices that play the instrument at the next block. In flash (O3OS_AUDIO_COLD: once a span a voice that uses either).
//
//   GATE (OS-ODD11; new Community OS behaviour, the sound is unchanged): a voice on a track whose strip is inaudible (Graph::quiet_tracks: level and both sends closed in the smoothers, so a mute, a solo
//   elsewhere or a fader at 0 with shut sends, never the mute flag) still runs its note (the sample loop, mod_block, the envelopes: its position and amplitude go on) but skips the tremolo, the filter and the
//   shaper, and its block is not fed to the track. The engine passes the mask with set_gate() once a block; it is the previous block's, so a strip opens one block late (the voice is not fed that block) and
//   closes up to two blocks late. The cutoff modulator's clock goes on by the span. The block the gate lifts, the voice re-enters through a 32-sample ramp over its output (`dk`, kAttackSamples) with a
//   cold filter (svf_reset); a new note clears the debt. The gate is empty in an export and for the first live block after it (Engine::render_offline, offline_leave).
//   Cost of the unmuted render (OS-ODD11 UTAIL): a span pays one compare of the gate against a register and one byte load (v.dk) a voice; the gated span and the spans of a re-entry run in odd_span (flash).
//   CLOSED-FORM ADVANCE (OS-ODD11 slice 2): a gated voice of the plain kind (a one shot or a forward loop: mode 0, no residual tail, not a wavetable) does not even run its sample loop. gated_span (flash)
//   walks the same envelope segments as render_voice (render_seg<true>) and replaces the loop by advance(): the position moves by n x inc (a loop wraps with the loop's own modulo of the last sample read, so
//   the lazy wrap leaves the same bits; a one shot that has run out stops in the same span), the amplitude by n x step under the loop's clamp (two clamps: the first step, then the rest). The voice's block is
//   not zeroed or written (`touched_` bit 16 + i marks the visit instead), and `last` is 0 (the voice put nothing out). mod_block, the envelopes, the slide and the cutoff clock go on as for any gated voice, so
//   the state after K gated blocks is the rendered path's, bit for bit (tests/audio_advance_test.cpp, VoicePool::probe). Backward, ping-pong, granular and wavetable voices, and a voice that holds a residual (a retrigger's
//   fade-out), run as in slice 1.
//
//   LIVE PARAMETERS (OS-LIVEP): an instrument parameter edit on the Params screen reaches the sound while the pattern plays (a change of volume, tune, fine tune, panning, cutoff or
//   resonance reaches the sounding voice at once). The edit is a
//   ParamChange of kind Instrument on the engine's ring; the graph's drain offers it to Engine::instrument_hook, which calls apply_param(): the PARAMETER fields of the slot's descriptor
//   are updated in place (apply_instrument_param, the one function app::make_sample_desc builds the same fields with, so a rebuild from the project equals the live-updated descriptor)
//   and the voices playing the slot pick them up at the next block: the gain glides over kDeclickSamples and an enveloped voice ramps across its next
//   span (env_segment), the pitch (tune, fine tune) steps its increment (the phase is continuous), the filter takes the new type, cutoff and resonance (the state is kept: no click in a
//   state variable filter), the cutoff modulator is read live and restarted only when its kind changes, the pan is the graph's one-block ramp (Engine::instrument_hook). The amp
//   envelope's times and sustain are read from the descriptor when the voice enters a stage, so an edit is heard at the voice's next stage. The pan, finetune and amp LFO modulators follow the same way (remod): an edit of a slot's ENABLE starts or ends it on the voices that play it (the others' fields are read
//   live), pan and pitch returning to the instrument's. NOT live: the PCM, the start and end points, the slices and the loop's mode (the sample bank's handover, sample_bank.h); the loop's START and
//   END are live, see LIVE LOOP. NOT live either: the Amiga engine's voices (they read the descriptor at the note); the glide is read when a note starts. Live (OS-IFX1): the overdrive and the bit depth reach the sounding voices.
//
//   LIVE LFO FIELDS (OS-LFOLIVE; the first slice of the live-parameter path of OS-MODMX, docs/specs/OS-LFOLIVE.md): a modulation slot's SOURCE (Off / Envelope / LFO: the enable and loop bytes), the LFO's SHAPE,
//   SPEED (the rate index) and AMOUNT are ParamChanges of kind Instrument with inst_param::lfo(slot, field) (the model's mod ids, kModFirst 0x60), applied by apply_instrument_param (the descriptor, the same
//   function make_sample_desc is held equal to) and lfo_live (the voices). THE PROTOCOL: the render context is the only writer of the descriptor while a voice sounds (it applies the ring's entries in the graph's pass, after the block's
//   voices, at most 64 a block), the main loop writes a descriptor only at the handover (no voice, ring empty), as for the loop points; nothing the render waits on, no allocation, no per-block work for
//   an edit that did not come. What an edit costs, once per ParamChange: SHAPE and AMOUNT one descriptor field (the evaluation reads them: the voice has them at its next one, the cutoff's within 16 samples, the
//   others' at the next block); SPEED the same and, for each voice that plays the slot, the period of its LFO and its reciprocal made again (one division; no derive of the descriptor; the phase is the transport's,
//   so the LFO is where the new period puts it: no continuity is kept across a rate change); SOURCE the voices' start or end of the modulator as an ENABLE edit does. The descriptor has ONE amount for the slot's two
//   generators: switching to the LFO makes it the LFO's (lfo_amount), and the sender sends an EnvAmount after a switch to Envelope. The Filter LFO FX keeps the cutoff's rate while the track holds it.
//
//   LIVE LOOP (OS-MOD): changing an instrument's loop start or end while it plays moves them on the sounding voices.
//   THE PROTOCOL (single writer): the descriptor's loop_start / loop_end are written by exactly ONE context at a time. Playing, only the RENDER writes them, from a ParamChange of kind Instrument with
//   field inst_param::LoopStart or LoopEnd (value: the point in samples from the descriptor's `data`, sent by the app when a Wave edit moves a loop inside the same window and mode, og_samples.h
//   live_loop_moves); apply_loop() holds the point to [0, length] and the loop to 2 samples or more (the other point stays; the sender orders the two so that each is valid), writes the descriptor and
//   the voices that loop over it (end, loop_len), all in the render context. The sample bank's handover (the main loop, sample_bank.h THE HANDOVER) stays the ONLY path for the PCM, the window
//   (start, end), the slices and the loop mode: it waits for the ring to be empty, so no ParamChange that names the old descriptor is still to come, and it rebuilds the loop from the project, which is
//   what the live moves said (same function of the same points), so the two agree and a dropped ring entry is healed when playback stops. The two never write at once: the main loop writes only
//   while the voices are silent and the ring is empty. A voice takes the moved loop at the next block (the drain is after the voices). A voice whose position is past the
//   new end: FORWARD wraps by itself (the sample loop's wrap, loop_start + (position - loop_end) for an overshoot under a
//   loop length; farther it wraps modulo the loop); BACKWARD and PING-PONG voices are put at the new end (just inside it) and turn there. A position under
//   the new start plays on to the end and loops from the new start. Clamping a point that crosses the other is this OS's own rule.
//
// Contexts. Everything but set_sample() and set_law() runs in the render context. set_sample() is the main loop's: a slot is filled at boot (the demo kit) or by a
// loader while no note of it can start; a slot is never rewritten under a playing voice (a loader must silence the voices first, a later slice's command).
//
// Real time: no heap, no division in a sample loop (one UDIV at a loop wrap), no libm, bounded loops; every hot function is O3OS_AUDIO_HOT (SRAM_L), none
// calls into flash.

#include <o3os/audio/events.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/pitch_ramp.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/svf.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace o3os::audio {

inline constexpr unsigned kMaxVoices = 12;   // a pattern's tracks (app::kTracks): the sequencer alone never steals; a live note over 12 sounding tracks steals the oldest. OS-VOX had 16
                                             // (one per graph track); 0.4.3's diag cpu put 8 unfiltered voices at ~49% of the block, so 16 filtered ones would come near ~90% (OS-STATUS)
inline constexpr unsigned kSampleSlots = 64;   // the model's kMaxInstruments (audio_model_ids_test holds them equal)
// OS-FU2 (Sample load audition): a 65th descriptor that no instrument owns, `instrument` 65 of a live note. The app binds the first seconds of the WAV under the cursor to it
// (app::SampleBank::preview_bind) and plays it through the live path; a pattern cannot name it (the model has 64 instruments), so only a live note ever reads it.
inline constexpr unsigned kPreviewSlot = kSampleSlots;
inline constexpr unsigned kDescSlots = kSampleSlots + 1;
inline constexpr unsigned kVoiceTracks = 16;   // the graph's kTracks
inline constexpr std::uint8_t kRootNote = 60;  // plays a sample at its own pitch

inline constexpr std::uint32_t kAttackSamples = 32;     // 0.7 ms
inline constexpr std::uint32_t kReleaseSamples = 256;   // NoteOff: 5.8 ms
inline constexpr std::uint32_t kFadeSamples = 13230;    // NoteFade: 300 ms at 44.1 kHz
inline constexpr std::uint32_t kDeclickSamples = 64;    // the residual of an abrupt stop fades over one block
inline constexpr std::uint32_t kEnvOne = 1u << 30;      // the amp envelope's unity, Q30
inline constexpr std::uint32_t kEnvGroup = 8;           // the envelope's stages are counted in groups of 8 samples
inline constexpr std::uint32_t kFadeEnvUnits = static_cast<std::uint32_t>((core::samples_in_ms(300) + 7u) >> 3);  // 300 ms in groups: 1655 at 44,117.647 Hz, 1654 at 44,100
inline constexpr std::uint32_t kFilterCtl = 16;         // the filter modulation's control period in samples (a divisor of the block, aligned to its start)
inline constexpr std::uint8_t kLfoShapeSine = 5;        // OS-LFOSINE: the LFO shapes are 0 reverse saw, 1 saw, 2 triangle, 3 square, 4 random, 5 sine; a larger one is a triangle (sanitize_mod)
inline constexpr std::uint8_t kLfoShapeCount = 6;
inline constexpr unsigned kLfoRates = 29;               // the tempo-sync rates of the cutoff LFO
inline constexpr unsigned kAmpLfoRates = 24;            // OS-MOD: the amp LFO's table is the last 24 of the 29: its rate r is kLfoRate[r + kAmpRateBase]
inline constexpr unsigned kAmpRateBase = kLfoRates - kAmpLfoRates;
// The tempo-sync rates in steps: the period of the LFO is floor(576 x rate) ticks.
inline constexpr float kLfoRate[kLfoRates] = {128.0f, 96.0f, 64.0f, 48.0f, 32.0f, 24.0f, 16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f, 1.0f, 0.75f, 0.5f, 0.375f, 0.333333f,
                                              0.25f, 0.1875f, 0.166667f, 0.125f, 0.083333f, 0.0625f, 0.041667f, 0.03125f, 0.020833f, 0.015625f};

// The 12 semitones 2^(k/12) in Q30 (1.0 .. 1.89, which fits 32 bits), rounded.
inline constexpr std::uint32_t kSemitoneQ30[12] = {1073741824u, 1137589835u, 1205234447u, 1276901417u, 1352829926u, 1433273380u,
                                                   1518500250u, 1608794974u, 1704458901u, 1805811301u, 1913190429u, 2026954652u};

// The position increment per output sample, Q32.32, of `note` (0..127) against kRootNote: 2^((note - 60) / 12), to better than 1e-8 (the Q30 table, and the bits the lowest octave's shift drops).
O3OS_AUDIO_INLINE constexpr std::uint64_t pitch_inc_q32(unsigned note) noexcept {
  const int d = static_cast<int>(note > 127u ? 127u : note) - static_cast<int>(kRootNote);
  const int octave = d >= 0 ? d / 12 : -((11 - d) / 12);  // floor(d / 12)
  const unsigned semitone = static_cast<unsigned>(d - octave * 12);
  const std::uint64_t base = static_cast<std::uint64_t>(kSemitoneQ30[semitone]) << 2;
  return octave >= 0 ? base << octave : base >> (-octave);
}

// The position increment of `note` as the instrument plays it (OS-007f): `tune` semitones (-24..24) shift the note index,
// clamped into the table; `fine` (-100..100, clamped) moves the pitch that fraction of the way to the neighbouring semitone, up for a positive value, down for
// a negative one, linearly in the pitch (not in cents), and not at all past either end of the table. The fraction is |fine| * 5243 / 2^19
// (0.0100002 a step: no division, so it may run in the render context; the product stays under 2^53).
O3OS_AUDIO_INLINE constexpr std::uint64_t tuned_inc_q32(unsigned note, int tune, int fine) noexcept {
  int n = static_cast<int>(note > 127u ? 127u : note) + tune;
  n = n < 0 ? 0 : (n > 127 ? 127 : n);
  const std::uint64_t base = pitch_inc_q32(static_cast<unsigned>(n));
  if (fine == 0) {
    return base;
  }
  const bool up = fine > 0;
  if (up ? n >= 127 : n <= 0) {
    return base;
  }
  const unsigned f = static_cast<unsigned>(up ? (fine > 100 ? 100 : fine) : (fine < -100 ? 100 : -fine));
  const std::uint64_t other = pitch_inc_q32(static_cast<unsigned>(up ? n + 1 : n - 1));
  const std::uint64_t part = ((up ? other - base : base - other) * static_cast<std::uint64_t>(f * 5243u)) >> 19;
  return up ? base + part : base - part;
}

// The same with the Slide FX's offset on top (OS-PFX, SLIDE FX): `slide_q8` is the offset in 1/4096 of a semitone (sixteenths x 256, signed). The fine tune is read in hundredths of a
// semitone (the linear law, as above; 655 in 1/65536 of a semitone for a hundredth, 0.05 % off at the full 100), the three add, and the pitch is the note index plus the whole
// semitones with the rest as the fraction toward the neighbouring semitone: linear between the two increments, as tuned_inc_q32's, and held at the ends of the table. A voice
// with no slide uses tuned_inc_q32 itself, so that it is bit for bit what it was.
O3OS_AUDIO_INLINE constexpr std::uint64_t slid_inc_q32(unsigned note, int tune, int fine, std::int32_t slide_q8) noexcept {
  const std::int32_t q16 = (fine > 100 ? 100 : (fine < -100 ? -100 : fine)) * 655 + slide_q8 * 16;  // 1/65536 semitone; |slide_q8| < 2^20 (a glide of 127 semitones and the Perform TUNE's 48 are 175 x 4096)
  const std::int32_t whole = q16 >= 0 ? q16 / 65536 : -((65535 - q16) / 65536);                         // floor(q16 / 65536)
  const std::uint64_t frac = static_cast<std::uint64_t>(q16 - whole * 65536);                          // 0..65535
  int n = static_cast<int>(note > 127u ? 127u : note) + tune + whole;
  if (n < 0) {
    return pitch_inc_q32(0);  // below the table: held at its first note
  }
  if (n >= 127) {
    return pitch_inc_q32(127);  // at its last: held (the fraction would go past it)
  }
  const std::uint64_t base = pitch_inc_q32(static_cast<unsigned>(n));
  return base + (((pitch_inc_q32(static_cast<unsigned>(n) + 1u) - base) * frac) >> 16);
}

// OS-RATEPITCH: the sample's own rate against the engine's 44,100 Hz. A sample recorded at R Hz holds R frames a second, so at the root note it must advance R / 44100 frames per output frame
// to sound at the pitch it was made at. The factor is made once, when the descriptor is bound (rate_factor_q28, main loop), is Q28 (R / 44100 x 2^28, under 2^32 up to 384 kHz), and enters
// the increment where a voice makes it (note_on, repitch: control rate, never in the per-sample loops). 0 is exactly 1.0: a sample of 44,100 Hz, or one that records no rate, takes the
// increment untouched, bit for bit what it was. The wavetable pitch law is a window's, not a rate's, and does not take it (the loader leaves it 0 there).
inline constexpr std::uint32_t kEngineRateHz = 44100;
inline constexpr std::uint32_t kRateMinHz = 1000;    // the factor holds the rate to this range; a rate under it would stop a voice, one over it overflow the Q28
inline constexpr std::uint32_t kRateMaxHz = 384000;
constexpr std::uint32_t rate_factor_q28(std::uint32_t hz) noexcept {
  if (hz == 0u || hz == kEngineRateHz) {
    return 0u;
  }
  const std::uint64_t r = hz < kRateMinHz ? kRateMinHz : (hz > kRateMaxHz ? kRateMaxHz : hz);
  return static_cast<std::uint32_t>(((r << 28) + kEngineRateHz / 2u) / kEngineRateHz);
}
// The rate in Hz that rate_factor_q28 was made from (rounded: the Q28 step is 0.00016 Hz, so a rate in whole Hz comes back exactly); 0 for the factor 0, "no rate recorded or 44,100 Hz".
constexpr std::uint32_t rate_hz_of_q28(std::uint32_t f) noexcept { return static_cast<std::uint32_t>(((static_cast<std::uint64_t>(f) * kEngineRateHz) + (1u << 27)) >> 28); }
// `inc` (Q32.32, under 2^40) times the factor `f` (rate_factor_q28; 0 leaves it): the product in two halves of `inc` so that it needs no more than 64 bits; for f = 2^28 it is exact.
O3OS_AUDIO_INLINE constexpr std::uint64_t rate_scale_q32(std::uint64_t inc, std::uint32_t f) noexcept {
  if (f == 0u) {
    return inc;
  }
  return (((inc >> 20) * f) >> 8) + (((inc & 0xFFFFFu) * f) >> 28);
}

// The note a beat slice instrument's first slice sits on: the pad board's root note, 24 in every project and fixture (session state, og.md
// section 3), so a project's slices play from the pads' lowest note.
inline constexpr std::uint8_t kSliceRootNote = 24;
inline constexpr unsigned kMaxSlices = 48;  // the instrument's slice table (model::kSlices)

// The wavetable pitch law: the note i of a window of NNNN frames is the note table's i x NNNN x kWtPerFrame, kWtPerFrame = 523.2511306 / 44100 (C-5 over the rate), here in Q32 (round(K x 2^32):
// 1e-9 off). `inc` is a Q32.32 increment of the plain table (tuned_inc_q32, under 2^37); the product is made in two halves of `inc` so that it needs no more than
// 64 bits and loses under two units of 2^-32 sample.
inline constexpr std::uint32_t kWtPerFrameQ32 = 50960238u;
O3OS_AUDIO_INLINE constexpr std::uint64_t wavetable_inc_q32(std::uint64_t inc, unsigned window) noexcept {
  const std::uint64_t f = static_cast<std::uint64_t>(window) * kWtPerFrameQ32;  // under 2^37 for a window of 2048
  return (((inc >> 20) * f) >> 12) + (((inc & 0xFFFFFu) * f) >> 32);
}

// The amp envelope of an instrument (slot 0), as the loader computes it from the stored milliseconds (app::make_sample_desc). The times are in groups of 8
// samples, 1..65535 each; `sustain` is Q30 0..kEnvOne, `amount` a Q16 gain 0..65536.
struct AmpEnvelope {
  std::uint16_t delay = 1;
  std::uint16_t attack = 1;
  std::uint16_t hold = 1;
  std::uint16_t decay = 1;
  std::uint16_t release = 1;
  bool enable = false;  // false: the plain gain, and the other fields are not read
  bool lfo = false;     // OS-LIVEP: the stored slot is an LFO (the instrument's loop byte): enable stays false whatever a live edit says (a flag of the instrument, not a parameter)
  std::uint32_t sustain = kEnvOne;
  std::int32_t amount = kernel::kUnity;
};

// The cutoff modulator of an instrument (slot 2), as the loader gives it from the stored fields (app::make_sample_desc); set_sample() holds them to range. One of an ADSR
// (`kind` kFmodEnv: amount -1..1, sustain 0..1, the five times in ms) or an LFO (kFmodLfo: shape 0 reverse saw, 1 saw, 2 triangle, 3 square, 4 random, 5 sine (OS-LFOSINE: (1 - cos 2 pi u) / 2, a quintic at control rate), a larger one is a triangle;
// rate 0..28 the index of the tempo-sync rate, a larger one is 28; amount -1..1). The other fields are not read.
inline constexpr std::uint8_t kFmodNone = 0;
inline constexpr std::uint8_t kFmodEnv = 1;
inline constexpr std::uint8_t kFmodLfo = 2;
struct FilterMod {
  std::uint8_t kind = kFmodNone;
  std::uint8_t shape = 2;
  std::uint8_t rate = 0;
  bool lfo = false;  // OS-LIVEP: the instrument's slot 2 is an LFO (loop byte), so enabling it makes `kind` kFmodLfo and its amount is the LFO's (not a parameter: a flag of the instrument)
  float amount = 1.0f;
  float sustain = 1.0f;
  // OS-FU2 (the Filter LFO FX 35): what the step FX plays when the instrument's own modulator is not an LFO. `shape` (above) is then the instrument's lfo[2] shape and `lfo_amount`
  // its lfo[2] amount, whatever `kind` is (the FX builds the LFO from the instrument's shape and amount with the FX's rate). For
  // `kind` kFmodLfo set_sample() makes it `amount`.
  float lfo_amount = 1.0f;
  std::uint16_t delay = 0;
  std::uint16_t attack = 0;
  std::uint16_t hold = 0;
  std::uint16_t decay = 0;
  std::uint16_t release = 0;
};

// The instrument's filter as the loader gives it (app::make_sample_desc), in the .pti's units; set_sample() holds them to range. `type` is kFilterLow / kFilterHigh /
// kFilterBand (a larger one switches the filter off), `cutoff` 0..1, `resonance` 0..4.3 as stored (the Q is resonance + 0.7, svf.h).
struct FilterDesc {
  bool enable = false;  // the filter is in the path: `wanted` and a known type
  std::uint8_t type = kFilterLow;
  bool wanted = false;  // OS-LIVEP: the instrument's own enable switch; set_sample() turns `enable` off for a type it does not know, and a live edit of the type must find the switch again
  float cutoff = 0.0f;
  float resonance = 0.0f;
  FilterMod mod{};  // OS-007m
};

// The playback engine of an instrument (OS-005h; radar card `amiga-playback-mode`). Modern is the engine of this header. Bits12 and Bits8 are the Modern engine with the
// voice's output cut to 12 or 8 bits (VoicePool::shape_span): bit reduction only (a rounding), a cap on the instrument's BIT DEPTH (PAR-051; OS-IFX1: the lower of the two applies, and the BIT DEPTH's own reduction truncates). Amiga plays the sample as Paula does
// (audio/amiga_voices.h: integer period, sample-and-hold, BLEP, the MED tick processor) and never reaches this pool. The engine is chosen by the app (an import sets it) and, since OS-004d, stored in the
// project (model::Instrument::engine, INST 1.2; 0 there = automatic, which is what an older project is).
inline constexpr std::uint8_t kEngineModern = 0;
inline constexpr std::uint8_t kEngineAmiga = 1;
inline constexpr std::uint8_t kEngineBits12 = 2;
inline constexpr std::uint8_t kEngineBits8 = 3;

// OS-IFX1 (OVERDRIVE, BIT DEPTH): the curves as pure functions, which the host tests hold to their bounds, and which VoicePool::shape_span runs.
// The overdrive's drive gain in Q11 (2048 = unity .. 32768 = 16, i.e. 1 + 15 (value / 100)^2) of a value 0..100; 0 for value 0 (no overdrive: the voice skips the pass).
constexpr std::uint16_t overdrive_gain_q11(unsigned value) noexcept {
  const unsigned v = value > 100u ? 100u : value;
  return v == 0u ? std::uint16_t{0} : static_cast<std::uint16_t>(2048u + 30720u * v * v / 10000u);
}
// One sample through the overdrive: u = x g / 2 held to 16 bits (Q15, so the clip is at |x g| = 2.0 of full scale), then y = u (2 - |u|) = x g - x g |x g| / 4. Odd, monotonic, slope g at 0,
// exactly full scale at the clip, no overflow (|u| <= 32768, so u * (65536 - |u|) < 2^31). `g` is overdrive_gain_q11.
constexpr std::int32_t overdrive_sample(std::int32_t x, std::int32_t g) noexcept {
  const std::int32_t w = (x * g) >> 12;
  const std::int32_t u = w > 32767 ? 32767 : (w < -32768 ? -32768 : w);
  return (u * (65536 - (u < 0 ? -u : u))) >> 15;
}
// One sample through the bit depth: the low `shift` bits cleared (shift = 16 - bits, 1..15), a floor to a multiple of 2^shift.
constexpr std::int32_t bit_depth_sample(std::int32_t x, unsigned shift) noexcept { return x & ~((std::int32_t{1} << shift) - 1); }

// A sample the voices read. `data` is `length` int16 samples, mono, in memory that outlives every note of it (the sample arena).
struct SampleDesc {
  const std::int16_t* data = nullptr;
  std::uint32_t length = 0;       // samples; 0: the slot is empty and a note on it is ignored (counted)
  std::uint32_t loop_start = 0;   // a forward loop over [loop_start, loop_end) when loop_end - loop_start >= 2 and loop_end <= length
  std::uint32_t loop_end = 0;
  std::int32_t gain = kernel::kUnity;  // the instrument's own gain, Q16 in [0, kernel::kMaxGain] (65536 = 0 dB; a NoteOn that names no velocity)
  std::int8_t tune = 0;           // semitones, the instrument's tune (-24..24)
  std::int8_t fine_tune = 0;      // the instrument's fine tune (-100..100), see tuned_inc_q32
  std::uint32_t rate_q28 = 0;      // OS-RATEPITCH: the sample's rate over the engine's (rate_factor_q28), 0 = unity; scales the increment wherever a voice makes it
  // OS-007h: the play modes that are not "a window at the note's pitch".
  bool beat_slice = false;        // beat slice: the pitch is the root's whatever the note; with slice_count != 0 the note picks the slice
  std::uint8_t slice_count = 0;   // 1..kMaxSlices, with `slices`; 0: the whole of the sample is the one slice
  std::uint16_t wt_size = 0;      // a wavetable's window length (32..2048): the pitch law is the wavetable's, and the loop is one window; 0: an ordinary sample
  // OS-SENDFX: the instrument's DELAY SEND and REVERB SEND, 0..100: what the Engine gives the note's track where the note starts (Graph::set_voice_sends), as
  // the pan. They sit in the two bytes the layout had spare before `slices`.
  std::uint8_t delay_send = 0;
  std::uint8_t reverb_send = 0;
  // The slice boundaries: slice_count + 1 offsets in samples from `data`, ascending, the last the length; each as two u16 (low half, high half) so that
  // the table can sit in the 16-bit sample arena after its PCM. Read only in note_on().
  const std::uint16_t* slices = nullptr;
  std::uint32_t offset = 0;       // samples of the instrument's PCM before `data`, for `diag inst` (the window's start); the voices do not read it
  // OS-007k. The loop's direction when there is a loop: 0 forward (play mode 1), 1 backward (2), 2 ping-pong (3).
  std::uint8_t loop_mode = 0;
  std::uint8_t pan = 50;          // 0..100, 50 centre: what the Engine gives the note's track (see PAN)
  std::uint8_t engine = kEngineModern;  // OS-005h: the instrument's playback engine
  std::uint8_t amiga_vol = 64;    // OS-005h, Amiga engine: the instrument's own volume 0..64 (a note that names no velocity): its gain read against unity, min(64, round(64 x gain / 65536)), set with the gain
  AmpEnvelope env{};
  FilterDesc filter{};  // OS-007l
  // OS-MOD: the other three modulators of the instrument, each a FilterMod (kind, shape, rate, amount, the ADSR's fields). pan_mod (slot 1) moves the pan, tune_mod (slot 5) the fine tune, each
  // by +-100 of its own unit at amount 1; amp_mod is the tremolo: the amp slot (0) when it is an LFO (the loop byte), kind kFmodLfo or none (an amp ADSR is `env`), its rate the AMP table's
  // 0..23 (held to range by set_sample). See PAN, FINETUNE and AMP LFO MODULATION.
  FilterMod pan_mod{};
  FilterMod tune_mod{};
  FilterMod amp_mod{};
  // OS-GRAN, GRANULAR (play mode 7): `data` / `length` are the WHOLE sample, loop_mode is the grain type (0 forward, 1 backward, 2 ping-pong), and these say the grain (voices.h GRANULAR).
  bool granular = false;
  std::uint8_t gr_shape = 0;      // 0 square, 1 triangle, 2 gauss (the instrument's granular shape)
  std::uint16_t gr_len = 441;     // the grain's length in samples (44..44100)
  std::uint16_t gr_pos = 0;       // the grain's position 0..65535 of the sample
  std::uint16_t glide = 0;        // OS-IFX1 (fills the padding after gr_pos): the instrument's GLIDE in ms, 0..15000, 0 = none (see GLIDE)
  // OS-GRAN, WAVETABLE (wt_size != 0): the stack has `wt_windows` windows of wt_size frames and `data` is window `wt_pos` of it (the loader's pick), so that the window can move (POSITION
  // MODULATION, and the live window, inst_param::WtWindow). wt_windows 0: not said, the window is fixed.
  std::uint32_t wt_windows = 0;
  std::uint32_t wt_pos = 0;
  // OS-GRAN, POSITION MODULATION: the instrument's slot 3 (a wavetable with more than one window) or slot 4 (a granular instrument): the same descriptor as the other modulators; slot 3 runs
  // for a wavetable instrument only and slot 4 for a granular one only, so one serves both (set_sample drops it for any other).
  FilterMod pos_mod{};
  // OS-IFX1: the instrument's OVERDRIVE 0..100 (0 = none) and BIT DEPTH 4..16 (16 = none) (see OVERDRIVE and BIT DEPTH).
  std::uint8_t overdrive = 0;
  std::uint8_t bit_depth = 16;
};

// The Slice FX of a track that has none (VoicePool::forced_slice).
inline constexpr std::uint8_t kNoForcedSlice = 0xFF;

// Boundary `i` of a slice table: two u16 halves, low then high (the table lives in the 16-bit sample arena).
O3OS_AUDIO_INLINE std::uint32_t slice_bound(const std::uint16_t* table, unsigned i) noexcept {
  return static_cast<std::uint32_t>(table[2u * i]) | (static_cast<std::uint32_t>(table[2u * i + 1u]) << 16);
}

// The part of a BEAT SLICE sample that a note plays (OS-SLICEFIX: the one rule of both engines, the sampler's voices and the Amiga voices). `from` and `length` are in samples from `s.data`.
// The slice is the Slice FX's `forced` value modulo the slice count when the track has one, else
// note - kSliceRootNote (24, the pad board's lowest note) clamped to the instrument's slices. A sample with no slice table (slice_count 0) is one slice, all of it. False: the slice has no
// length or starts past the end of the sample, and plays nothing. The boundaries come from the arena: clamped, so that no read leaves [0, length) whatever they hold; an end before the start
// runs to the end of the sample (this OS's rule). The pitch of a beat slice is not the note's: the voices play it at the root's (kRootNote, the instrument's tune and fine tune apply).
O3OS_AUDIO_INLINE bool beat_slice_span(const SampleDesc& s, unsigned note, unsigned forced, std::uint32_t& from, std::uint32_t& length) noexcept {
  from = 0;
  length = s.length;
  if (s.slice_count == 0u) {
    return true;
  }
  unsigned k;
  if (forced != kNoForcedSlice) {
    k = forced % s.slice_count;
  } else {
    k = note > kSliceRootNote ? note - kSliceRootNote : 0u;
    k = k >= s.slice_count ? s.slice_count - 1u : k;
  }
  const std::uint32_t a = slice_bound(s.slices, k);
  const std::uint32_t b = slice_bound(s.slices, k + 1u);
  const std::uint32_t last = b < a ? s.length : (b > s.length ? s.length : b);
  if (a >= last) {
    return false;
  }
  from = a;
  length = last - a;
  return true;
}

// OS-LIVEP: the instrument parameters the descriptor holds. The ids are model::instrument_param's (the layers: audio does not include the model; tests/audio_model_ids_test.cpp holds
// them equal). An envelope e (0..5) field f is kEnvelopeFirst + e x kEnvelopeStride + f; the descriptor has the amp envelope and tremolo (e 0), the pan (e 1), cutoff (e 2), wavetable position (e 3, a wavetable's), granular position (e 4, a granular instrument's) and finetune (e 5) modulators.
namespace inst_param {
inline constexpr unsigned Volume = 0;
inline constexpr unsigned Panning = 1;
inline constexpr unsigned Tune = 2;
inline constexpr unsigned FineTune = 3;
inline constexpr unsigned Glide = 4;
inline constexpr unsigned FilterEnable = 5;
inline constexpr unsigned FilterType = 6;
inline constexpr unsigned Cutoff = 7;
inline constexpr unsigned Resonance = 8;
inline constexpr unsigned DelaySend = 9;
inline constexpr unsigned ReverbSend = 10;
inline constexpr unsigned Overdrive = 11;
inline constexpr unsigned BitDepth = 12;
inline constexpr unsigned PlayEngine = 13;  // OS-FU5: the playback engine; not a descriptor field (its edit rebinds the slot through the engine action)
inline constexpr unsigned kBase = 14;
// OS-MOD: the LIVE LOOP POINTS (voices.h LIVE LOOP). Not parameters of the model's tables (the loop points are the instrument's start / loop / end, 0..65535 fractions, and the Wave screen
// edits them): a ParamChange of kind Instrument with these ids carries the descriptor's loop start or end IN SAMPLES from the descriptor's `data` (a whole number, exact in a float up to
// 2^24), sent by the app (og_samples.h live_loop_moves) only when the instrument's window and mode are what the descriptor was built with.
inline constexpr unsigned LoopStart = 0x10;
inline constexpr unsigned LoopEnd = 0x11;
// OS-GRAN: the instrument's wavetable window (0..windows-1) and granular position (0..65535) as a live edit, handled like the loop points (the render writes the descriptor and moves the voices; value a
// whole number as a float): they are not parameters of the model's tables either (the Wave screen or a position FX would send them).
inline constexpr unsigned WtWindow = 0x12;
inline constexpr unsigned GranPos = 0x13;
inline constexpr unsigned kEnvelopeFirst = 0x20;
inline constexpr unsigned kEnvelopeStride = 8;
inline constexpr unsigned EnvEnable = 0;
inline constexpr unsigned EnvAmount = 1;
inline constexpr unsigned EnvAttack = 2;
inline constexpr unsigned EnvDecay = 3;
inline constexpr unsigned EnvSustain = 4;
inline constexpr unsigned EnvRelease = 5;
inline constexpr unsigned kEnvelopeFields = 6;
inline constexpr unsigned kEnvelopes = 6;
constexpr unsigned envelope(unsigned e, unsigned f) noexcept { return kEnvelopeFirst + e * kEnvelopeStride + f; }
// OS-LFOLIVE: the modulation slots' SOURCE and LFO fields as live edits (model::instrument_param::mod, kModFirst 0x60: audio_model_ids_test holds them equal). Slot e field f is kLfoFirst + e x kLfoStride + f:
// LfoSource 0 Off / 1 Envelope / 2 LFO (the loop byte and the enable), LfoShape 0..5, LfoSpeed (the tempo-sync rate index: 0..28, the amp slot's 0..23) and LfoAmount (-1..1).
inline constexpr unsigned kLfoFirst = 0x60;
inline constexpr unsigned kLfoStride = 4;
inline constexpr unsigned LfoSource = 0;
inline constexpr unsigned LfoShape = 1;
inline constexpr unsigned LfoSpeed = 2;
inline constexpr unsigned LfoAmount = 3;
inline constexpr unsigned kLfoFields = 4;
constexpr unsigned lfo(unsigned e, unsigned f) noexcept { return kLfoFirst + e * kLfoStride + f; }
constexpr bool is_lfo_field(unsigned field) noexcept { return field >= kLfoFirst && field < kLfoFirst + kEnvelopes * kLfoStride; }
}  // namespace inst_param

// What apply_instrument_param did with a field: Unknown (the id names no instrument parameter), Ignored (it is one the descriptor does not hold: the playback engine
// and the position envelope of an instrument it does not belong to: e 3 of what is no wavetable, e 4 of what is no granular instrument), Applied.
enum class InstApply : std::uint8_t { Unknown, Ignored, Applied };

// The one place an instrument parameter becomes a descriptor field, as the stored value `value` (a float whatever the storage: u8, i8, u16, f32 or a flag; held to range as the
// instrument's own tables hold it) says: volume -> gain and amiga_vol through `law`, pan, tune, fine tune, the filter, the amp envelope and the cutoff modulator's fields. Pure; the
// result is what VoicePool::set_sample keeps (the same sanitising), so apply_instrument_param over a descriptor equals the descriptor app::make_sample_desc builds for the instrument
// (which calls this for every one of these fields). The envelope's delay and hold and the cutoff modulator's shape, rate and delay are not parameters and are not touched.
InstApply apply_instrument_param(SampleDesc& d, unsigned field, float value, LevelLaw law) noexcept;
// OS-PERF-LFO (docs/specs/OS-PERFFX.md section 11): the LFO a Perform row plays over a slot that is Off or an envelope when the slot's stored LFO amount cannot be heard (an old project or an OG file stores 0, or a
// negative amount): a triangle at 50 %. A slot that stores an amount of at least kPerfLfoMinAmount plays its own stored shape and amount. Never written into the shared descriptor (OS-LFOLIVE: the rebuild equals the live edits).
inline constexpr float kPerfLfoMinAmount = 0.05f;
constexpr FilterMod make_perf_lfo_mod() noexcept {
  FilterMod m;
  m.kind = kFmodLfo;
  m.shape = 2;
  m.rate = 0;
  m.lfo = true;
  m.amount = 0.5f;
  m.lfo_amount = 0.5f;
  return m;
}
inline constexpr FilterMod kPerfLfoMod = make_perf_lfo_mod();
// The envelope time in groups of 8 samples from milliseconds: at least 1, at most 65535.
std::uint16_t env_units(std::uint16_t ms) noexcept;

// OS-MOD: the running state of ONE modulator of a voice (the ADSR's stage and level, or the LFO's period and phase shift). The cutoff's (OS-007m) and the three OS-MOD adds (pan,
// finetune, amp LFO) are the same generator over their own FilterMod, so they share fenv_eval / flfo_eval / mod_start.
inline constexpr unsigned kMgCut = 0;
inline constexpr unsigned kMgPan = 1;
inline constexpr unsigned kMgTune = 2;
inline constexpr unsigned kMgTrem = 3;
inline constexpr unsigned kMgPos = 4;   // OS-GRAN: the wavetable window's or the granular position's
inline constexpr unsigned kModGens = 5;
struct ModGen {
  std::uint8_t kind = kFmodNone;  // kFmodEnv / kFmodLfo / kFmodNone: what the note started
  std::uint8_t stage = 0;         // ADSR: the phase (VoicePool::FStage)
  bool key = false;               // the ADSR's note is held (false from the release)
  bool need = false;              // evaluate at the next sample, not at the next 16th
  std::uint8_t l_ovr = 0;         // OS-COREFX CF2: the tracker FX's waveform for this LFO, shape + 1 (0: the instrument's own); fits the alignment gap before `since`: ModGen does not grow
  std::uint16_t since = 0;        // ADSR: samples since the last evaluation: the time the modulator has still to take
  float e_t = 0.0f;               // ADSR: samples into the stage
  float e_kill = 0.0f;            // the level the delay holds and the attack starts from
  float e_max = 0.0f;             // the level the release starts from
  float e_out = 0.0f;             // the last output (the next note's kill)
  float l_inv = 1.0f;             // LFO: 1 / period
  float l_rand = 0.0f;            // the random shape's level, 0..A
  std::uint32_t l_period = 1;     // ticks
  std::uint32_t l_shift = 0;      // ticks: 576 x (step % 36) at the note
  std::uint32_t l_q = 0;          // the period the random level was drawn in
};
static_assert(sizeof(ModGen) == 44, "OS-COREFX CF2: l_ovr fits the alignment gap before `since`; ModGen (five a voice) does not grow");

// OS-BUG-SLICEPITCH: the staging cell of one descriptor (main loop writes, the render takes; VoicePool::stage_sample). The platform keeps it in SDRAM (app::SampleBankMem::stage): the voice state
// in SRAM_U holds only a pointer and the state byte. `desc` is sanitised (VoicePool::sanitized); `table` is the slice table of a beat slice descriptor, in the layout of SampleDesc::slices, which the
// render copies to `table_dst` (the slot's table in the sample arena, only ever read by note_on in the render context) in the same step as the descriptor, so a note never reads a half-written table.
struct StageCell {
  SampleDesc desc;
  std::uint16_t table[2u * (kMaxSlices + 1u)];
  std::uint16_t* table_dst = nullptr;
  std::uint16_t table_words = 0;  // 0: no table
  std::uint8_t slot = 0;
};

class VoicePool {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;

  struct Stats {
    std::uint32_t note_ons = 0;
    std::uint32_t steals = 0;     // a playing voice of another track was taken
    std::uint32_t ignored = 0;    // a note on an empty slot or a track outside the pool
    std::uint32_t gated_blocks = 0;  // OS-ODD11 (test hook only, not in `diag voices`): blocks in which a voice ran gated (post stages skipped, its block not fed)
    std::uint32_t advanced_spans = 0;  // OS-ODD11 slice 2: voice spans taken by the closed-form advance (gated_span) and not rendered. `diag voices` shows it (voice_snapshot(): the render is the only writer, gated_span, one aligned word)
  };

  VoicePool() noexcept;
  VoicePool(const VoicePool&) = delete;
  VoicePool& operator=(const VoicePool&) = delete;

  // ---- Main loop (see "Contexts") -----------------------------------------------------------------------------------------
  // Slot 0..kSampleSlots-1 (kPreviewSlot too: the audition's). A loop that does not fit the sample, or is under 2 samples, is dropped (one shot); a sample of no length empties the slot. A
  // slice table that is not there, or a count past kMaxSlices, is dropped (the whole sample is one slice); a wavetable with no loop is an ordinary sample.
  void set_sample(unsigned slot, const SampleDesc& desc) noexcept;
  // The descriptor set_sample() keeps for `desc` (the same sanitising).
  static SampleDesc sanitized(const SampleDesc& desc) noexcept;
  // ---- Staged descriptor (OS-BUG-SLICEPITCH) ----
  // set_sample() may be called only while no voice can read the slot (the handover waits for silence). A note started while the pads or the transport keep the engine busy would play the
  // old descriptor for seconds, so the main loop can instead STAGE one: it fills the cell, then publishes it (state 1, release); the render takes it at the start of its next block
  // (begin_block, before the live and sequenced notes of the block): the slot's descriptor is replaced, and the slot's sounding voices end with a declick when their audio, length, granular
  // or wavetable state or engine changed (a slice, loop or direction edit leaves a ringing note alone: it has cached its own span). The render is the only writer of samples_ while it runs
  // (as apply_param and apply_loop are); a ParamChange pushed after the stage is drained after the take, in the same or a later block. state: 0 free, 1 staged, 2 being taken (render only).
  // `cell` lives in memory that outlives the pool. Main loop.
  void bind_stage(StageCell* cell) noexcept { stage_ = cell; }
  // True when the cell was free: it now holds `desc` (sanitised) for `slot`, `table_words` words of `table` going to `table_dst`. Main loop.
  bool stage_sample(unsigned slot, const SampleDesc& desc, const std::uint16_t* table, unsigned table_words, std::uint16_t* table_dst) noexcept;
  bool stage_busy() const noexcept { return stage_state_.load(std::memory_order_acquire) != 0u; }
  // How many descriptors the render has taken (it is the only writer): the main loop compares it with the value it saw when it staged, to tell a take from a cancel.
  std::uint32_t stage_takes() const noexcept { return stage_takes_.load(std::memory_order_acquire); }
  // Takes a staged cell back (state 1 to 0). False when the render is taking it right now (never seen on one core: the take runs to completion above the main loop).
  bool stage_cancel() noexcept;
  const SampleDesc& sample(unsigned slot) const noexcept { return samples_[slot < kDescSlots ? slot : 0u]; }
  // The law the instrument volume goes through (OS-LIVEP: the live volume edit makes its gain with it, as app::make_sample_desc did for the descriptor). Main loop, with the project's
  // load: a word the render reads at a ParamChange.
  void set_law(LevelLaw law) noexcept { law_ = law; }
  LevelLaw law() const noexcept { return law_; }
  // Render context, from Engine::instrument_hook (see LIVE PARAMETERS): the parameter `field` of instrument `slot` is `value`. The descriptor of a slot that has audio takes it (the voices
  // that play the slot pick it up); an empty slot only has the field checked. False: the field or the slot names nothing, or the value is not a number (the engine counts it rejected).
  // `pan_tracks` is the tracks whose playing voice has the slot's pan to give (bit t = track t) when `field` is the pan, else 0: the caller moves the graph's pan. In flash: per
  // ParamChange, at most kMaxParamsPerBlock a block, never per sample; at most kMaxVoices voices are visited. The main loop writes the descriptor only when the engine is quiet
  // (SampleBank's handover), and what it writes then is what the project says, which is what every ParamChange it may meet in the ring says too.
  bool apply_param(unsigned slot, unsigned field, float value, std::uint32_t& pan_tracks) noexcept;
  // OS-SENDFX, render context, in flash (per live edit of a send): the tracks whose voice plays `slot`, bit t = track t.
  std::uint32_t slot_tracks(unsigned slot) const noexcept;
  bool has_samples() const noexcept {
    for (unsigned i = 0; i < kSampleSlots; ++i) {
      if (samples_[i].length != 0u) {
        return true;
      }
    }
    return false;
  }

  // ---- Render context ------------------------------------------------------------------------------------------------------
  // One block is: begin_block(); then, in time order, render(cursor, event offset) and the event's call; render(cursor, kBlock); end_block(). A voice is
  // rendered into its own block, zeroed the first time it is touched in the block, so the part of the block before a note starts is exactly silent.
  O3OS_AUDIO_INLINE void begin_block() noexcept {
    touched_ = 0;
    gated_ = 0;
    if (stage_state_.load(std::memory_order_acquire) == 1u) {
      take_stage();  // OS-BUG-SLICEPITCH: once per edit, not per block (in flash)
    }
  }
  // OS-ODD11 (the voice gate, see GATE below): the tracks whose strip is inaudible (Graph::quiet_tracks), taken from the next begin_block() on; 0: no voice is gated. Render context, before begin_block().
  O3OS_AUDIO_INLINE void set_gate(std::uint32_t quiet_tracks) noexcept { gate_ = static_cast<std::uint16_t>(quiet_tracks); }
  O3OS_AUDIO_HOT void render(std::uint32_t begin, std::uint32_t end) noexcept;
  O3OS_AUDIO_HOT void end_block() noexcept;

  // `note` 0..127, `slot` 0..63 (64: the audition's), `velocity` 0..127 or kVelocityDefault. Starts at the sample the next render() call starts at. False: nothing started (an
  // empty slot, a track outside the pool, a slice with no length).
  O3OS_AUDIO_HOT bool note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity) noexcept;
  O3OS_AUDIO_HOT void note_off(unsigned track) noexcept;  // release over kReleaseSamples
  O3OS_AUDIO_HOT void fade(unsigned track) noexcept;      // release over kFadeSamples
  O3OS_AUDIO_HOT void cut(unsigned track) noexcept;       // stop at once (the residual fades: no click)
  // OS-WAVEXP: the random LFO shape's generator back to its seed. Engine::offline_quiet calls it at the start of every pass of an export (the render parked), so that an export of an unchanged
  // project is the same file every time; the live render draws from wherever the generator is.
  void seed_random() noexcept { rng_ = kRngSeed; }
  O3OS_AUDIO_HOT void release_all() noexcept;             // every playing voice enters its release (the transport stopped)
  // The Slice FX (see SLICE FX): the track's next notes play slice `value % slice_count` of a beat slice instrument, until cleared. A value is kept as given
  // (0..255): the modulo is the note's, by the instrument it plays.
  O3OS_AUDIO_INLINE void set_forced_slice(unsigned track, unsigned value) noexcept {
    if (track < kVoiceTracks) {
      forced_slice_[track] = static_cast<std::uint8_t>(value > 254u ? 254u : value);
    }
  }
  // The track's Slice FX value (kNoForcedSlice when it has none): the Amiga voices play a beat slice by it too (OS-SLICEFIX).
  O3OS_AUDIO_INLINE unsigned forced_slice(unsigned track) const noexcept { return track < kVoiceTracks ? forced_slice_[track] : static_cast<unsigned>(kNoForcedSlice); }
  O3OS_AUDIO_INLINE void clear_forced_slice(unsigned track) noexcept {
    if (track < kVoiceTracks) {
      forced_slice_[track] = kNoSlice;
    }
  }
  O3OS_AUDIO_INLINE void clear_forced_slices() noexcept {
    for (unsigned t = 0; t < kVoiceTracks; ++t) {
      forced_slice_[t] = kNoSlice;
    }
  }
  // A ModSet / ModEnd of a group the Engine does not name (it is SRAM_L code, and every group it names costs it bytes: a jump table, a compare). The whole event is handed over (OS-PERFFX P0: by
  // reference, so that the layer `c` and its value `value` arrive too; a ModEnd carries neither type nor value). Today the Slide FX's (kModGroupSlide, see SLIDE FX) and OS-IFX1's Glide, Overdrive
  // and Bit depth (kModGroupGlide / Overdrive / BitDepth) are acted on, the others are ignored. For Slide the FX type `a` is kFxSlideUp or kFxSlideDown and `b` the distance in sixteenths of a
  // semitone (0..255): the track's notes slide that far up or down from their pitch, the voice sounding now from where it is. For the three of OS-IFX1 `a` is the FX's own number and `b` the value
  // 0..100 (Glide, Overdrive) or 1..16 (Bit depth): the track's next notes take it (Glide) or the sounding voice too (Overdrive, Bit depth) in place of the instrument's. Any other type (a ModEnd) ends
  // the FX. Every group but Slide goes through layer_event, which also keeps the performance layer's value (PERFORMANCE LAYER, THE REST OF THE GROUPS). In flash (O3OS_AUDIO_COLD: once per FX
  // event). The transport start (FxReset) ends the step FX of every track and leaves the layer.
  O3OS_AUDIO_COLD void fx_event(unsigned track, const SeqEvent& e) noexcept;
  // The scalar form, for tests: a ModSet of the step layer with FX type `fx_type` and value `value` (type 0: the group's end).
  void fx_event(unsigned track, unsigned group, unsigned fx_type, unsigned value) noexcept {
    SeqEvent e{};
    e.kind = EventKind::ModSet;
    e.track = static_cast<std::uint8_t>(track);
    e.a = static_cast<std::uint8_t>(fx_type);
    e.b = static_cast<std::uint8_t>(value);
    e.c = kLayerFx1;
    e.flags = static_cast<std::uint8_t>(group);
    fx_event(track, e);
  }
  // OS-PERFFX P0 (PERFORMANCE LAYER, THE REST OF THE GROUPS): what the voices hold of the performance layer, for the export to put away. kPerfSlots groups (the slot of a group: perf_slot_of) have a value
  // (-127..127, the layer's own int16 held to a byte; the Tune slot -48..48) and a held bit a track (90 bytes since P7's SMP POS and SMP END slots, 54 since P5's Tune slot, 36 more than P5's; the Pos and End slots -100..100; the SRAM_U row of the voices, mem_map.h voice_state, took them from the ADR's slack); the performance filter is the FilterFx's, so its type + 1 (0: none) and offset go along.
  static constexpr unsigned kPerfSlots = 10;  // P7 added the SMP POS and SMP END slots: 90 bytes; OS-PERF-LFO (P4) the five LFO slots: 180 bytes
  static constexpr unsigned kPerfTuneSlot = 2;
  static constexpr unsigned kPerfPosSlot = 3;  // OS-PERFFX P7: -100..100 percent, read by note_on (smp_frames)
  static constexpr unsigned kPerfEndSlot = 4;
  // OS-PERF-LFO (docs/specs/OS-PERFFX.md section 11): slots 5..9 are the Perform LFO rows VOL, PAN, TUNE, FLT and POS LFO (destination d = slot - kPerfLfoSlot): the value is the signed offset on the LFO speed
  // index of the track's LFO for that destination, -28..28. Read by perf_index (flash) when a note starts and when the layer or the instrument changes, never per block.
  static constexpr unsigned kPerfLfoSlot = 5;
  static constexpr unsigned kLfoDests = 5;
  static constexpr int kPerfLfoMax = 28;
  static constexpr unsigned kPerfLfoBase = 14;  // the table index of "1 step" (kLfoRate[14] = 1.0): the speed of a destination whose instrument slot has no LFO, before the offset
  static constexpr int kPerfTuneMax = 48;  // the TUNE slot's range, in semitones (seq/perform.h kPerfFxInfo)
  static constexpr int kPerfPctMax = 100;  // the SMP POS and SMP END slots' range, in percent
  struct PerfSlots {
    std::int8_t val[kVoiceTracks][kPerfSlots];
    std::uint16_t on[kPerfSlots];  // bit t: the layer holds the slot on track t
  };
  static_assert(kVoiceTracks <= 16, "PerfSlots::on is a 16-bit mask, a bit a track");
  struct PerfSave {
    PerfSlots slots;
    std::uint8_t filter_type[kVoiceTracks];
    std::int8_t filter_off[kVoiceTracks];
    std::uint16_t rev;  // OS-PERF-REVERSE: perf_rev_
    std::uint8_t step_lfo[4][kVoiceTracks];  // OS-PERF-LFO-STEP: step_lfo_ (the export puts the step FX away, as it does the layer)
  };
  // The slot of a ModSet group (events.h kModGroup*) that the voices keep the layer's value for; kPerfSlots: none.
  static constexpr unsigned perf_slot_of(unsigned group) noexcept {
    return group == kModGroupOverdrive ? 0u
           : (group == kModGroupBitDepth ? 1u
              : (group == kModGroupTune ? kPerfTuneSlot
                 : (group == kModGroupPosition ? kPerfPosSlot
                    : (group == kModGroupSampleEnd ? kPerfEndSlot
                       : (group >= kModGroupLfoVolume && group <= kModGroupLfoPosition ? kPerfLfoSlot + (group - kModGroupLfoVolume) : kPerfSlots)))));  // the LFO groups 11..15 are the slots 5..9
  }
  // For tests and `diag`: does the layer hold `group` on `track`, and its value.
  bool perf_held(unsigned track, unsigned group) const noexcept {
    if (group == kModGroupReverse) {
      return track < kVoiceTracks && ((perf_rev_ >> track) & 1u) != 0u;  // OS-PERF-REVERSE: a mask of its own, no value (the layer's 1)
    }
    return track < kVoiceTracks && perf_slot_of(group) < kPerfSlots && ((perf_.on[perf_slot_of(group)] >> track) & 1u) != 0u;
  }
  int perf_value(unsigned track, unsigned group) const noexcept { return group == kModGroupReverse ? (perf_held(track, group) ? 1 : 0) : (perf_held(track, group) ? perf_.val[track][perf_slot_of(group)] : 0); }
  // OS-BUG-MODDRIVE: the Overdrive and Bit depth of a MODULE instrument's track (Engine::shape_modules). shape_mask: the tracks that hold either FX, the step FX's or the layer's (a bit a track; zero: nothing to
  // shape, the engine makes no call). shape_track: the drive and depth in force on `track` (the layer over the step FX over the instrument's `od_inst` / `bits_inst`, `engine_bits` the sampler engine's cap
  // 0 / 4 / 8; a module track passes the neutral instrument values and 0), the rule of shape_set, which calls it. shape_pcm: the passes over n samples in place (the sampler's, shape_span calls it too);
  // it returns at once for a drive and a depth of none. Both in flash: shape_pcm once a block per shaped module track, shape_track beside it (audio/hot.h exceptions).
  struct Shape {
    std::uint16_t drive;
    std::uint8_t crush;
    std::uint8_t round;
  };
  O3OS_AUDIO_INLINE std::uint16_t shape_mask() const noexcept {
    static_assert(perf_slot_of(kModGroupOverdrive) == 0u && perf_slot_of(kModGroupBitDepth) == 1u, "shape_mask reads the layer's slots 0 and 1");
    return static_cast<std::uint16_t>(ifx_on_[1] | ifx_on_[2] | perf_.on[0] | perf_.on[1]);
  }
  O3OS_AUDIO_COLD Shape shape_track(unsigned track, unsigned od_inst, unsigned bits_inst, unsigned engine_bits) const noexcept;
  O3OS_AUDIO_COLD static void shape_pcm(std::int16_t* io, std::uint32_t n, unsigned drive_q11, unsigned crush, bool round) noexcept;
  // OS-PERF-REVERSE: does the step FX 26 (Reverse) hold on `track` (a test and `diag`); a note on the track starts backward when this or perf_held(track, kModGroupReverse) says so.
  bool step_reverse(unsigned track) const noexcept { return track < kVoiceTracks && ((step_rev_ >> track) & 1u) != 0u; }
  // The export (Engine::offline_enter / offline_leave), main loop with the render parked, in flash: put the layer away, clear it (the slots, and the performance filter of every track), bring it back.
  O3OS_AUDIO_COLD void perf_save(PerfSave& out) const noexcept;
  O3OS_AUDIO_COLD void perf_clear() noexcept;
  O3OS_AUDIO_COLD void perf_restore(const PerfSave& in) noexcept;
  // OS-COREFX, the tracker FX (audio/tracker_fx.h), render context, in flash (O3OS_AUDIO_COLD: once a tick for each track that has an effect). tracker_view tells what the processor needs of
  // the sampler voice on `track`: the note-on counter of the note (a new one starts the processor's state over), the pitch note, and the tracker volume 0..64 the note plays at (the
  // instrument's own, or the velocity's (velocity + 1) / 2); false when no voice plays there. tracker_set gives it the pitch OFFSET from the note (1/65536 semitone, the Slide FX's ramp already
  // in place with its distance reached at once; kTrackerKeep leaves the pitch alone) and a tracker volume 0..64 (negative: leave the volume alone), which becomes the voice's gain by the law the
  // note used (VoicePool::regain's glide, or the envelope's next segment). A voice that is releasing keeps its level (regain's rule).
  // CF2: a note that named no velocity keeps its kind for as long as it lasts (Voice::tlaw, decided at the note's start): the processor's volumes (0..64) are vol / 64 of unity for it (C40 is 0 dB,
  // C20 -6 dB; OS-BUG-PASTE24: the reference was level 100 through the instrument law, +24 dB at C40 under the 1.2+ law), so a slide, a retrigger or a tremor moves the level on that scale, and
  // tracker_view gives back the volume the processor last set (Voice::tvol), so a processor that starts over on the same note (the transport's FxReset) reads the level the note has, not one decoded
  // from a gain made by the other law. A note with a velocity (the importer names one for every note it gives a volume) is linear.
  // OS-BUG-MODPLAY F4: rate_hz is the sample's recorded rate (SampleDesc::rate_q28 as Hz; 0: none recorded or 44,100 Hz, the OG's), which sizes the period of the note (trkfx::note_period_at).
  struct TrackerView {
    std::uint32_t age = 0;
    std::uint32_t rate_hz = 0;
    std::uint8_t note = 0;
    std::uint8_t volume = 0;
  };
  static constexpr std::int32_t kTrackerKeep = static_cast<std::int32_t>(0x80000000u);
  O3OS_AUDIO_COLD bool tracker_view(unsigned track, TrackerView& out) const noexcept;
  O3OS_AUDIO_COLD void tracker_set(unsigned track, std::int32_t pitch_q16, int volume) noexcept;
  // CF2. tracker_retrigger restarts the sample of the track's voice from its start (the old output fades under the new, as a retriggered note's), keeping its level and its age; false when no voice
  // (or a granular one) plays. tracker_wave sets the waveform of the track's vibrato (which 0: the finetune LFO) or tremolo (1: the amp LFO) from the tracker's 0..7 (0 sine, 1 ramp down, 2 square,
  // 3 random; bit 2, "no retrigger", is not modelled: the LFOs run on the transport): the LFO's shape is triangle, reverse saw, square or random, for the voice sounding and for the track's
  // next notes, until the transport starts. tracker_gliss makes the Glide and Slide ramps of the track's voices move in whole semitones (a ramp in progress; a fine slide is not one).
  // tracker_reset ends all three on every track. Render context, in flash.
  O3OS_AUDIO_COLD bool tracker_retrigger(unsigned track) noexcept;
  O3OS_AUDIO_COLD void tracker_wave(unsigned track, unsigned which, unsigned wave) noexcept;
  O3OS_AUDIO_COLD void tracker_gliss(unsigned track, bool on) noexcept;
  O3OS_AUDIO_COLD void tracker_reset() noexcept;
  // The transport start: the step FX of the Slide, Glide, Overdrive and Bit depth groups end. The performance layer's values (perf_) are not touched: they are the layer's, as the performance filter is.
  O3OS_AUDIO_INLINE void clear_slide_fxs() noexcept {
    slide_on_ = 0;
    ifx_on_[0] = 0;
    ifx_on_[1] = 0;
    ifx_on_[2] = 0;
  }
  // The Cutoff FX (see CUTOFF FX): `type` kFilterLow / kFilterHigh / kFilterBand, `value` 0..100. Starts or changes the track's override, which acts on the voice sounding
  // now and on the track's next notes, until cleared.
  O3OS_AUDIO_INLINE void set_filter_fx(unsigned track, unsigned type, unsigned value) noexcept {
    if (track < kVoiceTracks && type <= kFilterBand) {
      set_filter(track, true, type, value > 100u ? 100u : value, fx_filter_[track].perf_type, fx_filter_[track].perf_off);
    }
  }
  O3OS_AUDIO_INLINE void clear_filter_fx(unsigned track) noexcept {
    if (track < kVoiceTracks) {
      set_filter(track, false, 0, 0, fx_filter_[track].perf_type, fx_filter_[track].perf_off);
    }
  }
  // The performance layer's cutoff and type (OS-011b; the Perform screen's low-pass, high-pass and band-pass slots, see PERFORMANCE FILTER): the filter is on with the type, its cutoff
  // the base plus `offset` / 100 held to 0..1, until cleared; it wins over the Cutoff FX and it is the layer's own, so clear_filter_fx and the transport start leave it.
  O3OS_AUDIO_INLINE void set_filter_perf(unsigned track, unsigned type, int offset) noexcept {
    if (track < kVoiceTracks && type <= kFilterBand) {
      const FilterFx& fx = fx_filter_[track];
      set_filter(track, fx.on, fx.type, static_cast<unsigned>(fx.cutoff * 100.0f + 0.5f), type + 1u, offset < -100 ? -100 : (offset > 100 ? 100 : offset));
    }
  }
  O3OS_AUDIO_INLINE void clear_filter_perf(unsigned track) noexcept {
    if (track < kVoiceTracks) {
      const FilterFx& fx = fx_filter_[track];
      set_filter(track, fx.on, fx.type, static_cast<unsigned>(fx.cutoff * 100.0f + 0.5f), 0, 0);
    }
  }
  // The whole of a track's filter state at once, as the Engine hands it over from a ModSet or ModEnd of the Filter group: the Cutoff FX (`fx_on`, its type 0..2 and value 0..100)
  // and the performance layer (`perf_type_plus_one` 0 none, else 1..3, and its offset -100..100; the arguments are in range). `track` kVoiceTracks is every track (FxReset: the
  // sequencer's FX end, the performance layer's stay: the callers pass what the track holds). The voice sounding on the track takes it at once.
  // It runs in FLASH, called through a pointer: an event is rare, and the hot code of SRAM_L (ADR 0013 D7) has no room for it (the same arrangement as Graph's pointers into usb::).
  O3OS_AUDIO_INLINE void set_filter(unsigned track, bool fx_on, unsigned fx_type, unsigned fx_value, unsigned perf_type_plus_one, int perf_offset) noexcept {
    filter_set_(*this, track, fx_on, fx_type, fx_value, perf_type_plus_one, perf_offset);
  }
  // The sequencer's Cutoff FX of every track end (FxReset: the transport starts); the performance layer's holds stay.
  O3OS_AUDIO_INLINE void clear_filter_fxs() noexcept {
    set_filter(kVoiceTracks, false, 0, 0, 0, 0);
    lfo_set_(*this, kVoiceTracks, false, 0);  // the Filter LFO FX too (OS-FU2)
  }
  // The Filter LFO FX (see FILTER LFO FX): on with `rate` 0..28 (held to range), or off. In flash, through a pointer, as set_filter.
  O3OS_AUDIO_INLINE void set_filter_lfo(unsigned track, bool on, unsigned rate) noexcept {
    if (track < kVoiceTracks) {
      lfo_set_(*this, track, on, rate);
    }
  }
  // The filter modulation's clock (see FILTER MODULATION; the Engine calls them from the events and the event source). `x100` is the tempo in force, BPM x 100 (once a block); a Step
  // event snaps the tick count to its step (`step` 0..127 is the song cursor's); the transport start (FxReset) restarts it. Without them the LFO runs at 120 BPM from tick 0.
  O3OS_AUDIO_INLINE void set_tempo_x100(std::uint32_t x100) noexcept { tick_rate_q_ = tick_rate_q24(x100); }  // (audio/pitch_ramp.h: the module instruments' Slide FX reads the same rate)
  // In flash (O3OS_AUDIO_COLD, once per Step event): a Step after a stop with no start (Continue, a resume) first gives the voices that sound the phase they have (resume_phase), then snaps the count.
  O3OS_AUDIO_COLD void on_step(unsigned step) noexcept;
  // The transport start (FxReset). In flash through a pointer (a rare event; the hot code is SRAM_L): it restarts the count, and, when the transport was STOPPED, the LFOs of the voices
  // that sound keep their phase (OS-BUG-WTLFO: the count goes on at the tempo while stopped, see LFOS WHILE STOPPED).
  O3OS_AUDIO_INLINE void restart_clock() noexcept { clock_restart_(*this); }
  // The transport stopped (the Stopped event): the count goes on at the tempo in force instead of holding at the end of its step.
  O3OS_AUDIO_INLINE void transport_stopped() noexcept { running_ = false; }
  // OS-MOD: the pan modulator's output. A voice with a pan modulator re-evaluates it once a block (mod_block); the Engine takes the tracks it did that for after the block's voices
  // (take_pan_mods, render context) and gives each one's pan to the graph (pan_mod_value 0..100, the instrument's pan plus the modulation, held to range).
  O3OS_AUDIO_INLINE bool pan_mods_pending() const noexcept { return pan_dirty_ != 0u; }
  O3OS_AUDIO_INLINE std::uint32_t take_pan_mods() noexcept {
    const std::uint32_t m = pan_dirty_;
    pan_dirty_ = 0;
    return m;
  }
  O3OS_AUDIO_INLINE unsigned pan_mod_value(unsigned track) const noexcept { return pan_val_[track < kVoiceTracks ? track : 0u]; }
  // For tests and `diag`: the cutoff (0..1, modulation included) the track's voice's filter was last set to; -1 when the track has no voice or its filter is out of the path.
  float filter_cutoff(unsigned track) const noexcept {
    const unsigned i = find(track);
    return (i != kMaxVoices && v_[i].filt) ? v_[i].fcut : -1.0f;
  }

  // The track's block of this block (kBlock samples, 4-byte aligned), valid until the next begin_block(); null when the track has no voice sounding.
  O3OS_AUDIO_INLINE const std::int16_t* feed(unsigned track) const noexcept { return track < kVoiceTracks ? feed_[track] : nullptr; }

  // For tests and `diag`: how many voices are in use (playing or fading), and the counters.
  unsigned voices_in_use() const noexcept {
    unsigned n = 0;
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      n += (v_[i].playing || v_[i].resid != 0) ? 1u : 0u;
    }
    return n;
  }
  // A voice, sounding or fading, reads descriptor `slot` (a benign read of the render's state, as voices_in_use(): the sample bank's handover and the audition's bind wait for false).
  bool slot_in_use(unsigned slot) const noexcept {
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      if ((v_[i].playing || v_[i].resid != 0) && v_[i].slot == slot) {
        return true;
      }
    }
    return false;
  }
  bool track_playing(unsigned track) const noexcept {
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      if (v_[i].playing && v_[i].track == track) {
        return true;
      }
    }
    return false;
  }
  const Stats& stats() const noexcept { return stats_; }

  // OS-ODD11 slice 2 (test hook, inline: no code unless a test calls it): the state a gated voice's closed-form advance must keep equal to the rendered path's. False: the track has no voice.
  struct Probe {
    std::uint64_t phase;
    std::uint64_t inc;
    std::int32_t amp;
    std::int32_t gain;
    std::int32_t gain_prev;
    std::int32_t shape;
    std::uint32_t ramp_left;
    std::uint32_t env_left;
    std::uint8_t env_stage;
    bool playing;
    bool releasing;
    std::int32_t resid;  // the residual tail's current value (a stop or a retrigger carries `last` into it; 0 after a gated stop, by design)
    std::int32_t last;   // the last sample the voice put out (0 while it is advanced: it put nothing out)
  };
  bool probe(unsigned track, Probe& out) const noexcept {
    const unsigned i = find(track);
    if (i >= kMaxVoices) {
      return false;
    }
    const Voice& v = v_[i];
    out = Probe{v.phase, v.inc, v.amp, v.gain, v.gain_prev, v.shape, v.ramp_left, v.env_left, v.env_stage, v.playing, v.releasing, v.resid, v.last};
    return true;
  }

  // ---- The live voice's position (OS-FU3: the Wave screen's playhead) ----
  // The voice the last LIVE note (pad, MIDI keyboard, preview) started on, and where it is in its sample, as ONE lock-free word: the render is the only writer (mark_live() from Engine::play_live
  // after a note on, the store in end_block(), once a block, relaxed, no loop), the main loop reads it whenever it wants (live_position()). Bit 31: the voice is sounding; bits 24..30: its
  // descriptor slot + 1 (the audition's is 65); bits 0..23: the position in samples from the start of the data the descriptor gave it (saturating at 16 Mi: the arena is 8 Mi). 0: no live voice
  // is sounding (it ended, or another note took its track). The position runs inside the loop while it loops, and a beat slice's is from the start of the slice.
  static constexpr std::uint32_t kLivePlaying = 1u << 31;
  O3OS_AUDIO_INLINE void mark_live() noexcept {
    live_idx_ = last_idx_;
    live_age_ = v_[last_idx_].age;  // the note itself: a voice taken by another note, or retriggered, is no longer the live one
  }
  std::uint32_t live_position() const noexcept { return live_pos_.load(std::memory_order_relaxed); }

  // ---- The voices' load (OS-VOX: the status strip's "VOX 9/16" and `diag voices`) ----
  // Three words the render alone writes, in end_block() once a block, relaxed, no loop (the single-writer rule of status.h; each is monotonic or a plain level, so a reader that
  // sees them from slightly different blocks is not wrong): the voices in use (sounding or fading) at the end of the last block, the most that was in use at the end of a block since
  // boot (the reader cannot reset it: that would be a second writer) and the steals since boot (Stats::steals, which the render alone writes, published). The main loop reads them
  // whenever it wants.
  // `advanced` (OS-ODD11 slice 2, for the unit A/B): the voice spans the closed form took since boot (Stats::advanced_spans). Not published by end_block (hot code): the one writer is gated_span (flash), which stores it with a
  // relaxed atomic store (a plain word store on the M4); the reader sees a count that is at most a block behind.
  struct VoiceSnapshot {
    std::uint32_t in_use;
    std::uint32_t peak;
    std::uint32_t steals;
    std::uint32_t advanced;
  };
  VoiceSnapshot voice_snapshot() const noexcept {
    return VoiceSnapshot{pub_in_use_.load(std::memory_order_relaxed), pub_peak_.load(std::memory_order_relaxed), pub_steals_.load(std::memory_order_relaxed),
                         __atomic_load_n(&stats_.advanced_spans, __ATOMIC_RELAXED)};
  }

 private:
  friend struct VoiceRunTest;  // tests/audio_voices_dir_test.cpp: drives run_dir on a hand-made Voice (the differential test of OS-PERF-CPU2 A1); no code
  friend struct LfoStepTest;   // tests/audio_perflfo_test.cpp: reads a voice's modulator mask and step_lfo_ (OS-PERF-LFO-STEP); no code
  struct Voice {
    const std::int16_t* data = nullptr;
    std::uint64_t phase = 0;      // Q32.32 position in samples
    std::uint64_t inc = 0;        // Q32.32 per output sample
    std::uint32_t end = 0;        // the position at which a loop wraps or a one shot ends
    std::uint32_t loop_len = 0;   // 0: one shot
    std::uint32_t age = 0;        // the note-on counter at its start
    std::uint32_t ramp_left = 0;  // samples of the attack or release ramp still to run
    std::int32_t amp = 0;         // Q24: 1 << 24 is unity (a Q16 gain with 8 more bits)
    std::int32_t amp_step = 0;    // Q24 per sample, while ramp_left != 0 or the envelope runs
    std::int32_t gain = 0;        // Q16: the level the attack reaches (velocity), times the envelope's amount
    std::int32_t last = 0;        // the last sample this voice produced
    std::int32_t resid = 0;       // the residual of an abrupt stop: its current value
    std::int32_t resid_dec = 0;   // its step toward zero per sample
    // The amp envelope (env != nullptr): the stage, the samples it has left (kEnvForever in the sustain), the level Q30 and its step a sample.
    const AmpEnvelope* env = nullptr;
    std::uint32_t env_left = 0;
    std::int32_t shape = 0;
    std::int32_t env_inc = 0;
    std::uint8_t env_stage = 0;   // EnvStage
    std::uint8_t mode = 0;        // the loop's direction (SampleDesc::loop_mode) when there is a loop
    bool bwd = false;             // running backward (modes 1 and 2, after the first turn)
    Svf svf{};                    // the filter (OS-007l): its coefficients and state, read when `filt`
    std::uint8_t ftype = kFilterLow;
    bool filt = false;            // the filter is in the path
    // The filter modulation (OS-007m): `fmod` when the instrument's filter is on and slot 2 modulates it. fbase is the cutoff it moves (the instrument's or the Cutoff FX's), fcut the last one set.
    // The generator of each modulator is mg[] (ModGen: the cutoff's, then OS-MOD's pan, finetune and tremolo).
    bool fmod = false;
    float fbase = 0.0f;
    float fcut = 0.0f;
    ModGen mg[kModGens];
    std::uint8_t slot = 0;        // the instrument the note plays (the filter's settings come back from it)
    std::uint8_t track = 0;
    bool playing = false;
    bool releasing = false;
    // OS-LIVEP (at the end: the offsets of the fields the sample loop reads do not move)
    std::int32_t gain_prev = 0;   // OS-LIVEP: the gain `amp` was made for: equal to `gain` except while a live volume change glides (env_segment ramps from it, run() caps the ramp at the larger)
    std::int32_t tcur = 0;        // OS-MOD: the tremolo's multiplier, Q23 (1 << 23 = unity), at the sample the pass has reached, and its step a sample
    std::int32_t tstep = 0;
    std::int32_t tgt[kBlock / kFilterCtl] = {};  // ... the multiplier it is to reach at each grid point of the block (mod_block evaluates them), and the one at the span's first sample
    std::int32_t t0 = 0;
    std::uint8_t pmask = 0;       // OS-PERF-LFO: bit d (the Perform LFO destination, 0 VOL .. 4 POS): the layer's LFO of the destination plays kPerfLfoMod, not the descriptor's slot (its stored amount is too small to hear)
    std::uint8_t mmask = 0;       // OS-MOD: the modulators that run for this voice, kMmPan | kMmTune | kMmTrem | kMmPos (0: none, the voice costs one test a span for them)
    bool mneed = false;           // OS-MOD: the modulators evaluate at the next span, not at the next block (a note's start)
    std::uint8_t dk = 0;          // OS-ODD11 (GATE): the samples of the re-entry ramp still to run (0: none), or kOwed: the post stages were skipped since the note and the re-entry owes the ramp and a cold filter. In the alignment gap before `base`: the Voice does not grow
    std::int32_t base = 0;        // OS-LIVEP: the note's gain before the envelope's amount: the instrument's, or the velocity's
    std::uint8_t pnote = 0;       // OS-LIVEP: the note the pitch follows (the root for a beat slice), so that a live tune can make the increment again
    bool bvel = false;            // OS-LIVEP: `base` is a step's velocity: the instrument's volume does not move it
    bool tlaw = false;            // OS-COREFX CF2: the note named no velocity: the processor's volumes are vol / 64 of unity for it (decided at the note's start, which tracker_set does not change)
    std::uint8_t tvol = 255;      // ... the tracker volume 0..64 the processor last gave the voice (255: none yet; tracker_view reads the note's own then). These two bytes sit in the alignment gap before `gm`
    // OS-GRAN: a granular voice (mode kModeGranular). `phase` is then the position from `data`, in samples, Q32.32: the grain's start plus x. gstart / gavail: the grain's start and the samples
    // readable from it; gl1 / gl2 its loop's points counted from gstart (gl2 > gl1); gm the envelope index's reciprocal (grain_geom); gpos the position in use, gnew the one that takes over at the next wrap.
    std::uint64_t gm = 0;
    std::uint32_t gstart = 0;     // (a voice that is not granular keeps its note's first frame here, the layer's SMP POS or 0: mods_start sets it, tracker_retrigger restarts there; OS-PERFFX P7, no field of its own: the Voice does not grow)
    std::uint32_t gavail = 0;
    std::uint32_t gl1 = 0;
    std::uint32_t gl2 = 1;
    std::uint16_t gpos = 0;
    std::uint16_t gnew = 0;
    bool gpend = false;
    std::uint8_t gtype = 0;       // 0 forward, 1 backward, 2 ping-pong
    std::uint8_t gshape = 0;
    // OS-PFX, SLIDE FX: the pitch offset now, where the ramp is going, and the samples it still has to run (0: it has landed). 1/65536 of a semitone (OS-IFX1: finer than the Slide's own
    // 1/4096, so that a glide of up to 15 s still moves by whole units a block); the GLIDE uses the same ramp.
    std::int32_t slide_q16 = 0;
    std::int32_t slide_tgt = 0;
    std::uint32_t slide_left = 0;
    // OS-IFX1 (shape_set): the overdrive's gain Q11 (2048..32768, 0: none) and the bit depth's shift (16 - bits, 1..15, 0: none) in force for this voice; render() calls shape_span when either is set.
    std::uint16_t drive = 0;
    std::uint8_t crush = 0;
    std::uint8_t crush_round = 0;  // 1: the 12/8-bit engine's rounding is the reduction (its shift >= the bit depth's); 0: the mask (truncation)
    // OS-CPUOPT1 (C4): the slide offset (q8) and the whole-cent fine tune repitch() last made `inc` from, so that a finetune LFO that lands on the same cent as the last block does not make it again.
    // rp_ok is false whenever the rest of pitch_inc's inputs may have moved: the note's start (inc made another way, pnote) and an edit of the slot's Tune / FineTune (a new descriptor only arrives with no voice sounding on it).
    std::int32_t rp_q8 = 0;
    std::int16_t rp_fine = 0;
    bool rp_ok = false;
  };
  static_assert(offsetof(Voice, dk) < offsetof(Voice, base), "dk fits the gap before base (OS-ODD11): the Voice does not grow");
  static constexpr std::uint8_t kOwed = 255;  // Voice::dk of a voice whose post stages were skipped (the gate)
  static_assert(offsetof(Voice, gm) == ((offsetof(Voice, bvel) + 1u + 7u) & ~7u) && offsetof(Voice, tvol) < offsetof(Voice, gm), "tlaw and tvol fit the alignment gap before gm (where gm sat without them): the Voice does not grow");
  static constexpr std::uint8_t kModeGranular = 3;  // Voice::mode of a granular voice (0..2 are the loop directions)
  static constexpr std::uint8_t kModeReverse = 4;   // Voice::mode of a backward one shot (OS-PERF-REVERSE): run_dir with loop_len = the window, bwd from the start, and a stop below its first frame
  static constexpr std::uint8_t kMmSlide = 32u;  // OS-PFX: the Slide FX moves the pitch (mod_block); the bit stays until the ramp has landed and the target is in the pitch
  static constexpr std::uint8_t kMmPan = 1u, kMmTune = 2u, kMmTrem = 4u, kMmTremOff = 8u, kMmPos = 16u;  // kMmPos: OS-GRAN, the position modulator; kMmTremOff: the tremolo was switched off live, the pass ramps back to unity and ends
  static constexpr std::int32_t kTremOne = 1 << 23;  // the tremolo's unity, Q23
  O3OS_AUDIO_INLINE static const FilterMod& dmod(const Voice& v, unsigned d, const FilterMod& m) noexcept { return ((v.pmask >> d) & 1u) != 0u ? kPerfLfoMod : m; }  // the modulator destination d plays (OS-PERF-LFO)
  static constexpr std::uint8_t mod_bit(unsigned k) noexcept { return k == 3u ? kMmPos : static_cast<std::uint8_t>(1u << k); }  // the bit of mg[kMgPan + k] in mmask: pan, finetune, tremolo, position
  enum EnvStage : std::uint8_t { kEnvOff = 0, kEnvDelay, kEnvAttack, kEnvHold, kEnvDecay, kEnvSustain, kEnvRelease };
  enum FStage : std::uint8_t { kFeOff = 0, kFeDelay, kFeAttack, kFeHold, kFeDecay, kFeSustain, kFeRelease };  // the cutoff ADSR's phase order
  // The tick count is integer, Q24 ticks in a u64, so that it does not depend on how a block is split into spans. Ticks a sample is 576 ticks a step x (BPM / 15) steps a second / the
  // engine's rate (the rate option follows hal::audio): in Q24 per 100 x BPM, 384 x 2^24 / the rate in mHz x 1000 (up to 14.6 million at 1000 BPM).
  static constexpr std::uint64_t kTickMaxQ24 = (static_cast<std::uint64_t>(kTicksPerStep) << 24) - 1u;  // the last tick of a step, held there until the next Step event
  static constexpr std::uint32_t kEnvForever = 0xFFFFFFFFu;

  O3OS_AUDIO_HOT unsigned find(unsigned track) const noexcept;
  O3OS_AUDIO_HOT unsigned allocate() noexcept;
  O3OS_AUDIO_HOT std::int16_t* touch(unsigned i) noexcept;
  O3OS_AUDIO_HOT void render_voice(Voice& v, std::int16_t* out, std::uint32_t begin, std::uint32_t end) noexcept;
  // One voice's span in segments of constant envelope: rendered into `out` (Adv false, render_voice), or advanced in closed form with no block (Adv true, gated_span; OS-ODD11 slice 2). One text, two callers.
  template <bool Adv>
  O3OS_AUDIO_INLINE void render_seg(Voice& v, std::int16_t* out, std::uint32_t begin, std::uint32_t end) noexcept;
  // OS-ODD11 slice 2: the closed form of run<Ramp, false>(v, out, n) without the samples: the position and the amplitude n samples on, n if the voice is still sounding after them, 0 if a one shot ran out.
  static O3OS_AUDIO_INLINE std::uint32_t advance(Voice& v, std::uint32_t n, bool ramp) noexcept;
  // The filter over n samples that start at index `pos` of the block, the cutoff moved at every kFilterCtl-th index (and at the voice's first sample).
  O3OS_AUDIO_HOT void filter_run(Voice& v, const FilterMod* m, std::int16_t* block, std::uint32_t pos, std::uint32_t n) noexcept;  // the filter over n samples of the block from pos, in place; m: the cutoff modulator (OS-CPUOPT1)
  O3OS_AUDIO_HOT static float fenv_eval(ModGen& g, const FilterMod& m) noexcept;                        // the ADSR's output after the time since the last call
  O3OS_AUDIO_HOT float flfo_level(ModGen& g, const FilterMod& m, std::uint32_t samples_on) noexcept;     // the LFO's level A x shape(phase), `samples_on` samples into the span
  O3OS_AUDIO_HOT float flfo_eval(ModGen& g, const FilterMod& m, std::uint32_t samples_on) noexcept;      // ... and the cutoff's, pan's, finetune's output 2 x level - A
  // lfo_fx: 0, or the Filter LFO FX's rate + 1; rate0: the LFO table's index of the instrument's rate 0 (0, the amp table's is 5)
  O3OS_AUDIO_HOT static void mod_start(ModGen& g, const FilterMod& m, std::uint32_t step, float kill, unsigned lfo_fx, unsigned rate0 = 0) noexcept;
  // OS-MOD (voices.h PAN, FINETUNE, AMP LFO MODULATION). trem_span is the tremolo's pass over a voice's span (HOT: per sample: the multiply only); mod_block the once-a-block evaluation of the pan, the
  // finetune and the tremolo's four grid targets (flash, O3OS_AUDIO_COLD: once per voice per block, and only for a voice that has one); mods_start is its note start (flash, once per event).
  O3OS_AUDIO_HOT void trem_span(Voice& v, std::int16_t* block, std::uint32_t pos, std::uint32_t n) noexcept;
  O3OS_AUDIO_COLD void mod_block(Voice& v, const SampleDesc& s, std::uint32_t begin) noexcept;
  O3OS_AUDIO_COLD void gate_cut(Voice& v, std::uint32_t n) noexcept;  // OS-ODD11: a gated voice's cutoff ADSR clock over n samples (flash, a call a gated span)
  O3OS_AUDIO_COLD void odd_span(Voice& v, unsigned i, std::int16_t* block, std::uint32_t begin, std::uint32_t end, bool gate) noexcept;  // OS-ODD11: a gated span, or a span of the re-entry (flash, see render)
  O3OS_AUDIO_COLD bool gated_span(Voice& v, std::uint32_t begin, std::uint32_t end) noexcept;  // OS-ODD11 slice 2: a gated span of a plain voice in closed form (flash, a call a gated voice and span); false: not that kind, nothing done
  O3OS_AUDIO_INLINE void gate_mark(Voice& v, unsigned i, std::uint32_t n) noexcept;  // a span of n samples that was gated: the voice's bit, the re-entry owed, the cutoff clock (odd_span, gated_span)
  O3OS_AUDIO_INLINE void post_stages(Voice& v, std::int16_t* block, std::uint32_t begin, std::uint32_t end) noexcept;  // the tremolo, the filter and the shaper (render and odd_span)
  O3OS_AUDIO_COLD void mods_start(Voice& v, const SampleDesc& s, bool retrigger, unsigned track) noexcept;
  O3OS_AUDIO_COLD void slide_start(Voice& v, int sixteenths) noexcept;  // OS-PFX: the ramp to `sixteenths` from where the voice's offset is, over one step of the tempo (flash, per FX event and per note)
  // OS-PERFFX P5 (PERFORMANCE LAYER, TUNE): the layer's pitch base of `track` in 1/65536 semitone (0: the layer holds none). Flash. Every writer of Voice::slide_q16 / slide_tgt keeps it as the offset's zero.
  O3OS_AUDIO_COLD std::int32_t tune_base_q16(unsigned track) const noexcept;
  // OS-PERFFX P7 (PERFORMANCE LAYER, SMP POS AND SMP END): the frame the layer's slot `slot` of `track` puts a note at, in a part of `len` frames, and `fallback` when the layer holds none. Flash, once a note.
  O3OS_AUDIO_COLD std::uint32_t smp_frame(unsigned track, unsigned slot, std::uint32_t len, std::uint32_t fallback) const noexcept;
  // note_on's part of the two (flash, once a note that has the layer on): (first frame << 32) | last frame of the part the note plays; kSmpSilent (after cutting the track's note) when the start is at or past the end.
  static constexpr std::uint64_t kSmpSilent = ~std::uint64_t{0};
  O3OS_AUDIO_COLD std::uint64_t smp_window(unsigned track, const SampleDesc& s, bool loops, std::uint32_t length) noexcept;
  // OS-IFX1 (GLIDE, OVERDRIVE, BIT DEPTH), all in flash. glide_start: the ramp from the previous note's pitch to the note's own over the glide time, through slide_*; shape_set: the voice's drive and
  // depth from the instrument or the track's FX (per note, per live edit, per FX event); shape_span: the two passes over n samples of a voice's block, called by render() when shape_set made one.
  O3OS_AUDIO_COLD void glide_start(Voice& v, const SampleDesc& s, unsigned track) noexcept;
  O3OS_AUDIO_COLD void shape_set(Voice& v, const SampleDesc& s, unsigned track) const noexcept;
  O3OS_AUDIO_COLD static void shape_span(const Voice& v, std::int16_t* io, std::uint32_t n) noexcept;
  O3OS_AUDIO_COLD void ifx_event(unsigned track, unsigned group, unsigned fx_type, unsigned value) noexcept;
  O3OS_AUDIO_COLD void perf_filter_to(unsigned track, unsigned type_plus_one, int offset) noexcept;  // OS-PERFFX P0: the track's performance filter (type + 1, 0 none; offset) set, the Cutoff FX kept (flash, the export)
  O3OS_AUDIO_COLD void layer_event(unsigned track, const SeqEvent& e) noexcept;  // OS-PERFFX P0: a ModSet / ModEnd of a group other than Slide: the layer's value, then the step layer's FX (ifx_event)
  static void move_position(Voice& v, const SampleDesc& s, float mod) noexcept;  // OS-GRAN: the position modulator's output, once a block (flash, from mod_block)
  struct FilterFx {
    float cutoff = 0.0f;
    std::uint8_t type = kFilterLow;
    bool on = false;
    std::int8_t perf_off = 0;      // the performance layer's cutoff offset, -100..100 (OS-011b)
    std::uint8_t perf_type = 0;    // 0: the layer does not hold the filter, else its type + 1
  };
  static_assert(sizeof(FilterFx) == 8u, "the performance layer's two bytes fit the padding of FilterFx: no SRAM");
  using FilterSetFn = void (*)(VoicePool&, unsigned, bool, unsigned, unsigned, unsigned, int) noexcept;
  using LfoSetFn = void (*)(VoicePool&, unsigned, bool, unsigned) noexcept;
  using ClockFn = void (*)(VoicePool&) noexcept;
  static void clock_restart_impl(VoicePool& pool) noexcept;  // flash, like lfo_set_impl (restart_clock)
  O3OS_AUDIO_COLD static void tick_carry_stopped(VoicePool& pool) noexcept;  // flash: the stopped clock's carry into the next step, at most once a block (render)
  static void resume_phase(VoicePool& pool) noexcept;  // flash (on_step): the first Step after the count ran on while stopped: the sounding voices' LFOs keep their phase
  // The tick count the LFOs read now (flfo_level without the voice's shift): flash callers only.
  std::uint32_t tick_base() const noexcept { return (tick_steps_ != 0u ? tick_steps_ - 1u : 0u) * kTicksPerStep + static_cast<std::uint32_t>(tick_q_ >> 24); }
  static void lfo_set_impl(VoicePool& pool, unsigned track, bool on, unsigned rate) noexcept;  // flash, like filter_set_impl
  static void filter_set_impl(VoicePool& pool, unsigned track, bool fx_on, unsigned fx_type, unsigned fx_value, unsigned perf_type_plus_one, int perf_offset) noexcept;  // flash
  O3OS_AUDIO_HOT static void filter_apply(Voice& v, const SampleDesc& s, const FilterFx& fx, unsigned lfo_fx) noexcept;  // the voice's filter from the instrument, or from the track's Cutoff FX
  // OS-LIVEP, in flash (per ParamChange, see apply_param): what a live edit does to the voice that plays the slot.
  static void regain(Voice& v, const SampleDesc& s) noexcept;
  // OS-RATEPITCH: the voice's increment (Q32.32): the note's pitch with the tune, `fine` and the slide's offset `q8` (1/4096 semitone, 0: none), times the sample's rate factor, in the
  // wavetable's law for a wavetable. In flash (O3OS_AUDIO_COLD: once a note, and once a block for a voice with a slide or the finetune modulator) to keep SRAM_L for the kernels.
  O3OS_AUDIO_COLD static std::uint64_t pitch_inc(unsigned pnote, const SampleDesc& s, int fine, std::int32_t q8) noexcept;
  void repitch(Voice& v, const SampleDesc& s, int fine) const noexcept;  // `fine`: the fine tune to play (the instrument's, or with the finetune modulator's offset)
  void refilter(Voice& v) noexcept;
  void remod(Voice& v, std::uint32_t& pan_tracks) noexcept;  // a live edit of slot 1, 5 or the amp LFO's enable: the voice's modulators follow the descriptor
  void remod_k(Voice& v, unsigned k, std::uint32_t& pan_tracks) noexcept;  // ... one modulator of them (k: mod_slots order, pan, finetune, amp, position)
  // OS-PERF-LFO (PERFORMANCE LAYER, THE PERFORM LFOS), in flash. perf_index: the 0..28 table index of the layer's LFO for destination d (0 VOL, 1 PAN, 2 TUNE, 3 FLT, 4 POS) on the voice, or -1 when the layer holds
  // none or the destination cannot act (FLT: no filter runs; POS: not a wavetable or granular voice). perf_dest makes the voice's generator the layer's and says whether it owns the destination (false: nothing
  // touched, the instrument's own state stands); `live` is false at a note start (mod_start's fresh phase, the tremolo's first sample without a ramp). perf_sync brings destination d to what the layer and
  // the instrument ask for, on the sounding voice of a track (the layer changed). lfo_rerate moves a running LFO to table index `idx`, keeping its phase.
  bool perf_lfo_held(unsigned track) const noexcept {
    unsigned any = 0;
    for (unsigned d = 0; d < kLfoDests; ++d) {
      any |= perf_.on[kPerfLfoSlot + d];
    }
    // OS-PERF-LFO-STEP: a held step FX 23 / 24 / 33 / 36 counts too (inlined into mods_start only, flash)
    return track < kVoiceTracks && (((any >> track) & 1u) != 0u || (step_lfo_[0][track] | step_lfo_[1][track] | step_lfo_[2][track] | step_lfo_[3][track]) != 0u);
  }
  int perf_index(const Voice& v, unsigned d) const noexcept;
  bool perf_dest(Voice& v, unsigned d, bool live) noexcept;
  void perf_sync(Voice& v, unsigned d) noexcept;
  void lfo_rerate(ModGen& g, unsigned idx) noexcept;
  void lfo_live(unsigned slot, unsigned e, unsigned f, std::uint32_t& pan_tracks) noexcept;  // OS-LFOLIVE: a live edit of a slot's SOURCE / LFO field, on the voices
  bool apply_loop(unsigned slot, unsigned field, float value) noexcept;  // OS-MOD, LIVE LOOP
  bool apply_position(unsigned slot, unsigned field, float value) noexcept;  // OS-GRAN, the live wavetable window and granular position
  O3OS_AUDIO_HOT void start_release(Voice& v, std::uint32_t samples) noexcept;
  O3OS_AUDIO_HOT void release_voice(Voice& v, bool fade) noexcept;  // NoteOff / NoteFade / the stop: the envelope's release, or the plain ramp
  O3OS_AUDIO_HOT static void env_enter(Voice& v, EnvStage stage, std::uint32_t units) noexcept;
  O3OS_AUDIO_HOT static bool env_segment(Voice& v, std::uint32_t n) noexcept;
  O3OS_AUDIO_HOT static void env_advance(Voice& v, std::uint32_t n, bool stage_end) noexcept;
  O3OS_AUDIO_HOT static void stop_voice(Voice& v) noexcept;
  O3OS_AUDIO_HOT static void carry(Voice& v, std::int32_t value) noexcept;
  O3OS_AUDIO_HOT static void add_residual(Voice& v, std::int16_t* out, std::uint32_t n) noexcept;
  // The sample loop: up to n samples into out, the attack / release ramp or the envelope running (Ramp) or the gain steady. Dir: the loop runs backward or
  // ping-pong (a voice with loop_mode != 0 and a loop), so the position turns at the loop's ends. Returns how many it made (fewer than n: a one shot ran
  // out). Updates the voice's position, amplitude and last sample.
  template <bool Ramp, bool Dir>
  static O3OS_AUDIO_INLINE std::uint32_t run(Voice& v, std::int16_t* out, std::uint32_t n) noexcept;
  // OS-CPU10 slice 2 (A5): the k samples of a forward loop that cannot reach the end of the data, in a function of their own so that the loop keeps its few values in registers (inlined in
  // run() the register allocator spilled the output pointer and the increment to the stack). Reads and writes the voice's position and amplitude. Same integers as the checked loop.
  template <bool Ramp>
  static O3OS_AUDIO_INLINE void fast_body(Voice& v, std::int16_t* out, std::uint32_t k, std::int32_t top) noexcept;
  O3OS_AUDIO_HOT static void fast_steady(Voice& v, std::int16_t* out, std::uint32_t k, std::int32_t top) noexcept;
  O3OS_AUDIO_HOT static void fast_ramp(Voice& v, std::int16_t* out, std::uint32_t k, std::int32_t top) noexcept;
  O3OS_AUDIO_HOT static std::uint32_t run_steady(Voice& v, std::int16_t* out, std::uint32_t n) noexcept;
  O3OS_AUDIO_HOT static std::uint32_t run_ramp(Voice& v, std::int16_t* out, std::uint32_t n) noexcept;
  O3OS_AUDIO_HOT static std::uint32_t run_dir(Voice& v, std::int16_t* out, std::uint32_t n) noexcept;  // the ramp variant: a steady voice has a step of 0
  // OS-GRAN (voices.h GRANULAR), all in flash: grain_geom makes the grain's points from the descriptor and a position (once a note, once a refresh); run_gran is the grain's sample loop (one call
  // a span a voice, like the other loops but from flash: SRAM_L has no room for it), refresh_grain takes the pending position at a wrap (at most once a block a voice: the position moves once
  // a block).
  O3OS_AUDIO_COLD static void grain_geom(Voice& v, const SampleDesc& s, unsigned pos) noexcept;
  O3OS_AUDIO_COLD static void grain_start(Voice& v, const SampleDesc& s) noexcept;  // a granular note's start (once a note)
  O3OS_AUDIO_COLD void refresh_grain(Voice& v) noexcept;
  template <unsigned Type>
  O3OS_AUDIO_INLINE std::uint32_t run_gran_t(Voice& v, std::int16_t* out, std::uint32_t n) noexcept;
  O3OS_AUDIO_COLD std::uint32_t run_gran(Voice& v, std::int16_t* out, std::uint32_t n) noexcept;

  Voice v_[kMaxVoices];
  SampleDesc samples_[kDescSlots];
  StageCell* stage_ = nullptr;                // main loop sets once; see bind_stage()
  std::atomic<std::uint8_t> stage_state_{0};  // the staged descriptor's hand-over: main loop 0 -> 1 (cell filled first), render 1 -> 2 -> 0; stage_cancel() 1 -> 0
  std::atomic<std::uint32_t> stage_takes_{0}; // render writes (once per take), main loop reads
  O3OS_AUDIO_COLD void take_stage() noexcept;
  LevelLaw law_ = LevelLaw::Og12;  // OS-LIVEP: the instrument volume law of the project (set_law)
  alignas(8) std::int16_t blocks_[kMaxVoices][kBlock] = {};
  const std::int16_t* feed_[kVoiceTracks] = {};
  static constexpr std::uint8_t kNoSlice = kNoForcedSlice;
  std::uint8_t forced_slice_[kVoiceTracks] = {kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice,
                                              kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice, kNoSlice};  // the Slice FX, per track
  FilterFx fx_filter_[kVoiceTracks];  // the Cutoff FX and the performance layer's hold, per track
  std::int16_t slide_fx_[kVoiceTracks] = {};  // OS-PFX: the Slide FX per track, in sixteenths of a semitone, signed; held when the track's bit of slide_on_ is set
  std::uint16_t slide_on_ = 0;
  // OS-IFX1: the Glide, Overdrive and Bit depth FX per track (index 0, 1, 2): the value in force and, in ifx_on_[k] bit t, whether track t holds the FX. last_note1_: the pitch note (+ 1; 0 none) of
  // the last sampler note on the track, which the next one glides from. Render context, like the rest.
  std::uint8_t ifx_val_[kVoiceTracks][3] = {};
  std::uint16_t ifx_on_[3] = {};
  std::uint8_t last_note1_[kVoiceTracks] = {};
  FilterSetFn filter_set_ = &VoicePool::filter_set_impl;
  LfoSetFn lfo_set_ = &VoicePool::lfo_set_impl;
  ClockFn clock_restart_ = &VoicePool::clock_restart_impl;
  std::uint8_t fx_lfo_[kVoiceTracks] = {};  // the Filter LFO FX per track: 0 none, else the rate + 1
  // The transport clock of the filter LFOs: ticks = 576 x (steps - 1) + tick_q_ / 2^24 since the start, tick_q_ held at the end of its step.
  // OS-BUG-WTLFO: running_ is set by a Step event and cleared by the Stopped event (render context, both). While it is false the count is not held at the end of a step but runs on (the carry
  // into tick_steps_ is made at the end of each span): the LFOs go on at the tempo in force. While it is true nothing differs from before.
  std::uint32_t tick_steps_ = 0;
  std::uint64_t tick_q_ = 0;
  bool running_ = false;
  std::uint32_t tick_rate_q_ = static_cast<std::uint32_t>(12000.0f * kTickRateQ24PerX100);  // 120 BPM until the engine says
  std::uint32_t cursor_step_ = 0;                  // the song cursor's step at the last Step event
  static constexpr std::uint32_t kRngSeed = 0x2545F491u;
  std::uint32_t rng_ = kRngSeed;                   // the random LFO's xorshift32 (render context; seed_random() puts it back, main loop, render parked)
  std::uint32_t touched_ = 0;  // voices whose block was zeroed and written this block, one bit each (0..kMaxVoices - 1); bit kVisit + i: voice i ran a closed-form span (gated_span) and its block was left alone
  static constexpr unsigned kVisit = 16;
  static constexpr std::uint32_t kSeen = 1u + (1u << kVisit);  // voice i is "seen" this block when either of its two bits (kSeen << i) is set
  static_assert(kMaxVoices <= kVisit, "touched_ holds the touched bits below kVisit and the visited bits from it");
  std::uint16_t gate_ = 0;     // OS-ODD11: the tracks the engine says are inaudible (set_gate), bit t = track t; render context only
  std::uint16_t gated_ = 0;    // ... the voices (bit i, kMaxVoices <= 16) that skipped their post stages this block: their block is not fed to the track
  static_assert(kMaxVoices <= 16, "gated_ holds one bit a voice");
  std::uint8_t last_idx_ = 0;                 // the voice the last note_on used (render only)
  std::uint8_t live_idx_ = 0xFF;              // the voice of the last live note (mark_live), 0xFF none
  std::uint32_t live_age_ = 0;                // its note-on counter
  std::atomic<std::uint32_t> live_pos_{0};    // see live_position()
  std::atomic<std::uint32_t> pub_in_use_{0};  // see voice_snapshot(): render writes, main loop reads
  std::atomic<std::uint32_t> pub_peak_{0};
  std::atomic<std::uint32_t> pub_steals_{0};
  std::uint32_t age_ = 0;
  std::uint32_t pan_dirty_ = 0;                // OS-MOD: tracks whose voice's modulated pan was evaluated since the Engine last took them (bit t)
  std::uint8_t pan_val_[kVoiceTracks] = {};    // ... and the pan (0..100) each of them is to have
  std::uint16_t gliss_ = 0;                    // OS-COREFX CF2: the tracks whose slide ramps move in whole semitones (tracker_gliss), bit t
  std::uint8_t wave_[2][kVoiceTracks] = {};    // ... the LFO shape + 1 the tracker FX gave each track's finetune LFO [0] and amp LFO [1] (tracker_wave), 0: the instrument's
  Stats stats_;
  // OS-PERF-REVERSE (P8): the tracks whose next note plays backward, bit t. perf_rev_: the performance layer's PLAYBACK (put away and brought back by the export with perf_); step_rev_: the step FX 26
  // (cleared by the transport start, tracker_reset, and by the export's start and end). Both written by layer_event and read by mods_start (note start), render context / main loop with the render parked.
  std::uint16_t perf_rev_ = 0;
  std::uint16_t step_rev_ = 0;
  PerfSlots perf_ = {};  // OS-PERFFX P0, render context (layer_event) and the main loop with the render parked (perf_save / perf_clear / perf_restore): the performance layer's values (PERFORMANCE LAYER, THE REST OF THE GROUPS)
  // (declared last: a member added in the middle moved the offsets that .ramfunc_l code encodes, +8 B)
  // OS-PERF-LFO-STEP: the step FX Volume LFO 23, Panning LFO 24, Finetune LFO 33 and Position LFO 36 per track (rows 0..3 = VOL, PAN, TUNE, POS; the FLT's is fx_lfo_): 0 none, else the rate index + 1
  // (VOL: the index in the amp's table of 24, perf_index adds 5). Single writer: layer_event / lfo_set_impl / perf_clear / perf_restore, render context (or the main loop with the render parked); read in flash
  // (perf_index, perf_lfo_held). The Perform layer's value wins while it is held and not 0; FxReset ends them, Stopped does not (docs/specs/OS-PERF-LFO-STEP.md).
  std::uint8_t step_lfo_[4][kVoiceTracks] = {};
};

}  // namespace o3os::audio
