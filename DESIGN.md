# Friction — design document

Friction is a sidechain plugin. It sits on a **target** track, receives a **reference** track through
an auxiliary (sidechain) input, and measures and controls the psychoacoustic roughness (beating,
sensory dissonance) between the two. It answers: *which partials of the target are beating against
the reference right now, and how badly?* Then it can reduce that friction (Smooth), retune it away
(Retune), or deliberately add it (Tension). The reference is never altered or output.

This document covers the research behind the design, the exact models implemented, the signal flow,
the parameters, the latency budget and the decisions taken (with reasons). Items marked
**[UNVERIFIED]** are claims I could not check against a primary source or by measurement. Section 11
records what the automated test harness actually measured.

---

## 1. Roughness models (research summary)

### 1.1 Plomp & Levelt (1965)
*"Tonal consonance and critical bandwidth"*, JASA 38, 548–560. Listeners rated the consonance of
pairs of pure tones as a function of their frequency difference. Key findings, which every later
model inherits:

* Dissonance is zero at unison. It rises steeply to a maximum at roughly **a quarter of a critical
  bandwidth (CB)**, then falls off. Beyond about one CB the pair is heard as consonant.
* The curve scales with the critical bandwidth, not with the musical interval. In the bass, where
  CBs are ~100 Hz wide, intervals as large as a fourth or fifth between pure tones are rough. At
  4 kHz the same intervals are smooth.
* For complex tones, total dissonance is approximately the **sum over all partial pairs**. This is
  why consonant intervals of harmonic tones (with coinciding partials) are smooth.

Plomp & Levelt published the curve graphically, not as a formula.

### 1.2 Sethares' parameterisation (1993, 2005)
W. A. Sethares, *"Local consonance and the relationship between timbre and scale"*, JASA 94(3)
(1993), and *Tuning, Timbre, Spectrum, Scale* (2nd ed., 2005). He fits the Plomp–Levelt curve with
a difference of exponentials:

$$ d(f_1,f_2) = e^{-b_1 x} - e^{-b_2 x},\qquad x = s\,|f_2-f_1|,\qquad s=\frac{d^*}{s_1 f_{min}+s_2} $$

with $b_1 = 3.5$, $b_2 = 5.75$, $d^* = 0.24$, $s_1 = 0.0207$, $s_2 = 18.96$. The term
$s_1 f_{min}+s_2$ plays the role of a critical bandwidth that grows with frequency. The curve peaks
at $x^* = \ln(b_2/b_1)/(b_2-b_1) = 0.2206$, where $d_{max} = 0.18077$.

Amplitude weighting has varied. The 1993 paper uses the product of loudnesses. Sethares' later
MATLAB code (`dissmeasure.m`) uses $\min(a_1,a_2)$ and the constant $b_1=3.51$. Published
re-implementations round the constants differently (e.g. $s_1=0.021$, $s_2=19$). **I use 3.5 /
5.75 / 0.24 / 0.0207 / 18.96.** These are the values used by Vassilakis and by the published
Python package `dissonant`. I checked them against that package's source.

### 1.3 Vassilakis' amplitude-weighted model (2001, 2005)
P. N. Vassilakis, PhD dissertation (UCLA 2001); *"Auditory roughness as a means of musical
expression"*, Selected Reports in Ethnomusicology 12 (2005). Vassilakis keeps Sethares' frequency
term but replaces the amplitude weighting with two terms. One is a weak dependence on overall level
(SPL). The other is a strong dependence on the **amplitude-fluctuation degree**: how deep the beats
are, which depends on the amplitude *ratio* of the two partials.

$$ R = X^{0.1}\cdot 0.5\,Y^{3.11}\cdot Z $$
$$ X = A_{max}A_{min},\qquad Y = \frac{2A_{min}}{A_{min}+A_{max}},\qquad Z = e^{-b_1 s (f_{max}-f_{min})}-e^{-b_2 s (f_{max}-f_{min})} $$

$s$, $b_1$, $b_2$ are as above, with $s$ evaluated at $f_{min}$. Two equal partials ($Y=1$) beat with
full modulation depth and give maximum roughness. A partial 20 dB below its neighbour ($Y=0.18$)
contributes only $0.18^{3.11}\approx0.5\%$ of that. This is exactly the behaviour a mix tool
needs: a quiet partial next to a loud one cannot cause strong beating. The formula was cross-checked
against the `dissonant` package; I did not re-read the dissertation itself. **[UNVERIFIED: primary
source]**

### 1.4 Choice and the exact formula implemented
**Choice: Vassilakis (Sethares frequency term + Vassilakis amplitude terms), peak-normalised.**
Reasons:

1. It is the only one of the three that models beat *depth*. Depth is what decides whether the
   reduction in Smooth mode actually reduces audible beating. Attenuating a target partial lowers
   $Y$, so the model predicts directly what Smooth achieves.
2. It is smooth, closed-form and cheap. It runs from a 4096-entry lookup table, so a full
   target-by-reference pair search costs microseconds.
3. Its frequency term is Sethares' fit of Plomp–Levelt. The test "peak near ~25 % of a CB" is
   therefore the Plomp–Levelt behaviour.

Implemented pair roughness (`fr::pairRoughness`, `src/core/Roughness.h`), with amplitudes $a$ as
linear peak amplitudes (1.0 = full-scale sine):

$$ r(f_1,a_1,f_2,a_2) = (a_1 a_2)^{0.1}\;Y^{3.11}\;\hat Z(x),\qquad \hat Z(x) = \frac{e^{-3.5x}-e^{-5.75x}}{0.18077},\qquad x=\frac{0.24\,|f_2-f_1|}{0.0207\,f_{min}+18.96} $$

The only change from Vassilakis is normalisation. I divide by $0.5\,Z_{max}$, so **two equal
0 dBFS sines at the worst spacing give $r=1$.** All Friction scores share this scale. Pairs with
$x > 2.0$ are skipped; there $\hat Z<0.5\%$.

Reference values of $\hat Z$ for a 440 Hz partial (the shape the sine-pair test checks):

| Δf (Hz) | 0 | 1 | 3 | 10 | 30 | 100 |
|---|---|---|---|---|---|---|
| $\hat Z$ | 0.000 | 0.102 | 0.284 | 0.718 | 0.988 | 0.237 |

The peak at 440 Hz is at Δf = 25.8 Hz.

Derived quantities:

* **Per-partial friction** of target partial $i$ against the reference:
  $F_i = \sum_j r(f_i,a_i,f_j,a_j)$ over reference partials $j$, with the coincidence correction
  below. The whole pair is attributed to the target partial, because only the target can be changed.
* **Coincidence correction** (added during implementation, §10 decision 16). Let $j^*$ be the
  reference partial nearest to $f_i$ (smallest $x$). Let $w_c = 1$ for $x_{ij^*}\le x^*/4$, falling
  with a raised cosine to 0 at $x^*$. Every other pair is then reduced by the roughness that
  already exists *inside* the reference:
  $F_i = r_{ij^*} + \sum_{j\ne j^*}\max\big(0,\;r_{ij} - w_c\,r_{j^*j}\big)$.
  A target partial that coincides with a reference partial adds no new beating against that
  partial's neighbours. Without the correction, a unison or octave doubling of a harmonic reference
  scores as rough: its partials beat against the reference's neighbouring partials exactly as the
  reference's own partials do.
* **Self (target-internal) friction:** $F^{self}_i = \sum_{j\ne i,\;a_j\ge a_i} w_s(x_{ij})\,r_{ij}$,
  with $w_s = 1$ for $x\le x^*/2$, raised-cosine to 0 at $x^*$. Only pairs closer than the
  roughness peak count. The regular spacing of one tone's own harmonics ($f_0$ apart) is timbre,
  not beating. Closely spaced partials (detuned unison, chorus, doubled takes) are what Self mode
  should treat.
  * Each pair is attributed to its *weaker* partial (split equally on ties). Reason: attenuating
    the weaker partial of a rough pair lowers $Y$ and so the beating. Attenuating both equally
    leaves $Y$ unchanged.
* **Total friction:** $R = \sum_i F_i$. The display maps it to 0–100 with $100\tanh(R/2)$.
* **Friction curve** (display): $c(E)=\sum_i F_i\,\exp(-\tfrac12((E-E_i)/0.5)^2)$, a Gaussian of
  0.5 ERB in ERB-rate $E$ (§2), truncated at ±2 ERB and evaluated on the 256-point
  log-frequency display grid (20 Hz–20 kHz).
* **Audibility gate:** partials below −84 dBFS, or more than 60 dB below the frame's strongest
  partial, are ignored. Without this, $X^{0.1}$ would let inaudible partials score.

**Limits of the model.** It is a steady-state sine-pair model summed over pairs. It ignores masking
between more than two partials, phase, and the fact that very slow beats (< ~2 Hz) are heard as
loudness wobble rather than "roughness". Slow beating still scores (0.1 for a 1 Hz beat at 440 Hz),
and the Sensitivity control sets how much of it to treat. **[UNVERIFIED: summing pairs for complex
tones is the standard approximation (Sethares, Vassilakis); it was not validated by listening tests
here.]**

---

## 2. Critical bandwidth and the ERB scale

* Zwicker & Terhardt (1980) critical bandwidth: $CB(f)=25+75\,(1+1.4f_{kHz}^2)^{0.69}$ Hz (about
  100 Hz below 500 Hz, about 20 % of $f$ above).
* Glasberg & Moore (1990) equivalent rectangular bandwidth: $ERB(f)=24.7\,(4.37f_{kHz}+1)$ Hz, with
  ERB-rate $E(f)=21.4\log_{10}(4.37f_{kHz}+1)$.

Sethares' $s_1f+s_2$ term implies a "critical bandwidth" about 4.5 times the peak distance. Measured
against each definition, the roughness peak falls at:

| f (Hz) | peak Δf (Hz) | peak / Zwicker CB | peak / ERB |
|---|---|---|---|
| 55 | 18.5 | 0.18 | 0.60 |
| 440 | 25.8 | 0.23 | 0.36 |
| 1000 | 36.5 | 0.23 | 0.28 |
| 4000 | 93.6 | 0.14 | 0.21 |

The "~25 % of a CB" rule holds for Zwicker's CB in the mid range. It does not hold for the ERB,
which is much narrower in the bass. **[UNVERIFIED: which CB definition Plomp & Levelt intended
maps onto these numbers only approximately; they used the CB data available in 1965.]**

Uses in Friction: the ERB-rate scale is the display axis for the friction curve and the width unit
for the spectral masking test (§3.1). The Sethares $s$ term defines the roughness window. Tension's
"grit" target is the Sethares peak distance, $\Delta f_{peak}(f) = 0.2206/s(f)$.

---

## 3. Partial tracking

Target and reference each get the same analysis chain, run on their mono sum (L+R)/2. Stereo is
processed linked (one decision for both channels), so the stereo image cannot shift.

### 3.1 High band: STFT peaks with reassignment
* **Frame:** periodic Hann window, $N$ = 4096 at 44.1/48 kHz (scaled ×2 per sample-rate octave, so
  the window stays ≈85–93 ms), hop $N/4$, no zero padding.
* **Frequency refinement: reassignment / instantaneous frequency.** The frame is also transformed
  with the analytic derivative of the window, $h'(n)=\frac{\pi}{N}\sin(2\pi n/N)$. For bin $k$ the
  instantaneous frequency is $\hat\omega_k=\omega_k-\mathrm{Im}\{X_{h'}(k)/X_h(k)\}$ (Kodera et
  al. 1978; Auger & Flandrin 1995). For a stationary sinusoid this is exact up to leakage from other
  components, and it needs no previous frame. I prefer it to the phase-difference phase vocoder
  estimate, which needs hop-to-hop unwrapping and is biased by fast chirps (kick drops), and to
  quadratic interpolation of log magnitude (Smith & Serra 1987; Abe & Smith 2004), whose bias with
  an unpadded Hann window is a few hundredths of a bin. Quadratic interpolation is still used to
  locate the peak and as a fallback.
* **Amplitude:** $\hat a = 2|X(k)|/|W(\delta)|$, where $W(\delta) = \tfrac N2\,\mathrm{sinc}(\delta)/(1-\delta^2)$
  is the Hann kernel at the fractional offset $\delta$. This corrects scalloping.
* **Noise rejection:**
  1. Local maximum over ±2 bins.
  2. Above −96 dBFS and within 70 dB of the frame maximum.
  3. **Prominence** of at least 10 dB over the surrounding floor: the log-spectral mean of ±ERB/2
     (at least ±4 bins) around the peak's region, *excluding* the region itself, so a smeared peak
     is not compared with itself.
  4. **Reassignment consistency:** the reassigned frequency must lie within 0.75 bin of bin $k$,
     and those of bins $k-1$ and $k+1$ must agree with it within 0.2 bin. Noise peaks scatter;
     sinusoids do not. This test does the most to separate tonal partials from noise.
  5. **Frequency-modulated partials** (vibrato, glides) fail test 4. They are kept when they are at
     least 15 dB prominent. Their frequency is then the energy-weighted reassigned frequency of the
     region, and their amplitude comes from the region's energy (Parseval; Hann sum of squares
     $3N/8$). Verified with ±25–100 cent vibrato at 5.5 Hz (§11).
* **Masking rejection:** a peak is dropped if it lies below the spreading threshold of a stronger
  peak in the same signal: $L_j - 18\,\text{dB} - 25\,\text{dB/ERB}\,(E_j-E_i)$ for $i$ below $j$,
  and $15\,\text{dB/ERB}$ slope above (upward spread of masking). Cross-signal masking (target under
  reference) needs no separate test. Vassilakis' $Y^{3.11}$ term already gives such a partial
  almost no roughness.
* **Continuation (McAulay & Quatieri 1986):** peaks are matched greedily, nearest first, to existing
  tracks within $\max(1\text{ bin},\;5\%)$ (×1.5 after a missed frame; 5 % ≈ 85 cents follows wide
  vibrato at the hop rate). Unmatched tracks survive 2 frames before they die. Each track carries an
  ID, so friction, gain and shift state can be smoothed *per partial* and dips follow vibrato.

### 3.2 Low band: time-domain fundamental tracking
At 48 kHz an 85 ms STFT gives 11.7 Hz bins, about 3.7 semitones at 55 Hz, and it smears a kick's
pitch drop over the whole window. The low band therefore uses a time-domain tracker:

1. **Band limit and decimation:** an 8th-order Butterworth low-pass at 900 Hz (double precision)
   runs at the full rate. It then decimates by $D=\mathrm{round}(f_s/4000)$ to $f_d\approx4$ kHz.
   The full-rate low-passed signal is kept in a ring buffer for step 3.
2. **YIN** (de Cheveigné & Kawahara 2002) on the decimated signal: difference function
   $d(\tau)=\sum_{n=0}^{W-1}(x_n-x_{n-\tau})^2$ with $W$ = 20 ms, **anchored at the newest
   samples** (the latest $W$ samples against the $W$ samples one lag earlier), so a short period
   locks in after $W+\tau$ rather than after the longest lag. Then the cumulative-mean-normalised
   difference $d'$, the absolute threshold 0.15 (first dip below it, which avoids octave-down
   errors), and parabolic interpolation on $d$. Lag range covers 27.5–420 Hz.
   * There is no DC blocker before YIN. The difference function ignores DC, and a high-pass would
     add an onset transient that delays lock-in.
   * Measured time to lock within 5 cents: 12–48 ms for 41–294 Hz, 56 ms at 31 Hz (§11).
   * I chose YIN over MPM (McLeod & Wyvill 2005) because its first-dip-below-threshold rule is the
     better guard against the octave errors that strong 2nd harmonics cause in bass and 808.
3. **Full-rate refinement:** at 4 kHz one lag sample is 14 cents at 400 Hz, and parabolic
   interpolation is biased there. The difference function is re-evaluated on the *full-rate*
   low-passed buffer at the five lags around $\tau D$ and a parabola is fitted. This gives
   cents-level accuracy at every f0. **[Verified in tests §11.]**
4. **Harmonics:** amplitudes of $k f_0$ ($k=1..8$, up to 1.25× crossover) are computed by
   projection onto $e^{-jk\omega_0 n}$ over a Hann window of exactly $m$ periods ($m\ge2$). With
   that window length every other harmonic falls on a null of the Hann kernel, so the estimates do
   not leak into each other (pitch-synchronous analysis). Harmonic frequencies are taken as exactly
   $k f_0$; string inharmonicity in this range is < 1 cent.
5. **Hop and kick drops:** an estimate every 4 ms (16 decimated samples). The only continuity
   constraint is an outlier test: if the two previous estimates agree within 3 % and the new one
   differs from both by more than 6 %, the previous value is held for that hop. Monotonic glides
   such as a 160→52 Hz kick drop are followed. Voicing requires $d'_{min}<0.2$, band RMS above
   $2\cdot10^{-4}$ (≈ −74 dBFS) and 25 Hz ≤ $f_0$ ≤ 440 Hz.
6. **STFT cross-check** (added during implementation, §10 decision 19). YIN can lock onto a common
   period of a low tone and a stronger tone above the crossover (measured: 52 Hz under 222 Hz read
   as 55.6 Hz). In Mix mode (bins ≤ 15 Hz), a stationary STFT peak below 1.5× the crossover that
   lies within one bin of a low-band harmonic but differs by more than max(0.3 Hz, 0.15 bin)
   replaces that harmonic's frequency and amplitude. Non-stationary peaks (kick drops) keep the YIN
   value. The correction ratio is reused on the 4 ms hops until the next frame, but only while the
   tracker stays within 3 % of the $f_0$ it was measured at.

**Band assignment:**
* Below the crossover (default 200 Hz) the low-band tracker's partials are used while it is
  voiced.
* When it is unvoiced (noise, chords), STFT peaks below the crossover are used instead (Mix mode).
* STFT peaks are always used above the crossover.

### 3.3 Smoothing and hysteresis
* Per-track friction is smoothed with a one-pole filter: 30 ms rise, 120 ms fall.
* Processing starts when smoothed friction exceeds the threshold $T$. A track that is already active
  stays active until friction falls below $0.75\,T$. This hysteresis stops dips flickering at the
  threshold.
* Depth, shift and synth-amplitude targets then go through the user Attack/Release one-pole filters.
* Retune targets also have decision hysteresis: a new target is adopted only if it lowers friction
  by more than 20 % compared with the current one.
* Per-partial Retune searches are cached per track. They are repeated only when the partial moved
  by more than 0.05 % or its friction changed by more than 10 %, and at least every 8 frames. The
  applied shift still glides every frame.

---

## 4. Pitch shifting (research summary and choice)

* **Phase vocoder** (Flanagan & Golden 1966; Dolson 1986). Each bin is resynthesised at its
  instantaneous frequency. Without phase locking, partials lose vertical phase coherence:
  "phasiness", and smeared transients.
* **Laroche & Dolson (1999):** *"Improved phase vocoder time-scale modification of audio"* (IEEE
  TSAP 7(3)) and *"New phase-vocoder techniques for pitch-shifting, harmonizing and other exotic
  effects"* (WASPAA 1999). Peaks are found, each bin is assigned to the peak region it belongs to
  (valley to valley), and each region is rotated **rigidly** (identity phase locking). For pitch or
  frequency shifting, a region is moved by an integer number of bins and its phase is advanced by
  $2\pi\Delta f\,H/f_s$ per hop. Each partial can therefore get its own shift, and bins outside the
  shifted regions pass unchanged.
* **Sinusoidal + residual modelling** (McAulay & Quatieri 1986; Serra & Smith 1990, SMS) gives
  explicit partials that can be resynthesised anywhere. Removing them from the residual cleanly needs
  very accurate phase and amplitude estimates. Any error leaves a ghost partial that beats against
  the shifted one, which is exactly the artifact this plugin exists to remove.
* **Transient preservation** (Röbel 2003; Duxbury et al. 2002): reset phases at onsets and do not
  process transient frames.

**Choices:**
* **Per-partial Retune:** Laroche–Dolson peak-region shifting in the Mix-mode STFT. Regions run
  valley to valley, capped at ±4 bins. Each is moved by `round(Δf/bin)` bins and rotated by an
  accumulated $\phi \mathrel{+}= 2\pi\Delta f H/f_s$, applied identically to L and R. Frames with an
  onset pass unprocessed and reset the accumulators.
  * Leaving the fractional part to the phase rotation is safe. With a Hann² analysis/synthesis
    product at 4× overlap, a ≤0.5-bin residual costs about 0.2 dB, and the hop-rate images sit
    below −40 dB. **[Verified in tests §11.]**
* **Whole-signal Retune:** not a phase vocoder. A **resampling delay line with
  correlation-optimised, onset-aligned splices.**
  * The delayed input is read at ratio $\rho=2^{c/1200}$ through a 32-tap windowed-sinc
    interpolator. This is an exact pitch shift with no phasiness and no transient smearing; at ±50
    cents the only side effect is a ±2.9 % time compression.
  * The read head drifts from the nominal latency-compensated position by $(\rho-1)$ samples per
    sample. It is pulled back by splices when the drift exceeds 10 ms. Each splice is a 12 ms
    crossfade to a jump target chosen by normalised cross-correlation over the 10 ms drift range,
    so the waveforms line up. The crossfade is equal-gain when the correlation exceeds 0.6 and
    equal-power otherwise.
  * Lookahead (inside the Mix-mode latency) shows upcoming onsets.
    * Before each onset, an **alignment splice** starts as late as possible, ending 1 ms before
      the onset. Its target is pre-compensated for the drift that accumulates until the onset, so
      every hit is output on time. It is skipped when the residual timing error would be below
      0.3 ms. Measured worst onset displacement: 0.04 ms (§11).
    * No routine splice starts within ~25 ms before an onset or within 60 ms after one. A forced
      splice happens only beyond 30 ms of drift (at most 0.45 L).
    * When the ratio returns to 1, a final splice puts the head back on the nominal position, and
      the path is again an exact delay.
  * At 50 cents the head drifts 10 ms in ≈0.34 s, so a routine splice happens roughly every
    0.3 s; at 10 cents, every ≈1.7 s.
  * This is the "simpler, fewer artifacts" option the brief asks for: ideal for kicks, bass and any
    monophonic tonal source.

---

## 5. Signal flow

```
                    ┌──────────── TARGET (main in, mono/stereo) ─────────────┐
                    │                                                        │
   mono sum ──► Target analysis                                    exact delay L ──► dry
                ├ STFT (Hann N, hop N/4) + reassignment ─► HF peaks ┐        │
                ├ LF tracker (LPF 900 Hz ▸ ↓D ▸ YIN ▸ refine ▸ harmonics) ─► LF partials
                └ onset detector (time domain)                      │        │
                                                                    ▼        │
   REFERENCE (sidechain) ─► mono sum ─► same analysis ─► ref partials        │
                                                                    │        │
                    Roughness engine (Vassilakis, LUT):  F_i, F_self, curve, total, worst clash
                    + smoothing/hysteresis per track ───────────────┤        │
                                                                    ▼        ▼
   MIX mode (latency L = N):                                                 
     SMOOTH : STFT(L,R) × dip gains (HF partials) ─► ISTFT/OLA ─► SVF bell cuts at LF partials ─► wet
     RETUNE : per-partial ─ STFT region move + phase rotation (PV) ─► ISTFT/OLA ─► wet
              whole       ─ delay line @ ratio ρ, sinc interp, onset-aligned splices ─► wet
     TENSION: synth bank (detuned copies of ref partials) + wet (optional Push via PV) ─► wet
   TRACK mode (latency 0): SMOOTH only ─ SVF bell bank (≤24 HF + 8 LF) driven by lagging analysis
                                                                    │
   out = trim · ( Delta ? mix·(wet−dry) : dry + mix·(wet−dry) );  Bypass ⇒ crossfade to dry (still delayed by L)
   GUI ◄── triple buffer (spectra, friction curve, gains/shifts, clash readout, status)
   Advisor thread ◄── SPSC FIFO (per-frame partial lists while Learning)
```

The "crossover" between the two Smooth processors is a **partition of the frequency axis, not an
audio band split.**
* STFT dips act only on partials ≥ crossover.
* Bell filters act only on low-band-tracker partials < crossover.
* The audio itself is never split into bands. Where nothing is processed the path is an exact delay,
  so no crossover summing error is possible. This is as phase-coherent as a crossover can be.
* The bells are minimum-phase but narrow (BW ≈ 10 % of f, 3–20 Hz). Their phase shift is confined
  to the partial being cut.

**Time alignment (Mix):**
* STFT frame $m$ is analysed and modified in place, so its gains are inherently aligned.
* Low-band estimates are stamped with their window-centre input time $u$ and applied to the output
  at $u+L$. The tracker lag (≈35–45 ms incl. decimation filter delay) is less than $L$ (≈85 ms),
  so the low-band filters act *with lookahead*.
* Tension voices are scheduled the same way.

**Idle path:** when no partial is being processed for 4 consecutive frames (the overlap depth), the
output is the exactly delayed dry signal (bit-exact). This guarantees the Amount-0 / silent-sidechain
nulls. Switching between the two costs nothing audible, because the overlap-add path is
perfect-reconstruction to within float rounding.

**Sidechain handling:**
* Sidechain bus disabled or 0 channels ⇒ status "No sidechain" ⇒ **Self** analysis
  (target-internal friction).
* Bus connected but silent ⇒ status "Sidechain silent" ⇒ **no processing**, as the brief requires.
* The user can also force Self.

---

## 6. Processing modes

### 6.1 Smooth
* **Threshold** from Sensitivity $S\in[0,1]$: $T = 0.6\cdot(1/30)^{S}$. That gives 0.6 at $S=0$,
  0.11 at 50 %, 0.066 at the 65 % default and 0.02 at 100 %.
* **Depth per partial:** $D_i = 24\,\text{dB}\cdot \text{Amount}\cdot(1-e^{-(F_i-T)_+/0.15})$.
  This is proportional to friction × Amount for small excesses and saturates at 24 dB × Amount.
* **High band (Mix):** dips are drawn around each active track in dB. Full depth covers the
  partial's own peak region (valley to valley, shrunk by one bin on each side, but at least ±1 bin
  around the partial), with raised-cosine skirts of 2 bins (11.7 Hz bins at N = 4096 / 48 kHz).
  Overlapping dips add, capped at −30 dB. The same gains are applied to L and R spectra.
* **Pre-echo guard (Mix STFT modes):** in the 25 ms before a detected onset, the overlap-add
  output crossfades (2 ms raised cosine) to the time-aligned dry signal. Otherwise the STFT would
  spread a removed or shifted component of the onset up to a window length *before* it. This
  needs the onset detector to see soft onsets, so it also uses a second-difference
  (+12 dB/oct) detection function (§10 decision 22).
* **Low band:** one TPT state-variable bell (Simper/Cytomic form) per low-band harmonic (≤8). Each
  follows the partial's frequency, with BW = clamp(0.1 f, 3, 20) Hz, and coefficients are
  interpolated every 16 samples. With 0 dB gain the bell is an exact identity ($m_1=k(A^2-1)=0$).
* **Track mode:** high-band partials also use bells, ≤24 of them, with BW = max(6 Hz, f/30), driven
  by an analysis with N/2 (2048 at 48 kHz) and hop N/4, which has no lookahead. When more than 24
  partials want a dip, bells go to the highest priority, depth (dB) + partial level (dB), so
  audible partials are served first.

### 6.2 Retune (Mix only)
* **Per-partial:** for each target track with $F_i>T$, the cost
  $J(c)=F_i(f_i2^{c/1200}) + 0.1\,F_i(f_i)\,|c|/c_{max}$ is minimised over
  $c\in[-c_{max},c_{max}]$, against the reference partials that can matter anywhere in that range.
  * Candidates are exact unison with each of those reference partials, plus a 5-cent grid. When
    the grid minimum beats every unison candidate, it is refined to 1 cent.
  * The second term of $J$ prefers the smallest shift that does the job.
  * The shift is applied only if the best cost is below half the current friction.
  * The minimum lands on unison with the nearest reference partial or on a spot clear of all of them.
    That is the low-roughness relationship available to a single partial; consonant ratios of
    complex tones emerge from these partial coincidences.
  * Shifts glide with Attack/Release and are scaled by Amount.
* **Whole Auto:** one global offset minimises $\sum_i F_i(c) + 0.1\,R_0\,|c|/c_{max}$ over the 48
  strongest eligible target partials ($R_0$: total friction of the eligible target partials at
  $c=0$). The search is a 2-cent grid followed by golden-section
  refinement within ±2 cents, and runs on every other frame (≈43 ms at 48 kHz).
  * It is applied only if it at least halves the friction.
  * A new offset replaces the current one only if it lowers the cost by more than 20 %, or
    differs by less than 3 cents.
  * The result is scaled by Amount, glides with Attack/Release and feeds the resampling shifter.
* **Whole Fixed:** a static offset (−1200…+1200 cents), set manually or by the Advisor's *Apply*.

### 6.3 Tension (Mix only)
* **Synth** (default):
  * Up to 16 phase-continuous quadrature oscillators, each locked (by track ID) to one of the
    strongest reference partials inside the range. Voices glide when the partial moves and fade over
    50 ms when it dies, so they follow the reference's harmony.
  * Voice frequency: $f_j \pm \beta\,\Delta f_{peak}(f_j)$, with the sign alternating by rank.
  * Beat Rate bias $b\in[0,1]$ maps to $\beta=0.04\cdot25^{b}$: about 1 Hz beats at 440 Hz (slow
    wobble) up to the roughness peak (fast grit).
  * Voice amplitude = Amount × reference partial amplitude (equal amplitude = full beat depth).
* **Push:** the per-partial PV engine with the opposite objective. It shifts target partials toward
  the max-roughness position next to their nearest reference partial, capped by Max Cents.
* Mix and Delta apply as usual.

### 6.4 Global
* **Delta** outputs `mix·(wet−dry)`, so dry + Delta = output. For Smooth it is the (polarity-inverted)
  removed signal; for Tension the added one.
* **Focus:** Low processes only partials below the crossover, High only those above, Both all.
* **Range Low/High** limits which partials may be processed. Analysis always uses everything.

---

## 7. Tuning Advisor
* **Learn** on: each analysis frame (≈21 ms) pushes a compact record onto a lock-free SPSC FIFO.
  * The record holds ≤32 target and ≤32 reference partials (f, a, low/high-band flag), low-band f0s,
    voicing and the onset flag.
  * It contains no heap data and costs the audio thread a memcpy.
* **Background thread** (`juce::Thread`, polls every 30 ms) accumulates the friction curve
  $R(c)=\sum_{frames}\sum_i\sum_j r(f_i2^{c/1200},a_i,f_j,a_j)$ for $c\in[-600,600]$ cents at
  1-cent steps.
  * Each pair is evaluated only over the $c$ range where it can be rough. The cost was not
    benchmarked separately, but in the plug-in test 8 s of audio are learned and finished within a
    test run of under 3 s, so the Advisor runs faster than real time.
  * The same coincidence correction as §1.4 is applied.
  * The low-band-only curve $R_{LF}(c)$ is accumulated the same way.
* **On stop:** best offset $c^*=\arg\min R$, reported in cents (plus semitones + cents when
  $|c^*|\ge100$), with expected reduction $100\,(R(0)-R(c^*))/R(0)$ % and the curve plotted.
  *Apply* sets Retune → Whole Fixed with offset $c^*$.
* **Kick vs bass:**
  * Kick detection: at least 3 onsets, and the target's low-band f0 in the first 30 ms after the
    onsets lies on average more than 10 % above the f0 40–800 ms after them (a pitch drop that
    settles).
  * Kick fundamental: the amplitude-weighted median of the settled f0 (≥40 ms after each onset).
  * Clashing bass notes: the reference low-band f0 during kick tails, quantised to note names with
    cents error and ranked by the friction they caused.
  * Best-compromise kick tuning: $\arg\min R_{LF}$, reported as a new kick fundamental and note
    name.

---

## 8. Parameters

All parameters live in an `AudioProcessorValueTreeState`, are automatable and are saved and
restored. Tapers are JUCE `NormalisableRange` skews: "log" means the skew is set so the centre of
the range is the geometric mean.

| ID | Name | Range | Default | Taper | Units |
|---|---|---|---|---|---|
| `mode` | Mode | Smooth / Retune / Tension | Smooth | choice | – |
| `latency` | Latency mode | Mix / Track | Mix | choice | – |
| `reference` | Reference | Sidechain / Self | Sidechain | choice | – |
| `amount` | Amount | 0 – 100 | 50 | linear | % |
| `sensitivity` | Sensitivity | 0 – 100 | 65 | linear (→ threshold exponential, §6.1) | % |
| `attack` | Attack | 1 – 500 | 20 | log | ms |
| `release` | Release | 10 – 2000 | 150 | log | ms |
| `mix` | Mix | 0 – 100 | 100 | linear | % |
| `trim` | Output Trim | −24 – +24 | 0 | linear | dB |
| `rangeLow` | Range Low | 20 – 20000 | 20 | log | Hz |
| `rangeHigh` | Range High | 20 – 20000 | 20000 | log | Hz |
| `focus` | Focus | Low / High / Both | Both | choice | – |
| `crossover` | Crossover | 80 – 400 | 200 | log | Hz |
| `delta` | Delta | off / on | off | toggle | – |
| `bypass` | Bypass | off / on | off | toggle (host bypass) | – |
| `maxCents` | Max Cents | 1 – 100 | 50 | linear | cents |
| `retuneStyle` | Retune Style | Per-partial / Whole Auto / Whole Fixed | Per-partial | choice | – |
| `wholeOffset` | Whole Offset | −1200 – +1200 | 0 | linear (0.1 step) | cents |
| `beatRate` | Beat Rate | 0 – 100 (slow wobble → fast grit) | 40 | linear (→ β exponential) | % |
| `tensionStyle` | Tension Style | Synth / Push | Synth | choice | – |

The Advisor's last result and the editor size are stored as extra properties in the same state tree.

---

## 9. Latency budget

| Mode | Reported latency | Components inside it | Analysis lag (control only) |
|---|---|---|---|
| Mix @ 44.1 / 48 kHz | N = 4096 smp (92.9 / 85.3 ms) | STFT frame N (OLA needs the full frame); LF tracker window + decimation delay used as lookahead; whole-signal drift (normally ≤ 10 ms, forced splice at 30 ms ≤ 0.45 L) and 16-sample sinc half-width; onset lookahead (alignment splices, pre-echo guard) | none (aligned) |
| Mix @ 88.2 / 96 kHz | N = 8192 smp (92.9 / 85.3 ms) | same | none |
| Mix @ 176.4 / 192 kHz | N = 16384 smp (92.9 / 85.3 ms) | same | none |
| Track (any rate) | **0 smp** | time-domain bells only | decisions lag the audio by about half a 43 ms window plus up to one 21 ms hop, plus the 30 ms friction rise **[estimated from the frame geometry, not measured]** |
| Bypass | same as active mode | exact delay of dry | – |

**Track mode trade-off:**
* Dips arrive a few tens of milliseconds late on new notes, because decisions lag the audio.
* The high-band analysis window is half as long (≈43 ms, 23 Hz bins), so frequency precision is
  lower for dense spectra.
* At most 24 + 8 simultaneous dips.
* Bells add minimum-phase shift around each cut.
* There is no Retune or Tension.

Changing the latency mode during playback causes a 10 ms mute crossfade, and the new latency reaches
the host from the message thread.

---

## 10. Decisions (with reasons)

1. **Pure-C++ DSP core (`FrictionCore`) separate from JUCE.** The whole engine can be unit-tested
   offline, bit-for-bit, with no host. The JUCE layer only maps parameters, buses, state and GUI.
2. **Own radix-2 real FFT** in the core. No dependency, deterministic across platforms (JUCE's FFT
   backend differs per OS), and fast enough at these sizes (≈1 % CPU for all transforms in Mix
   mode).
3. **Reassignment frequency estimation** rather than phase-difference phase vocoder estimation:
   single-frame, unbiased for chirps, no unwrapping (§3.1).
4. **YIN with full-rate refinement** for the low band (§3.2): time-domain, 35–66 ms, cents-level.
5. **Vassilakis roughness, peak-normalised** (§1.4).
6. **Frequency-axis partition instead of an audio crossover** (§5): phase-coherent by construction.
7. **Bells as TPT SVFs** (Simper): stable under per-sample modulation, and an exact identity at
   0 dB, which the null tests need.
8. **Whole-signal Retune by resampling plus smart splices**, not a phase vocoder (§4): no smearing
   or phasiness, onsets stay time-aligned.
9. **Per-partial Retune by Laroche–Dolson region shifting** (§4). Shipped only if it passes the
   artifact tests (§11); otherwise it is disabled and documented.
10. **Track mode = zero-latency filter bank** rather than a tiny STFT. At 10 ms an STFT has ~94 Hz
    bins and could not make "narrow dips".
11. **Latency L = N in all Mix sub-modes.** Switching Smooth/Retune/Tension never changes the
    latency, so mode automation is safe.
12. **Silent sidechain ⇒ no processing; missing sidechain ⇒ Self.** Required by the brief, and the
    UI shows which case applies.
13. **Lock-free everywhere on the audio thread:** a triple buffer to the GUI, an SPSC FIFO to the
    Advisor, atomics for parameters. All buffers are sized in `prepareToPlay` for the largest FFT
    and block. A latency-mode change is published to the host by a message-thread timer.
14. **Per-sample (not per-block) scheduling** of every control event, so output is independent of
    host block size (tested bit-exact).
15. **Windows binaries:** this development container is Linux and its network policy blocks
    Microsoft's SDK/CRT downloads, so MSVC cannot run here. The CMake project is written for MSVC,
    and a GitHub Actions workflow builds VST3 + Standalone with MSVC on `windows-latest` and runs
    the test harness and pluginval there. Local builds and tests run on Linux (GCC 13). The MSVC
    build links the static runtime, so the plug-in needs no VC++ redistributable.

**Decisions taken during implementation** (each one forced by a failing test or a missed budget):

16. **Coincidence correction** (§1.4). Without it, unison and octave references produced false
    friction: a doubled harmonic tone scored as rough, and Smooth cut consonant material. With it,
    a unison reference is bit-exact transparent. The amount of processing then follows consonance:
    unison < octave < fifth < fourth/third/tritone/minor second.
17. **Self friction only below the roughness peak** (§1.4). Otherwise a single harmonic tone scores
    as rough against itself.
18. **Vibrato-tolerant peak picking** (§3.1, test 5). The stationarity test alone dropped vibrato
    partials. Strongly prominent non-stationary peaks are now kept, with energy-based estimates.
19. **YIN: 20 ms window anchored at the newest samples, no DC blocker, STFT cross-check** (§3.2).
    The first design (30 ms window anchored at the oldest samples, with a DC blocker) took
    56–72 ms to lock. The cross-check fixes subharmonic locks under a strong tone above the
    crossover.
20. **Track mode: 2048-sample analysis (48 kHz) with 50 % overlap; bells allocated by depth +
    level** (§6.1). Track mode only analyses and never resynthesises, so half overlap is enough. The
    priority rule fixed a measured case: the bells went to weak high-frequency partials with
    inflated friction scores, and the audible clash was left uncut.
21. **Per-partial search by candidates plus a cache; Whole Auto on the 48 strongest partials every
    other frame** (§3.3, §6.2). Needed for the Mix CPU budget (< 10 %). Measured: 7.8 % and 6.3 %.
22. **Pre-echo guard and a second-difference onset function** (§6.1). The demo tests found
    +11 dB high-frequency bursts 5 ms before quiet piano notes. The STFT smeared removed energy of
    soft onsets backwards, and the broadband and first-difference detection functions did not
    detect those onsets over a ringing chord.
23. **Late, drift-compensated alignment splices** (§4). The first design spliced early, so the head
    drifted again before the hit. With large offsets (+96 cents, from the Advisor's kick retune)
    hits came out early and the splice added high-frequency energy.
24. **Raised-cosine transition fades** (10 ms, for mode and latency switching). They splatter less
    than linear ramps. Measured worst burst while switching: −56.5 dB re output RMS (§11).
25. **Per-partial Retune ships.** It passed every artifact test (transient smearing, phasiness,
    partials outside Range; §11), so decision 9's fallback (Whole-signal only) was not needed.
26. **Own on/off parameter class for Delta and Bypass.** `juce::AudioParameterBool` keeps the raw
    normalised value a host sends (0.44 means "off"). The value-tree state compares snapped values, so
    restoring a saved "off" over 0.44 changed nothing (found by pluginval). The replacement stores
    only 0 or 1.
27. **Latency sync at 5 Hz.** The processor's timer only forwards latency changes (Mix ↔ Track) to the
    host. 5 Hz keeps the message thread mostly idle, which also makes a teardown race in pluginval's
    Linux host (TEST_REPORT.md) less likely. The engine itself switches immediately.
28. **The Advisor FIFO is allocated once, in the engine constructor.** The Advisor thread pops from it
    at any time. `prepare()` used to reset its read and write indices, and a reset that races a pop
    can leave the FIFO permanently "full", so Learn would silently stop receiving data after a
    sample-rate or buffer-size change.

---

## 11. Verification results

The harness (`tests/`) generates `TEST_REPORT.md`, which lists every check with its measured value and
criterion, and the WAV renders in `renders/`. Summary of the run on Linux (GCC 13.3, 2.1 GHz
Xeon VM): **69/69 core checks and 11/11 plug-in checks pass.** External validators on the Linux
VST3: **pluginval 1.0.4 at strictness 10 passes**, and **Steinberg's VST3 validator (SDK 3.8.1,
extensive mode) passes 537/537 tests.**

| Brief item | Measured |
|---|---|
| Sine pairs 440 / 440+d Hz, d = 0, 1, 3, 10, 30, 100 | 0.000, 0.089, 0.247, 0.625, 0.860, 0.206. Equal to the model (§1.4) within rounding; peak at 25.8 Hz = 23 % of Zwicker's CB. The same holds at 60 Hz through the low-band (YIN) path. |
| Two saws 8 cents apart, Smooth | Beating modulation depth −13.3 dB (mean of 12 harmonics; fundamental −7.0 dB) in Mix, −14.5 dB in Track, at Amount 100 %, Sensitivity 80 %. Low band 55 vs 57.2 Hz: −17.8 dB. |
| Two saws 8 cents apart, Retune | Beat rate 1.019 Hz → 0.001 Hz (Whole Auto) / 0.002 Hz (per-partial); every harmonic < 0.01 Hz. |
| Kick (pitch drop to 52 Hz) vs bass line, Advisor | Best offset +96 cents vs oracle +97 cents (criterion ±5 cents). Kick fundamental 52.1 Hz (true 52.0); clashing notes C2, A1, G1 listed; best-compromise kick 55.0 Hz (A1). |
| Nulls | Amount 0 and Mix 0 in every mode, and a silent sidechain: output **bit-exact** equal to the dry signal delayed by the latency. |
| Consonant references | Unison: bit-exact (no processing). Octave: residual −35.1 dB re input. |
| Retune artifacts | Drum hits: no pre-echo (at the level of the input's silence between hits); attack energy within 0.21 dB (above 2 kHz within 0.72 dB). Phasiness: crest factor 16.8 → 15.7 dB, AM ripple of a shifted partial 0.04 dB. Partials outside Range: residual −58 / −66 dB. Whole-signal hits on time within 0.04 ms. |
| Real-time safety | **0 heap allocations** on the audio thread: 10,990 `setParams()`+`process()` calls (every mode, Self, no sidechain, live switching, Advisor learning) and 2,250 `processBlock`/`processBlockBypassed` calls under random automation. Counted by a replaced global `operator new` (per thread), with a self-check that the counter sees a deliberate allocation. No locks, I/O or exceptions on that path (by inspection: the core uses no mutex, file or stream I/O, or `throw`). |
| Robustness | NaN/Inf (treated as 0), DC passed unchanged, denormal-level input (time ratio 0.18), **bit-exact block-size independence** (block sizes 1, 7, 64, 333, 512, 4096, all modes), the same Smooth/Retune results at 44.1–192 kHz, linked stereo exact, live mode switching (worst burst −56.5 dB), +18 dBFS input. |
| Latency | Impulse latency equals the reported latency at 6 sample rates × 2 modes; output aligned while processing; bypass latency-compensated. |
| CPU (48 kHz, stereo + stereo sidechain) | Mix: Smooth 4.5 %, per-partial Retune 7.8 %, Whole Auto 6.3 %, Tension 4.4 %. Track: 2.4 % of one core. |
| Musical demos | Piano-like part (+18 cents) vs pad: mean friction −64 % (Smooth), −59 % (per-partial), −37 % (Whole Auto). Kick vs bass: −71 % (Smooth, Focus Low), −21 % (Advisor's +96-cent kick retune). No added high-frequency bursts > +3 dB in any render. |

**Not verified:**
* **[UNVERIFIED: no listening tests]** The roughness model, the thresholds and the musical value of
  the processing were judged by measurement only, on synthetic signals.
* **[UNVERIFIED: no DAW testing]** The plug-in was exercised by its own test host, pluginval and
  Steinberg's validator, not inside Live, FL Studio, Reaper, Cubase, Studio One or Bitwig. The
  README's routing steps come from those DAWs' documentation.
* **[UNVERIFIED: real recordings]** Dense polyphonic mixes, noisy recordings and extreme vibrato
  were not tested beyond the synthetic cases above.
* Windows: the binaries are built and tested by CI (MSVC on `windows-latest`). The README states
  which commit's CI run to download.
