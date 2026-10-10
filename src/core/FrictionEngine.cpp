#include "FrictionEngine.h"

namespace fr
{

namespace
{
    constexpr float kMaxDepthDb = 24.0f;
    constexpr float kDepthScale = 0.15f;      // friction excess giving 63 % of the maximum depth
    constexpr float kAudibleFloor = 6.3e-5f;  // -84 dBFS
    constexpr float kRelFloor = 1.0e-3f;      // -60 dB below the frame's strongest partial
    constexpr int kMaxSynthVoices = 16;

    inline float sanitize (float x) noexcept
    {
        return std::isfinite (x) ? x : 0.0f;
    }

    inline float depthFor (float excess, float amount) noexcept
    {
        if (excess <= 0.0f || amount <= 0.0f)
            return 0.0f;
        return kMaxDepthDb * amount * (1.0f - std::exp (-excess / kDepthScale));
    }

    inline void smoothTo (float& v, float target, float aUp, float aDown, float snap) noexcept
    {
        const float a = std::abs (target) > std::abs (v) ? aUp : aDown;
        v += a * (target - v);
        if (std::abs (target - v) < snap)
            v = target;
    }
}

FrictionEngine::FrictionEngine() = default;

void FrictionEngine::configureAnalysis (Analysis& a, int N, int H)
{
    a.N = N;
    a.H = H;
    a.specT.prepare (N, fs);
    a.specR.prepare (N, fs);
    a.trT.prepare (kMaxSlots);
    a.trR.prepare (kMaxSlots);
    a.slots.assign ((size_t) kMaxSlots, SlotState());
    a.frameT.assign ((size_t) N, 0.0f);
    a.frameR.assign ((size_t) N, 0.0f);
    a.gridLo.resize (DisplayGrid::kPoints);
    a.gridHi.resize (DisplayGrid::kPoints);
    const double binHz = fs / N;
    const int maxBin = N / 2;
    for (int g = 0; g < DisplayGrid::kPoints; ++g)
    {
        const double fLo = DisplayGrid::kMinHz * std::pow (DisplayGrid::kMaxHz / DisplayGrid::kMinHz, (g - 0.5) / (DisplayGrid::kPoints - 1));
        const double fHi = DisplayGrid::kMinHz * std::pow (DisplayGrid::kMaxHz / DisplayGrid::kMinHz, (g + 0.5) / (DisplayGrid::kPoints - 1));
        int lo = std::clamp ((int) std::floor (fLo / binHz), 1, maxBin);
        int hi = std::clamp ((int) std::ceil (fHi / binHz), 1, maxBin);
        if (hi < lo)
            hi = lo;
        a.gridLo[(size_t) g] = lo;
        a.gridHi[(size_t) g] = hi;
    }
}

void FrictionEngine::prepare (double sampleRate, int maxBlockSize, int numTargetChannels)
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    nCh = std::clamp (numTargetChannels, 1, kMaxChannels);

    const int k = std::clamp ((int) std::lround (std::log2 (fs / 48000.0)), -2, 3);
    mixN = k >= 0 ? (4096 << k) : (4096 >> -k);
    trackN = mixN / 2;
    mixH = mixN / 4;
    trackH = trackN / 2; // Track mode only analyses (no overlap-add), so half overlap is enough

    ringSize = nextPow2 (2 * mixN + 4096);
    ringMask = ringSize - 1;
    for (auto& r : inRing)
        r.assign ((size_t) ringSize, 0.0f);
    midT.assign ((size_t) ringSize, 0.0f);
    midR.assign ((size_t) ringSize, 0.0f);
    hpCoeff = 1.0 - kTwoPi * 10.0 / fs;

    configureAnalysis (mixAna, mixN, mixH);
    configureAnalysis (trackAna, trackN, trackH);

    lfT.prepare (fs);
    lfR.prepare (fs);
    onsets.prepare (fs);

    procFft.prepare (mixN);
    anaWin.resize ((size_t) mixN);
    synWin.resize ((size_t) mixN);
    procFrame.assign ((size_t) mixN, 0.0f);
    for (int i = 0; i < mixN; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / mixN);
        anaWin[(size_t) i] = (float) w;
        synWin[(size_t) i] = (float) (w / 1.5); // Hann x Hann at 4x overlap sums to 1.5
    }
    for (auto& s : spec)
        s.assign ((size_t) (mixN / 2 + 1), cfloat());
    const int olaSize = nextPow2 (2 * mixN);
    for (auto& o : ola)
        o.assign ((size_t) olaSize, 0.0f);
    olaMask = olaSize - 1;
    gainDb.assign ((size_t) (mixN / 2 + 1), 0.0f);

    lfBells.prepare (fs, kLfHarmonics, nCh);
    trackBells.prepare (fs, kTrackHfBells + kLfHarmonics, nCh);
    bellOwner.assign ((size_t) kTrackHfBells, -1);

    whole.prepare (fs, mixN, nCh);
    pv.prepare (mixN, mixH, fs, kMaxSlots);
    synth.prepare (fs, kMaxSynthVoices);

    advisorQ.prepare (1024);

    paramAlpha = onePoleCoeff (0.010, fs);
    fadeStep = (float) (1.0 / (0.010 * fs));
    preMaskStep = (float) (1.0 / (0.002 * fs));

    reset();
}

void FrictionEngine::reset()
{
    t = 0;
    for (auto& r : inRing)
        std::fill (r.begin(), r.end(), 0.0f);
    std::fill (midT.begin(), midT.end(), 0.0f);
    std::fill (midR.begin(), midR.end(), 0.0f);
    hpT_x1 = hpT_y1 = hpR_x1 = hpR_y1 = 0.0;

    for (Analysis* a : { &mixAna, &trackAna })
    {
        a->trT.reset();
        a->trR.reset();
        for (auto& s : a->slots)
            s = SlotState();
    }
    lfT.reset();
    lfR.reset();
    onsets.reset();
    lfPending = false;

    for (auto& o : ola)
        std::fill (o.begin(), o.end(), 0.0f);
    lastNonIdentityTrigger = INT64_MIN / 2;
    std::fill (gainDb.begin(), gainDb.end(), 0.0f);

    lfBells.reset();
    trackBells.reset();
    std::fill (bellOwner.begin(), bellOwner.end(), -1);
    for (auto& s : lfSlots)
        s = SlotState();

    activeLatency = params.latencyMode;
    activeMode = params.mode;
    activeRetuneStyle = params.retuneStyle;
    activeTensionStyle = params.tensionStyle;

    whole.reset();
    whole.setLatency (currentLatency());
    pv.reset();
    synth.reset();
    nPvReq = 0;
    frameIsTransient = false;
    wholeTargetCents = wholeCents = wholeDecided = wholeDecidedCost = 0.0f;
    lfSched.clear();
    synthSched.clear();
    centsSched.clear();

    mixSm = params.mix;
    trimSm = dbToGain (params.trimDb);
    deltaSm = params.delta ? 1.0f : 0.0f;
    bypassSm = params.bypass ? 1.0f : 0.0f;
    wetFade = 1.0f;
    outFade = 1.0f;
    preMask = preMaskTarget = 1.0f;
    modeTransition = latencyTransition = false;

    nCurT = nCurR = nPv = 0;
    totalCross = totalSelfF = 0.0f;
    worstCross = worstSelf = Clash();
    lastLfT = lastLfR = LfEstimate();
    for (int k = 0; k < kLfHarmonics; ++k)
        lfCorrT[k] = lfCorrR[k] = 0.0f;
    lfCorrF0T = lfCorrF0R = 0.0f;
    selfMode = false;
    scStatus = SidechainStatus::Absent;
    refEnergySmooth = 0.0;
    refLastActive = INT64_MIN / 2;
    wasLearning = false;
    lastProbe = Probe();
    frameCounter = 0;
}

bool FrictionEngine::eligible (float freq, bool lowBand) const noexcept
{
    const float lo = std::min (params.rangeLowHz, params.rangeHighHz);
    const float hi = std::max (params.rangeLowHz, params.rangeHighHz);
    if (freq < lo || freq > hi)
        return false;
    switch (params.focus)
    {
        case Focus::Low:  return lowBand;
        case Focus::High: return ! lowBand;
        case Focus::Both: default: return true;
    }
}

void FrictionEngine::gatherFrame (const std::vector<float>& ring, std::vector<float>& out, int N) const noexcept
{
    const std::int64_t start = t - N;
    for (int i = 0; i < N; ++i)
        out[(size_t) i] = ring[(size_t) ((start + i) & ringMask)];
}

void FrictionEngine::resetModeState() noexcept
{
    pv.reset();
    synth.reset();
    synthSched.clear();
    centsSched.clear();
    whole.realign();
    wholeCents = wholeTargetCents = wholeDecided = wholeDecidedCost = 0.0f;
    for (Analysis* a : { &mixAna, &trackAna })
        for (auto& s : a->slots)
        {
            s.shiftCents = 0.0f;
            s.targetCents = 0.0f;
            s.depthDb = 0.0f;
        }
    for (auto& s : lfSlots)
        s.depthDb = 0.0f;
}

void FrictionEngine::applyTransitionIfIdle() noexcept
{
    if (latencyTransition && outFade <= 0.0f)
    {
        activeLatency = params.latencyMode;
        activeMode = params.mode;
        activeRetuneStyle = params.retuneStyle;
        activeTensionStyle = params.tensionStyle;
        for (Analysis* a : { &mixAna, &trackAna })
        {
            a->trT.reset();
            a->trR.reset();
            for (auto& s : a->slots)
                s = SlotState();
        }
        for (auto& o : ola)
            std::fill (o.begin(), o.end(), 0.0f);
        lastNonIdentityTrigger = INT64_MIN / 2;
        lfBells.reset();
        trackBells.reset();
        std::fill (bellOwner.begin(), bellOwner.end(), -1);
        for (auto& s : lfSlots)
            s = SlotState();
        lfSched.clear();
        whole.setLatency (currentLatency());
        resetModeState();
        latencyTransition = false;
        modeTransition = false;
    }
    if (modeTransition && wetFade <= 0.0f)
    {
        activeMode = params.mode;
        activeRetuneStyle = params.retuneStyle;
        activeTensionStyle = params.tensionStyle;
        resetModeState();
        modeTransition = false;
    }
}

// ============================================================================ process

void FrictionEngine::process (float* const* io, int numCh, const float* const* sc, int numScCh, int numSamples,
                              bool sidechainPresent) noexcept
{
    ScopedFlushDenormals ftz;
    if (numSamples <= 0 || io == nullptr || numCh <= 0)
        return;

    const int ch = std::min (numCh, nCh);
    sidechainPresentNow = sidechainPresent && sc != nullptr && numScCh > 0;

    // requested structural changes
    if (params.latencyMode != activeLatency)
        latencyTransition = true;
    {
        const Mode wantMode = params.mode;
        const bool styleChanged = (wantMode == Mode::Retune && params.retuneStyle != activeRetuneStyle)
                                  || (wantMode == Mode::Tension && params.tensionStyle != activeTensionStyle);
        if (! latencyTransition && (wantMode != activeMode || styleChanged))
            modeTransition = true;
    }

    const bool mix = activeLatency == LatencyMode::Mix;
    const float targetMix = clampf (params.mix, 0.0f, 1.0f);
    const float targetTrim = dbToGain (params.trimDb);
    const float targetDelta = params.delta ? 1.0f : 0.0f;
    const float targetBypass = params.bypass ? 1.0f : 0.0f;

    int i = 0;
    while (i < numSamples)
    {
        const int phase = (int) (t % kControlTick);
        const int n = std::min (numSamples - i, kControlTick - phase);
        if (phase == 0)
            controlTick();

        const bool useWhole = mix && activeMode == Mode::Retune && activeRetuneStyle != RetuneStyle::PerPartial;

        for (int j = 0; j < n; ++j)
        {
            const int s = i + j;
            float x[kMaxChannels];
            for (int c = 0; c < ch; ++c)
                x[c] = sanitize (io[c][s]);
            for (int c = ch; c < kMaxChannels; ++c)
                x[c] = x[0];

            const float mt = ch >= 2 ? 0.5f * (x[0] + x[1]) : x[0];
            float r = 0.0f;
            if (sidechainPresentNow)
            {
                const float r0 = sc[0] != nullptr ? sanitize (sc[0][s]) : 0.0f;
                const float r1 = (numScCh >= 2 && sc[1] != nullptr) ? sanitize (sc[1][s]) : r0;
                r = 0.5f * (r0 + r1);
            }

            const size_t idx = (size_t) (t & ringMask);
            for (int c = 0; c < kMaxChannels; ++c)
                inRing[c][idx] = x[c];

            const double yT = (double) mt - hpT_x1 + hpCoeff * hpT_y1;
            hpT_x1 = mt;
            hpT_y1 = std::abs (yT) < 1.0e-30 ? 0.0 : yT;
            const double yR = (double) r - hpR_x1 + hpCoeff * hpR_y1;
            hpR_x1 = r;
            hpR_y1 = std::abs (yR) < 1.0e-30 ? 0.0 : yR;
            midT[idx] = (float) yT;
            midR[idx] = (float) yR;

            const bool a = lfT.push (mt);
            const bool b = lfR.push (r);
            if (a || b)
                lfPending = true;
            onsets.push (mt);
            whole.push (x, ch, mt);

            float dry[kMaxChannels], wet[kMaxChannels];
            if (mix)
            {
                const size_t didx = (size_t) ((t - mixN) & ringMask);
                for (int c = 0; c < ch; ++c)
                    dry[c] = inRing[c][didx];

                if (useWhole)
                {
                    whole.render (wet, ch);
                    for (int c = 0; c < ch; ++c)
                        ola[c][(size_t) (t & olaMask)] = 0.0f;
                }
                else
                {
                    const bool idle = lastNonIdentityTrigger <= t - mixN;
                    // pre-echo guard: just before a detected onset the spectral processing would smear the
                    // transient's removed component backwards in time, so the time-aligned dry is used there
                    preMask += preMask < preMaskTarget ? std::min (preMaskStep, preMaskTarget - preMask)
                                                       : -std::min (preMaskStep, preMask - preMaskTarget);
                    const float pm = 0.5f - 0.5f * std::cos ((float) kPi * preMask);
                    for (int c = 0; c < ch; ++c)
                    {
                        float& o = ola[c][(size_t) (t & olaMask)];
                        wet[c] = idle ? dry[c] : (pm >= 1.0f ? o : dry[c] + pm * (o - dry[c]));
                        o = 0.0f;
                    }
                }
            }
            else
            {
                for (int c = 0; c < ch; ++c)
                    dry[c] = wet[c] = x[c];
            }

            for (int c = 0; c < ch; ++c)
            {
                wetBuf[c][j] = wet[c];
                dryBuf[c][j] = dry[c];
            }
            ++t;
        }

        // chunk post-processing on the wet signal
        float* wetPtr[kMaxChannels] = { wetBuf[0], wetBuf[1] };
        if (mix)
        {
            lfBells.process (wetPtr, ch, 0, n);
            if (activeMode == Mode::Tension && ! synth.isSilent())
            {
                std::fill (synthBuf, synthBuf + n, 0.0f);
                synth.render (synthBuf, n);
                for (int c = 0; c < ch; ++c)
                    for (int j = 0; j < n; ++j)
                        wetBuf[c][j] += synthBuf[j];
            }
        }
        else
        {
            trackBells.process (wetPtr, ch, 0, n);
        }

        // output mixing
        for (int j = 0; j < n; ++j)
        {
            mixSm += paramAlpha * (targetMix - mixSm);
            if (std::abs (targetMix - mixSm) < 1.0e-6f) mixSm = targetMix;
            trimSm += paramAlpha * (targetTrim - trimSm);
            if (std::abs (targetTrim - trimSm) < 1.0e-7f) trimSm = targetTrim;
            deltaSm += paramAlpha * (targetDelta - deltaSm);
            if (std::abs (targetDelta - deltaSm) < 1.0e-6f) deltaSm = targetDelta;
            bypassSm += paramAlpha * (targetBypass - bypassSm);
            if (std::abs (targetBypass - bypassSm) < 1.0e-6f) bypassSm = targetBypass;

            wetFade = modeTransition ? std::max (0.0f, wetFade - fadeStep) : std::min (1.0f, wetFade + fadeStep);
            outFade = latencyTransition ? std::max (0.0f, outFade - fadeStep) : std::min (1.0f, outFade + fadeStep);

            // raised-cosine shaping of the transition fades (less splatter than linear ramps)
            const float wetShape = wetFade >= 1.0f ? 1.0f : 0.5f - 0.5f * std::cos ((float) kPi * wetFade);
            const float outShape = outFade >= 1.0f ? 1.0f : 0.5f - 0.5f * std::cos ((float) kPi * outFade);
            const float mixEff = mixSm * wetShape * (1.0f - bypassSm);
            const float dryKeep = 1.0f - deltaSm * (1.0f - bypassSm);
            for (int c = 0; c < ch; ++c)
            {
                const float dry = dryBuf[c][j];
                const float wet = wetBuf[c][j];
                float y = dryKeep * dry + mixEff * (wet - dry);
                if (trimSm != 1.0f)
                    y *= trimSm;
                if (outFade != 1.0f)
                    y *= outShape;
                io[c][i + j] = std::isfinite (y) ? y : 0.0f;
            }
        }

        i += n;

        if (t % kControlTick == 0)
        {
            if (lfPending)
            {
                onLfHop();
                lfPending = false;
            }
            const Analysis& a = activeLatency == LatencyMode::Mix ? mixAna : trackAna;
            if (t % a.H == 0)
                onFrame();
        }
        applyTransitionIfIdle();
    }
}

// ============================================================================ control tick

void FrictionEngine::controlTick() noexcept
{
    const std::int64_t now = t;
    if (activeLatency == LatencyMode::Mix)
    {
        const std::int64_t L = mixN;
        while (const LfRecord* r = lfSched.peek())
        {
            if (r->time + L > now)
                break;
            for (int k = 0; k < kLfHarmonics; ++k)
                lfBells.setTarget (k, r->f[k], r->g[k], r->bw[k]);
            lfSched.pop();
        }
        while (const SynthRecord* r = synthSched.peek())
        {
            if (r->time + L > now)
                break;
            synth.setTargets (r->v, r->n, mixH, (int) (0.05 * fs));
            synthSched.pop();
        }
        while (const CentsRecord* r = centsSched.peek())
        {
            if (r->time + L > now)
                break;
            wholeTargetCents = r->cents;
            centsSched.pop();
        }

        // pre-echo guard target for the samples of this tick (input time t - L)
        {
            const std::int64_t inTime = now - L;
            const std::int64_t u = onsets.nextOnsetAfter (inTime - 1);
            const bool preOnset = u != INT64_MAX && (double) (u - inTime) <= preEchoSec * fs;
            preMaskTarget = preOnset ? 0.0f : 1.0f;
        }

        const double tickRate = fs / kControlTick;
        const float aUp = onePoleCoeff (params.attackMs * 0.001, tickRate);
        const float aDown = onePoleCoeff (params.releaseMs * 0.001, tickRate);
        smoothTo (wholeCents, wholeTargetCents, aUp, aDown, 0.002f);
        whole.setRatio (wholeCents == 0.0f ? 1.0 : centsToRatio (wholeCents));
        if (activeMode == Mode::Retune && activeRetuneStyle != RetuneStyle::PerPartial)
            whole.controlTick (t, onsets);
        lfBells.tick();
    }
    else
    {
        trackBells.tick();
    }
}

// ============================================================================ low-band hop

void FrictionEngine::onLfHop() noexcept
{
    const LfEstimate eT = lfT.latest();
    const LfEstimate eR = lfR.latest();
    lastLfT = eT;
    lastLfR = eR;

    const float xo = params.crossoverHz;
    const float threshold = sensitivityToThreshold (params.sensitivity);

    Partial lfp[kLfHarmonics];
    int nl = 0;
    if (eT.voiced)
    {
        float maxA = 0.0f;
        for (float h : eT.harm)
            maxA = std::max (maxA, h);
        for (int k = 0; k < kLfHarmonics; ++k)
        {
            const float f = (float) (k + 1) * eT.f0 * lfCorrFor (lfCorrT, lfCorrF0T, eT.f0, k);
            if (f >= xo || f < 20.0f)
                continue;
            if (eT.harm[k] < std::max (kAudibleFloor, maxA * 0.01f))
                continue;
            Partial& p = lfp[nl++];
            p.freq = f;
            p.amp = eT.harm[k];
            p.slot = kLfSlotBase + k;
            p.src = 1;
        }
    }

    if (nl > 0)
    {
        if (selfMode)
        {
            // weaker-partial attribution against all target partials (LF harmonics + last HF partials)
            Partial all[kMaxPartials];
            int na = 0;
            for (int i = 0; i < nl && na < kMaxPartials; ++i)
                all[na++] = lfp[i];
            for (int i = 0; i < nCurT && na < kMaxPartials; ++i)
                if (curT[i].src == 0)
                    all[na++] = curT[i];
            computeSelfFriction (all, na, nullptr);
            for (int i = 0; i < nl; ++i)
                lfp[i].self = all[i].self;
        }
        else
        {
            Partial ref[kMaxPartials];
            int nr = 0;
            if (eR.voiced && sidechainPresentNow)
            {
                float maxA = 0.0f;
                for (float h : eR.harm)
                    maxA = std::max (maxA, h);
                for (int k = 0; k < kLfHarmonics && nr < kMaxPartials; ++k)
                {
                    const float f = (float) (k + 1) * eR.f0 * lfCorrFor (lfCorrR, lfCorrF0R, eR.f0, k);
                    if (f >= xo || eR.harm[k] < std::max (kAudibleFloor, maxA * 0.01f))
                        continue;
                    ref[nr].freq = f;
                    ref[nr].amp = eR.harm[k];
                    ++nr;
                }
            }
            for (int i = 0; i < nCurR && nr < kMaxPartials; ++i)
                if (curR[i].src == 0 && (! eR.voiced || curR[i].freq >= xo))
                    ref[nr++] = curR[i];
            if (nr > 0)
                computeCrossFriction (lfp, nl, ref, nr, nullptr);
        }
    }

    const double hopRate = fs / lfT.hopSamples();
    const float aRise = onePoleCoeff (0.030, hopRate), aFall = onePoleCoeff (0.120, hopRate);
    const float aAtt = onePoleCoeff (params.attackMs * 0.001, hopRate);
    const float aRel = onePoleCoeff (params.releaseMs * 0.001, hopRate);
    const bool smoothActive = activeMode == Mode::Smooth || activeLatency == LatencyMode::Track;

    LfRecord rec;
    rec.time = eT.time;
    for (int k = 0; k < kLfHarmonics; ++k)
    {
        SlotState& st = lfSlots[k];
        const Partial* p = nullptr;
        for (int i = 0; i < nl; ++i)
            if (lfp[i].slot == kLfSlotBase + k)
                p = &lfp[i];

        float targetDepth = 0.0f;
        if (p != nullptr)
        {
            const float F = selfMode ? p->self : p->friction;
            if (! st.seen)
                st.fSmooth = F;
            else
                st.fSmooth += (F > st.fSmooth ? aRise : aFall) * (F - st.fSmooth);
            st.seen = true;
            const float tEff = st.active ? 0.75f * threshold : threshold;
            st.active = st.fSmooth > tEff;
            if (smoothActive && st.active && eligible (p->freq, true))
                targetDepth = depthFor (st.fSmooth - tEff, params.amount);
            st.lastFreq = p->freq;
        }
        else
        {
            st.fSmooth += aFall * (0.0f - st.fSmooth);
            st.active = false;
            st.seen = false;
        }
        smoothTo (st.depthDb, targetDepth, aAtt, aRel, 0.01f);

        const float f = st.lastFreq > 0.0f ? st.lastFreq : 50.0f;
        rec.f[k] = f;
        rec.g[k] = -st.depthDb;
        rec.bw[k] = clampf (0.1f * f, 3.0f, 20.0f);
    }

    if (activeLatency == LatencyMode::Mix)
    {
        lfSched.push (rec);
    }
    else
    {
        for (int k = 0; k < kLfHarmonics; ++k)
            trackBells.setTarget (kTrackHfBells + k, rec.f[k], rec.g[k], rec.bw[k]);
    }
}

// ============================================================================ analysis frame

void FrictionEngine::onFrame() noexcept
{
    Analysis& a = activeLatency == LatencyMode::Mix ? mixAna : trackAna;
    const int N = a.N;
    const std::int64_t centre = t - N / 2;
    ++frameCounter;

    gatherFrame (midT, a.frameT, N);
    gatherFrame (midR, a.frameR, N);

    const float minHz = 20.0f;
    const float maxHz = (float) std::min (20000.0, 0.45 * fs);

    // sidechain status
    double eR = 0.0;
    for (int i = 0; i < N; ++i)
        eR += (double) a.frameR[(size_t) i] * a.frameR[(size_t) i];
    eR /= N;
    refEnergySmooth = eR;
    if (sidechainPresentNow && eR > 1.0e-9)
        refLastActive = t;
    if (! sidechainPresentNow)
        scStatus = SidechainStatus::Absent;
    else
        scStatus = (t - refLastActive) > (std::int64_t) (0.5 * fs) ? SidechainStatus::Silent : SidechainStatus::Active;
    selfMode = params.reference == ReferenceSource::Self || ! sidechainPresentNow;

    const int nT = a.specT.analyze (a.frameT.data(), minHz, maxHz);
    const int nR = sidechainPresentNow ? a.specR.analyze (a.frameR.data(), minHz, maxHz) : 0;
    const SpectralPeak* pkT = a.specT.peaks();
    const SpectralPeak* pkR = a.specR.peaks();

    LfEstimate eT, eRf;
    const std::int64_t maxDist = 2 * lfT.hopSamples();
    const bool hasT = lfT.lookup (centre, eT, maxDist);
    const bool hasR = sidechainPresentNow && lfR.lookup (centre, eRf, maxDist);
    const bool lfVT = hasT && eT.voiced;
    const bool lfVR = hasR && eRf.voiced;
    const float xo = params.crossoverHz;

    // tracking
    float fr[kMaxPeaks], am[kMaxPeaks];
    int slotT[kMaxPeaks], slotR[kMaxPeaks];
    for (int p = 0; p < nT; ++p) { fr[p] = pkT[p].freq; am[p] = pkT[p].amp; }
    a.trT.update (fr, am, nT, slotT, a.specT.binHz());
    for (int p = 0; p < nT; ++p)
    {
        const int s = slotT[p];
        if (s >= 0 && a.trT.isNew (s))
        {
            a.slots[(size_t) s] = SlotState();
            if (&a == &mixAna)
                pv.resetSlot (s);
        }
    }
    for (int p = 0; p < nR; ++p) { fr[p] = pkR[p].freq; am[p] = pkR[p].amp; }
    a.trR.update (fr, am, nR, slotR, a.specR.binHz());

    // ---- merged partial lists
    auto addLf = [&] (const LfEstimate& e, Partial* list, int& n)
    {
        float maxA = 0.0f;
        for (float h : e.harm)
            maxA = std::max (maxA, h);
        for (int k = 0; k < kLfHarmonics && n < kMaxPartials; ++k)
        {
            const float f = (float) (k + 1) * e.f0;
            if (f >= xo || f < 20.0f || e.harm[k] < std::max (kAudibleFloor, maxA * 0.01f))
                continue;
            Partial& q = list[n++];
            q = Partial();
            q.freq = f;
            q.amp = e.harm[k];
            q.slot = kLfSlotBase + k;
            q.src = 1;
        }
    };

    float maxAmpT = 0.0f, maxAmpR = 0.0f;
    for (int p = 0; p < nT; ++p) maxAmpT = std::max (maxAmpT, pkT[p].amp);
    for (int p = 0; p < nR; ++p) maxAmpR = std::max (maxAmpR, pkR[p].amp);

    // Cross-check: where a stationary STFT peak lies within one bin of a low-band harmonic but
    // disagrees, the STFT frequency is used (YIN can lock to a subharmonic of a stronger tone above
    // the crossover). Only at Mix-mode resolution; non-stationary peaks (kick drops) keep YIN.
    auto correctLf = [&] (Partial* list, int n, const SpectralPeak* pk, int np, double binHz)
    {
        if (binHz > 15.0)
            return;
        for (int i = 0; i < n; ++i)
        {
            Partial& q = list[i];
            if (q.src != 1)
                continue;
            int best = -1;
            float bestD = (float) binHz;
            for (int p = 0; p < np; ++p)
            {
                if (! pk[p].stationary || pk[p].freq > 1.5f * xo)
                    continue;
                const float d = std::abs (pk[p].freq - q.freq);
                if (d < bestD) { bestD = d; best = p; }
            }
            if (best >= 0 && bestD > std::max (0.3f, 0.15f * (float) binHz))
            {
                q.freq = pk[best].freq;
                q.amp = pk[best].amp;
            }
        }
    };

    nCurT = 0;
    if (lfVT)
    {
        addLf (eT, curT, nCurT);
        correctLf (curT, nCurT, pkT, nT, a.specT.binHz());
    }
    for (int p = 0; p < nT && nCurT < kMaxPartials; ++p)
    {
        if (lfVT && pkT[p].freq < xo)
            continue;
        if (pkT[p].amp < kAudibleFloor || pkT[p].amp < maxAmpT * kRelFloor)
            continue;
        Partial& q = curT[nCurT++];
        q = Partial();
        q.freq = pkT[p].freq;
        q.amp = pkT[p].amp;
        q.bin = pkT[p].bin;
        q.lo = (float) pkT[p].lo;
        q.hi = (float) pkT[p].hi;
        q.slot = slotT[p];
        q.src = 0;
    }

    nCurR = 0;
    if (lfVR)
    {
        addLf (eRf, curR, nCurR);
        correctLf (curR, nCurR, pkR, nR, a.specR.binHz());
    }
    for (int p = 0; p < nR && nCurR < kMaxPartials; ++p)
    {
        if (lfVR && pkR[p].freq < xo)
            continue;
        if (pkR[p].amp < kAudibleFloor || pkR[p].amp < maxAmpR * kRelFloor)
            continue;
        Partial& q = curR[nCurR++];
        q = Partial();
        q.freq = pkR[p].freq;
        q.amp = pkR[p].amp;
        q.bin = pkR[p].bin;
        q.slot = slotR[p];
        q.src = 0;
    }

    // remember corrected low-band frequencies for the LF hops until the next frame
    for (int k = 0; k < kLfHarmonics; ++k)
        lfCorrT[k] = lfCorrR[k] = 0.0f;
    lfCorrF0T = lfVT ? eT.f0 : 0.0f;
    lfCorrF0R = lfVR ? eRf.f0 : 0.0f;
    for (int i = 0; i < nCurT; ++i)
        if (curT[i].src == 1)
        {
            const int k = curT[i].slot - kLfSlotBase;
            if (k >= 0 && k < kLfHarmonics && lfVT && std::abs (curT[i].freq - (float) (k + 1) * eT.f0) > 0.01f)
                lfCorrT[k] = curT[i].freq / ((float) (k + 1) * eT.f0);
        }
    for (int i = 0; i < nCurR; ++i)
        if (curR[i].src == 1)
        {
            const int k = curR[i].slot - kLfSlotBase;
            if (k >= 0 && k < kLfHarmonics && lfVR && std::abs (curR[i].freq - (float) (k + 1) * eRf.f0) > 0.01f)
                lfCorrR[k] = curR[i].freq / ((float) (k + 1) * eRf.f0);
        }

    worstCross = Clash();
    worstSelf = Clash();
    if (nCurR > 0)
        totalCross = computeCrossFriction (curT, nCurT, curR, nCurR, &worstCross);
    else
    {
        totalCross = 0.0f;
        for (int i = 0; i < nCurT; ++i)
            curT[i].friction = 0.0f;
    }
    totalSelfF = computeSelfFriction (curT, nCurT, &worstSelf);

    // transient protection: any onset inside the well-weighted part of the window (7/8 .. 1/8)
    frameIsTransient = onsets.anyOnsetIn (t - (7 * N) / 8, t - N / 8);
    const float threshold = sensitivityToThreshold (params.sensitivity);

    if (activeLatency == LatencyMode::Mix)
    {
        const bool smoothActive = activeMode == Mode::Smooth;
        const bool pvMode = (activeMode == Mode::Retune && activeRetuneStyle == RetuneStyle::PerPartial)
                            || (activeMode == Mode::Tension && activeTensionStyle == TensionStyle::Push);

        decideSmooth (a, threshold);

        // per-partial list: every STFT peak of the target
        nPv = 0;
        for (int p = 0; p < nT && nPv < kMaxPartials; ++p)
        {
            if (pkT[p].amp < kAudibleFloor || pkT[p].amp < maxAmpT * kRelFloor || slotT[p] < 0)
                continue;
            Partial& q = pvList[nPv];
            q = Partial();
            q.freq = pkT[p].freq;
            q.amp = pkT[p].amp;
            q.bin = pkT[p].bin;
            q.slot = slotT[p];
            pvPeakIndex[nPv] = p;
            ++nPv;
        }
        bool pvState = pvMode;
        for (int i = 0; i < nPv && ! pvState; ++i)
            pvState = a.slots[(size_t) pvList[i].slot].shiftCents != 0.0f || pv.slotHasPhase (pvList[i].slot);
        nPvReq = 0;
        if (pvState)
        {
            if (selfMode)
                computeSelfFriction (pvList, nPv, nullptr);
            else if (nCurR > 0)
                computeCrossFriction (pvList, nPv, curR, nCurR, nullptr);
            decidePerPartial (a, threshold, activeMode == Mode::Tension);
        }
        if (! pvMode)
        {
            // shifts are releasing; keep the requests so phases stay continuous, or none at all
        }

        if (activeMode == Mode::Retune && activeRetuneStyle != RetuneStyle::PerPartial)
            decideWholeAuto (threshold);
        else
            centsSched.push ({ centre, 0.0f });

        if (activeMode == Mode::Tension && activeTensionStyle == TensionStyle::Synth)
            decideTensionSynth (a);
        else if (! synth.isSilent())
        {
            SynthRecord rec;
            rec.time = centre;
            rec.n = 0;
            synthSched.push (rec);
        }

        processMixSpectral (a, smoothActive, nPvReq > 0);
    }
    else
    {
        decideSmooth (a, threshold);
        updateTrackBells (a);
    }

    publishDisplay (a);
    pushAdvisorFrame (a);

    // probe for tests / introspection
    Probe& pr = lastProbe;
    pr.nT = nCurT;
    pr.nR = nCurR;
    std::copy (curT, curT + nCurT, pr.T);
    std::copy (curR, curR + nCurR, pr.R);
    pr.totalFriction = totalCross;
    pr.totalSelf = totalSelfF;
    pr.worst = selfMode ? worstSelf : worstCross;
    pr.lfTVoiced = lfVT;
    pr.lfRVoiced = lfVR;
    pr.lfTF0 = hasT ? eT.f0 : 0.0f;
    pr.lfRF0 = hasR ? eRf.f0 : 0.0f;
    pr.wholeCents = wholeCents;
    pr.wholeTargetCents = wholeDecided;
    float md = 0.0f;
    for (const auto& s : a.slots)
        md = std::max (md, s.depthDb);
    for (const auto& s : lfSlots)
        md = std::max (md, s.depthDb);
    pr.maxDepthDb = md;
    pr.pvRequests = nPvReq;
    pr.synthVoices = synth.activeVoices();
    pr.selfMode = selfMode;
    pr.sidechain = scStatus;
    pr.frameTime = t;
    pr.frames = frameCounter;
    pr.splices = whole.spliceCount();
}

void FrictionEngine::decideSmooth (Analysis& a, float threshold) noexcept
{
    const double frameRate = fs / a.H;
    const float aRise = onePoleCoeff (0.030, frameRate), aFall = onePoleCoeff (0.120, frameRate);
    const float aAtt = onePoleCoeff (params.attackMs * 0.001, frameRate);
    const float aRel = onePoleCoeff (params.releaseMs * 0.001, frameRate);
    const bool smoothOn = activeMode == Mode::Smooth || activeLatency == LatencyMode::Track;
    const float xo = params.crossoverHz;

    for (auto& s : a.slots)
        s.seen = false;

    for (int i = 0; i < nCurT; ++i)
    {
        const Partial& p = curT[i];
        if (p.src != 0 || p.slot < 0)
            continue;
        SlotState& st = a.slots[(size_t) p.slot];
        const float F = frictionOf (p);
        if (a.trT.isNew (p.slot) || st.lastFreq <= 0.0f)
            st.fSmooth = F;
        else
            st.fSmooth += (F > st.fSmooth ? aRise : aFall) * (F - st.fSmooth);
        st.seen = true;
        const float tEff = st.active ? 0.75f * threshold : threshold;
        st.active = st.fSmooth > tEff;
        float target = 0.0f;
        if (smoothOn && st.active && eligible (p.freq, p.freq < xo))
            target = depthFor (st.fSmooth - tEff, params.amount);
        smoothTo (st.depthDb, target, aAtt, aRel, 0.01f);
        st.lastFreq = p.freq;
        st.lastAmp = p.amp;
        st.loBin = p.lo;
        st.hiBin = p.hi;
    }

    // slots that are coasting (missed this frame) keep their dip at the last frequency but release;
    // dead slots release to zero
    for (int s = 0; s < (int) a.slots.size(); ++s)
    {
        SlotState& st = a.slots[(size_t) s];
        if (st.seen || st.depthDb == 0.0f)
            continue;
        smoothTo (st.depthDb, 0.0f, aAtt, aRel, 0.01f);
        if (! a.trT.isUsed (s))
            st.depthDb = 0.0f;
    }
}

void FrictionEngine::decidePerPartial (Analysis& a, float threshold, bool tension) noexcept
{
    const double frameRate = fs / a.H;
    const float aAtt = onePoleCoeff (params.attackMs * 0.001, frameRate);
    const float aRel = onePoleCoeff (params.releaseMs * 0.001, frameRate);
    const bool modeOn = (activeMode == Mode::Retune && activeRetuneStyle == RetuneStyle::PerPartial)
                        || (activeMode == Mode::Tension && activeTensionStyle == TensionStyle::Push);
    const float maxC = clampf (params.maxCents, 1.0f, 100.0f);
    const float xo = params.crossoverHz;
    const float beta = 0.04f * std::pow (25.0f, clampf (params.beatRate, 0.0f, 1.0f));
    const SpectralPeak* pk = a.specT.peaks();

    for (int i = 0; i < nPv; ++i)
    {
        const Partial& q = pvList[i];
        SlotState& st = a.slots[(size_t) q.slot];
        float desired = 0.0f;

        if (modeOn && eligible (q.freq, q.freq < xo))
        {
            // local reference set: partials that can matter for q anywhere in +-maxC (x < 1.5, zHat > 3 %)
            const Partial* src = selfMode ? pvList : curR;
            const int nSrc = selfMode ? nPv : nCurR;
            Partial loc[kMaxPartials];
            float locL[kMaxPartials], locS[kMaxPartials];
            int nl = 0;
            const float reachLo = q.freq * (float) centsToRatio (-maxC);
            const float reachHi = q.freq * (float) centsToRatio (maxC);
            const float win = 1.5f / Roughness::sFactor (reachLo);
            for (int j = 0; j < nSrc && nl < kMaxPartials; ++j)
            {
                if (selfMode && (j == i || src[j].amp < q.amp))
                    continue;
                if (src[j].freq > reachLo - win && src[j].freq < reachHi + win)
                {
                    loc[nl] = src[j];
                    locL[nl] = Roughness::level (src[j].amp);
                    locS[nl] = Roughness::sFactor (src[j].freq);
                    ++nl;
                }
            }
            const float lq = Roughness::level (q.amp);
            auto F = [&] (float cents)
            {
                const float f = q.freq * (float) centsToRatio (cents);
                return partialFriction (f, q.amp, lq, loc, locL, nl, nullptr, nullptr, locS);
            };
            const float F0 = F (0.0f);

            if (! tension)
            {
                const float tEff = st.targetCents != 0.0f ? 0.75f * threshold : threshold;
                // full searches are cached: they are repeated only when the partial or its friction
                // changed, at least every 8 frames (shifts still glide every frame)
                const bool unchanged = st.decidedF0 >= 0.0f && std::abs (q.freq - st.decidedFreq) < 0.0005f * q.freq
                                       && std::abs (F0 - st.decidedF0) < 0.1f * st.decidedF0 + 0.005f
                                       && frameCounter - st.decidedFrame < 8;
                if (unchanged)
                {
                    desired = st.targetCents;
                }
                else if (F0 > tEff && nl > 0)
                {
                    auto J = [&] (float c) { return F (c) + 0.1f * F0 * std::abs (c) / maxC; };
                    float bestC = 0.0f, bestJ = J (0.0f);
                    auto tryC = [&] (float c)
                    {
                        if (std::abs (c) > maxC) return;
                        const float v = J (c);
                        if (v < bestJ) { bestJ = v; bestC = c; }
                    };
                    // candidates: unison with each nearby reference partial, plus a coarse grid
                    for (int j = 0; j < nl; ++j)
                        tryC ((float) centsBetween (q.freq, loc[j].freq));
                    const float unisonBest = bestC, unisonJ = bestJ;
                    float gridC = 0.0f, gridJ = J (0.0f);
                    for (float c = -maxC; c <= maxC; c += 5.0f)
                    {
                        const float v = J (c);
                        if (v < gridJ) { gridJ = v; gridC = c; }
                    }
                    if (gridJ < unisonJ)
                    {
                        // optimum away from every reference partial: refine to 1 cent
                        bestC = gridC;
                        bestJ = gridJ;
                        for (float c = gridC - 4.0f; c <= gridC + 4.0f; c += 1.0f)
                            tryC (c);
                    }
                    else
                    {
                        bestC = unisonBest;
                        bestJ = unisonJ;
                    }
                    if (bestJ < 0.5f * F0)
                    {
                        if (st.targetCents != 0.0f)
                        {
                            const float jCur = J (st.targetCents);
                            desired = (bestJ < 0.8f * jCur) ? bestC : st.targetCents;
                        }
                        else
                        {
                            desired = bestC;
                        }
                    }
                    else if (st.targetCents != 0.0f && J (st.targetCents) < 0.5f * F0)
                    {
                        desired = st.targetCents;
                    }
                    st.decidedFreq = q.freq;
                    st.decidedF0 = F0;
                    st.decidedFrame = frameCounter;
                }
                else
                {
                    st.decidedF0 = -1.0f;
                }
            }
            else
            {
                // Tension push: move toward the maximum-roughness spacing next to the nearest reference partial
                int nearest = -1;
                float bestD = 1.0e9f;
                for (int j = 0; j < nl; ++j)
                {
                    const float d = std::abs (loc[j].freq - q.freq);
                    if (d < bestD && d < Roughness::windowHz (std::min (loc[j].freq, q.freq)))
                    {
                        bestD = d;
                        nearest = j;
                    }
                }
                if (nearest >= 0)
                {
                    const float fj = loc[nearest].freq;
                    const float spacing = beta * Roughness::peakDeltaHz (fj);
                    // keep the push direction once chosen; near unison push upward (estimation noise
                    // must not flip the direction from frame to frame)
                    float sign;
                    if (st.targetCents != 0.0f)
                        sign = st.targetCents > 0.0f ? 1.0f : -1.0f;
                    else if (std::abs (q.freq - fj) < 0.25f * spacing)
                        sign = 1.0f;
                    else
                        sign = q.freq >= fj ? 1.0f : -1.0f;
                    const float fTarget = fj + sign * spacing;
                    desired = clampf ((float) centsBetween (q.freq, fTarget), -maxC, maxC);
                }
            }
        }

        st.targetCents = desired;
        const float goal = desired * clampf (params.amount, 0.0f, 1.0f);
        smoothTo (st.shiftCents, goal, aAtt, aRel, 0.01f);

        if (st.shiftCents != 0.0f || pv.slotHasPhase (q.slot))
        {
            if (nPvReq < kMaxPartials)
            {
                ShiftRequest& r = pvReq[nPvReq++];
                r.peakBin = pk[pvPeakIndex[i]].k;
                r.slot = q.slot;
                r.shiftHz = q.freq * ((float) centsToRatio (st.shiftCents) - 1.0f);
            }
        }
    }
}

void FrictionEngine::decideWholeAuto (float threshold) noexcept
{
    const float amount = clampf (params.amount, 0.0f, 1.0f);
    const std::int64_t centre = t - (activeLatency == LatencyMode::Mix ? mixAna.N : trackAna.N) / 2;

    if (activeRetuneStyle == RetuneStyle::WholeFixed)
    {
        wholeDecided = clampf (params.wholeOffsetCents, -1200.0f, 1200.0f);
        centsSched.push ({ centre, wholeDecided * amount });
        return;
    }

    // the global decision is re-evaluated on every other frame (the applied shift glides anyway)
    if ((frameCounter & 1) != 0)
    {
        centsSched.push ({ centre, wholeDecided * amount });
        return;
    }

    float desired = 0.0f;
    if (! selfMode && nCurT > 0 && nCurR > 0)
    {
        const float maxC = clampf (params.maxCents, 1.0f, 100.0f);
        float total0 = 0.0f;
        for (int i = 0; i < nCurT; ++i)
            if (eligible (curT[i].freq, curT[i].freq < params.crossoverHz))
                total0 += curT[i].friction;

        if (total0 > threshold)
        {
            // per target partial: the reference partials it can interact with anywhere in +-maxC
            // the strongest eligible target partials decide the global offset
            int order[kMaxPartials];
            int nOrd = 0;
            for (int i = 0; i < nCurT; ++i)
                if (eligible (curT[i].freq, curT[i].freq < params.crossoverHz))
                    order[nOrd++] = i;
            std::sort (order, order + nOrd, [&] (int x, int y) { return curT[x].amp > curT[y].amp; });
            nOrd = std::min (nOrd, 48);

            int nSeg = 0, pool = 0;
            for (int oi = 0; oi < nOrd; ++oi)
            {
                const Partial& p = curT[order[oi]];
                const float lo = p.freq * (float) centsToRatio (-maxC);
                const float hi = p.freq * (float) centsToRatio (maxC);
                const float win = 1.5f / Roughness::sFactor (lo);
                WholeSeg& sg = wholeSegs[nSeg];
                sg.f = p.freq;
                sg.a = p.amp;
                sg.l = Roughness::level (p.amp);
                sg.start = pool;
                for (int j = 0; j < nCurR && pool < kMaxWholePairs; ++j)
                    if (curR[j].freq > lo - win && curR[j].freq < hi + win)
                    {
                        wholeRef[pool] = curR[j];
                        wholeRefL[pool] = Roughness::level (curR[j].amp);
                        wholeRefS[pool] = Roughness::sFactor (curR[j].freq);
                        ++pool;
                    }
                sg.len = pool - sg.start;
                if (sg.len > 0)
                    ++nSeg;
            }
            auto cost = [&] (float c)
            {
                const float ratio = (float) centsToRatio (c);
                float acc = 0.0f;
                for (int k = 0; k < nSeg; ++k)
                {
                    const WholeSeg& sg = wholeSegs[k];
                    acc += partialFriction (sg.f * ratio, sg.a, sg.l, wholeRef + sg.start, wholeRefL + sg.start, sg.len,
                                            nullptr, nullptr, wholeRefS + sg.start);
                }
                return acc + 0.1f * total0 * std::abs (c) / maxC;
            };
            float bestC = 0.0f, bestV = cost (0.0f);
            for (float c = -maxC; c <= maxC; c += 2.0f)
            {
                const float v = cost (c);
                if (v < bestV) { bestV = v; bestC = c; }
            }
            // golden-section refinement in [bestC - 2, bestC + 2]
            float lo = std::max (-maxC, bestC - 2.0f), hi = std::min (maxC, bestC + 2.0f);
            const float gr = 0.6180339887f;
            float x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
            float f1 = cost (x1), f2 = cost (x2);
            for (int it = 0; it < 24; ++it)
            {
                if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - gr * (hi - lo); f1 = cost (x1); }
                else         { lo = x1; x1 = x2; f1 = f2; x2 = lo + gr * (hi - lo); f2 = cost (x2); }
            }
            const float xr = 0.5f * (lo + hi);
            const float vr = cost (xr);
            if (vr < bestV) { bestV = vr; bestC = xr; }

            if (bestV < 0.5f * total0)
            {
                if (wholeDecided != 0.0f)
                {
                    const float vCur = cost (wholeDecided);
                    desired = bestV < 0.8f * vCur ? bestC : wholeDecided;
                    if (std::abs (bestC - wholeDecided) < 3.0f)
                        desired = bestC; // small refinements are always welcome
                }
                else
                {
                    desired = bestC;
                }
            }
            else if (wholeDecided != 0.0f && cost (wholeDecided) < 0.5f * total0)
            {
                desired = wholeDecided;
            }
        }
    }
    wholeDecided = desired;
    centsSched.push ({ centre, desired * amount });
}

void FrictionEngine::decideTensionSynth (Analysis& a) noexcept
{
    const std::int64_t centre = t - a.N / 2;
    const float beta = 0.04f * std::pow (25.0f, clampf (params.beatRate, 0.0f, 1.0f));
    const Partial* src = selfMode ? curT : curR;
    const int nSrc = selfMode ? nCurT : nCurR;
    const PeakTracker& tr = selfMode ? a.trT : a.trR;

    int idx[kMaxPartials];
    int n = 0;
    for (int j = 0; j < nSrc; ++j)
        if (src[j].amp >= 1.0e-3f && eligible (src[j].freq, src[j].freq < params.crossoverHz))
            idx[n++] = j;
    std::sort (idx, idx + n, [&] (int x, int y) { return src[x].amp > src[y].amp; });
    n = std::min (n, kMaxSynthVoices);

    SynthRecord rec;
    rec.time = centre;
    rec.n = 0;
    for (int k = 0; k < n; ++k)
    {
        const Partial& p = src[idx[k]];
        int key;
        if (p.src == 1)
            key = 100000 + (p.slot - kLfSlotBase);
        else
            key = (int) (tr.idOf (p.slot) & 0x3fffffff);
        const float sign = (key & 1) ? -1.0f : 1.0f;
        VoiceTarget& v = rec.v[rec.n++];
        v.key = key;
        v.freq = p.freq + sign * beta * Roughness::peakDeltaHz (p.freq);
        v.amp = clampf (params.amount, 0.0f, 1.0f) * p.amp;
    }
    synthSched.push (rec);
}

void FrictionEngine::processMixSpectral (Analysis& a, bool, bool pvActive) noexcept
{
    const int N = mixN;
    const int bins = N / 2 + 1;
    const double binHz = fs / N;

    bool anyGain = false;
    for (int s = 0; s < (int) a.slots.size(); ++s)
        if (a.slots[(size_t) s].depthDb > 0.0f && a.trT.isUsed (s))
        {
            anyGain = true;
            break;
        }
    const bool identity = ! anyGain && ! pvActive;
    const int ch = nCh;
    const std::int64_t start = t - N;

    if (identity)
    {
        for (int c = 0; c < ch; ++c)
        {
            const float* in = inRing[c].data();
            float* o = ola[c].data();
            for (int j = 0; j < N; ++j)
            {
                const float x = in[(size_t) ((start + j) & ringMask)];
                o[(size_t) ((t + j) & olaMask)] += x * anaWin[(size_t) j] * synWin[(size_t) j];
            }
        }
        std::fill (gainDb.begin(), gainDb.end(), 0.0f);
        lastProbe.lastFrameIdentity = true;
        return;
    }

    for (int c = 0; c < ch; ++c)
    {
        const float* in = inRing[c].data();
        for (int j = 0; j < N; ++j)
            procFrame[(size_t) j] = in[(size_t) ((start + j) & ringMask)] * anaWin[(size_t) j];
        procFft.forward (procFrame.data(), spec[c].data());
    }

    std::fill (gainDb.begin(), gainDb.end(), 0.0f);
    if (anyGain)
    {
        for (int s = 0; s < (int) a.slots.size(); ++s)
        {
            const SlotState& st = a.slots[(size_t) s];
            if (st.depthDb <= 0.0f || ! a.trT.isUsed (s) || st.lastFreq <= 0.0f)
                continue;
            // full depth over the partial's own region (at least +-1 bin), raised-cosine skirts of 2 bins
            const float c = (float) (st.lastFreq / binHz);
            const float fullLo = std::min (c - 1.0f, st.loBin + 1.0f);
            const float fullHi = std::max (c + 1.0f, st.hiBin - 1.0f);
            const int k0 = std::max (1, (int) std::ceil (fullLo - 2.0f));
            const int k1 = std::min (bins - 2, (int) std::floor (fullHi + 2.0f));
            for (int k = k0; k <= k1; ++k)
            {
                const float kk = (float) k;
                const float d = kk < fullLo ? fullLo - kk : (kk > fullHi ? kk - fullHi : 0.0f);
                const float w = d <= 0.0f ? 1.0f : 0.5f * (1.0f + std::cos ((float) kPi * d * 0.5f));
                gainDb[(size_t) k] -= st.depthDb * w;
            }
        }
        for (int k = 0; k < bins; ++k)
        {
            const float g = gainDb[(size_t) k];
            if (g < 0.0f)
            {
                const float lin = dbToGain (std::max (g, -kMaxDepthDb - 6.0f));
                for (int c = 0; c < ch; ++c)
                    spec[c][(size_t) k] *= lin;
            }
        }
    }

    if (pvActive)
    {
        cfloat* specPtrs[kMaxChannels] = { spec[0].data(), spec[1].data() };
        pv.process (specPtrs, ch, a.specT.magnitude(), pvReq, nPvReq, frameIsTransient);
    }

    for (int c = 0; c < ch; ++c)
    {
        procFft.inverse (spec[c].data(), procFrame.data());
        float* o = ola[c].data();
        for (int j = 0; j < N; ++j)
            o[(size_t) ((t + j) & olaMask)] += procFrame[(size_t) j] * synWin[(size_t) j];
    }
    lastNonIdentityTrigger = t;
    lastProbe.lastFrameIdentity = false;
}

void FrictionEngine::updateTrackBells (Analysis& a) noexcept
{
    // candidates: live slots with a dip, strongest first
    int cand[kMaxSlots];
    int nc = 0;
    for (int s = 0; s < (int) a.slots.size(); ++s)
        if (a.slots[(size_t) s].depthDb > 0.0f && a.trT.isUsed (s) && a.slots[(size_t) s].lastFreq > 0.0f)
            cand[nc++] = s;
    // priority: depth plus partial level (dB), so audible partials get the limited bells first
    auto prio = [&] (int s) { const SlotState& st = a.slots[(size_t) s]; return st.depthDb + gainToDb (st.lastAmp, -120.0f); };
    std::sort (cand, cand + nc, [&] (int x, int y) { return prio (x) > prio (y); });
    nc = std::min (nc, kTrackHfBells);

    // release bells whose owner is no longer a candidate
    for (int b = 0; b < kTrackHfBells; ++b)
    {
        const int owner = bellOwner[(size_t) b];
        if (owner < 0)
            continue;
        bool still = false;
        for (int k = 0; k < nc; ++k)
            if (cand[k] == owner)
                still = true;
        if (! still)
        {
            trackBells.setTarget (b, trackBells.currentFreq (b), 0.0f, std::max (6.0f, trackBells.currentFreq (b) / 30.0f));
            if (std::abs (trackBells.currentGainDb (b)) < 0.05f)
                bellOwner[(size_t) b] = -1;
            a.slots[(size_t) owner].bell = -1;
        }
    }
    for (int k = 0; k < nc; ++k)
    {
        const int s = cand[k];
        SlotState& st = a.slots[(size_t) s];
        int b = -1;
        for (int i = 0; i < kTrackHfBells; ++i)
            if (bellOwner[(size_t) i] == s)
                b = i;
        if (b < 0)
        {
            for (int i = 0; i < kTrackHfBells; ++i)
                if (bellOwner[(size_t) i] < 0)
                {
                    b = i;
                    bellOwner[(size_t) i] = s;
                    break;
                }
        }
        if (b < 0)
            continue;
        st.bell = b;
        const float f = st.lastFreq;
        trackBells.setTarget (b, f, -st.depthDb, std::max (6.0f, f / 30.0f));
    }
}

// ============================================================================ display / advisor

void FrictionEngine::publishDisplay (Analysis& a) noexcept
{
    DisplayFrame& d = displayBuf.back();
    d.sequence = ++displaySeq;
    d.sampleRate = (float) fs;

    const float* mT = a.specT.magnitudeDb();
    const float* mR = a.specR.magnitudeDb();
    const double binHzA = fs / a.N;
    const int maxBin = a.N / 2;
    for (int g = 0; g < DisplayFrame::kN; ++g)
    {
        float vt = -200.0f, vr = -200.0f;
        if (a.gridHi[(size_t) g] - a.gridLo[(size_t) g] <= 1)
        {
            // grid finer than the FFT: interpolate between bins (log magnitude) instead of repeating bins
            const double pos = std::min ((double) maxBin - 1.0, DisplayGrid::hzAt (g) / binHzA);
            const int k0 = std::max (0, (int) pos);
            const float fr = (float) (pos - k0);
            vt = mT[k0] + fr * (mT[k0 + 1] - mT[k0]);
            vr = mR[k0] + fr * (mR[k0 + 1] - mR[k0]);
        }
        else
        {
            for (int k = a.gridLo[(size_t) g]; k <= a.gridHi[(size_t) g]; ++k)
            {
                vt = std::max (vt, mT[k]);
                vr = std::max (vr, mR[k]);
            }
        }
        d.targetDb[g] = vt;
        d.refDb[g] = sidechainPresentNow ? vr : -200.0f;
        d.friction[g] = 0.0f;
        d.selfFriction[g] = 0.0f;
        d.gainDb[g] = 0.0f;
    }

    // friction curves on the ERB axis (sigma = 0.5 ERB, +-4 sigma)
    const float eMin = (float) erbRate (DisplayGrid::kMinHz);
    const float eMax = (float) erbRate (DisplayGrid::kMaxHz);
    for (int i = 0; i < nCurT; ++i)
    {
        const Partial& p = curT[i];
        if (p.friction < 1.0e-4f && p.self < 1.0e-4f)
            continue;
        const float ep = (float) erbRate (p.freq);
        const float gLo = DisplayGrid::indexOf ((float) erbRateToHz (std::max (eMin, ep - 2.0f)));
        const float gHi = DisplayGrid::indexOf ((float) erbRateToHz (std::min (eMax, ep + 2.0f)));
        for (int g = std::max (0, (int) gLo); g <= std::min (DisplayFrame::kN - 1, (int) gHi + 1); ++g)
        {
            const float e = (float) erbRate (DisplayGrid::hzAt (g));
            const float z = (e - ep) / 0.5f;
            const float w = std::exp (-0.5f * z * z);
            d.friction[g] += p.friction * w;
            d.selfFriction[g] += p.self * w;
        }
    }

    // gain curve: STFT dips / Track bells / low-band bells
    auto addDip = [&] (float f, float depth, float halfWidthHz)
    {
        if (depth <= 0.0f || f <= 0.0f)
            return;
        const float gLo = DisplayGrid::indexOf (std::max (DisplayGrid::kMinHz, f - 2.0f * halfWidthHz));
        const float gHi = DisplayGrid::indexOf (std::min (DisplayGrid::kMaxHz, f + 2.0f * halfWidthHz));
        for (int g = std::max (0, (int) gLo); g <= std::min (DisplayFrame::kN - 1, (int) gHi + 1); ++g)
        {
            const float df = std::abs (DisplayGrid::hzAt (g) - f) / std::max (1.0f, halfWidthHz);
            const float w = df <= 1.0f ? 1.0f : (df < 2.0f ? 0.5f * (1.0f + std::cos ((float) kPi * (df - 1.0f))) : 0.0f);
            d.gainDb[g] -= depth * w;
        }
    };
    const float binHz = (float) (fs / a.N);
    float maxRed = 0.0f;
    for (int s = 0; s < (int) a.slots.size(); ++s)
    {
        const SlotState& st = a.slots[(size_t) s];
        if (st.depthDb > 0.0f && a.trT.isUsed (s))
        {
            const float hw = activeLatency == LatencyMode::Mix ? 2.0f * (float) (fs / mixN) : std::max (6.0f, st.lastFreq / 30.0f) * 0.5f;
            addDip (st.lastFreq, st.depthDb, hw);
            maxRed = std::max (maxRed, st.depthDb);
        }
    }
    (void) binHz;
    for (const auto& st : lfSlots)
        if (st.depthDb > 0.0f)
        {
            addDip (st.lastFreq, st.depthDb, 0.5f * clampf (0.1f * st.lastFreq, 3.0f, 20.0f));
            maxRed = std::max (maxRed, st.depthDb);
        }
    for (int g = 0; g < DisplayFrame::kN; ++g)
        d.gainDb[g] = std::max (d.gainDb[g], -kMaxDepthDb - 6.0f);

    // marks
    d.numTarget = std::min (nCurT, DisplayFrame::kMarks);
    for (int i = 0; i < d.numTarget; ++i)
    {
        const Partial& p = curT[i];
        DisplayFrame::Mark& m = d.target[i];
        m.freq = p.freq;
        m.amp = p.amp;
        m.friction = p.friction;
        m.self = p.self;
        m.src = p.src;
        m.gainDb = 0.0f;
        m.shiftCents = 0.0f;
        if (p.src == 0 && p.slot >= 0)
        {
            const SlotState& st = a.slots[(size_t) p.slot];
            m.gainDb = -st.depthDb;
            m.shiftCents = st.shiftCents;
        }
        else if (p.src == 1)
        {
            const int k = p.slot - kLfSlotBase;
            if (k >= 0 && k < kLfHarmonics)
                m.gainDb = -lfSlots[k].depthDb;
        }
    }
    d.numRef = std::min (nCurR, DisplayFrame::kMarks);
    for (int i = 0; i < d.numRef; ++i)
    {
        DisplayFrame::Mark& m = d.ref[i];
        m = DisplayFrame::Mark();
        m.freq = curR[i].freq;
        m.amp = curR[i].amp;
        m.src = curR[i].src;
    }

    d.worst = selfMode ? worstSelf : worstCross;
    d.worstSelf = worstSelf;
    d.totalFriction = totalCross;
    d.totalSelf = totalSelfF;
    d.threshold = sensitivityToThreshold (params.sensitivity);
    d.wholeShiftCents = wholeCents;
    d.maxGainReductionDb = maxRed;
    d.sidechain = (int) scStatus;
    d.mode = (int) activeMode;
    d.latencyMode = (int) activeLatency;
    d.retuneStyle = (int) activeRetuneStyle;
    d.tensionStyle = (int) activeTensionStyle;
    d.selfAnalysis = selfMode;
    d.lfTargetVoiced = lastLfT.voiced;
    d.lfRefVoiced = lastLfR.voiced && sidechainPresentNow;
    d.lfTargetF0 = lastLfT.f0 * lfCorrFor (lfCorrT, lfCorrF0T, lastLfT.f0, 0);
    d.lfRefF0 = lastLfR.f0 * lfCorrFor (lfCorrR, lfCorrF0R, lastLfR.f0, 0);
    d.synthVoices = synth.snapshot (d.voiceFreq, d.voiceAmp, 16);
    d.latencySamples = currentLatency();
    displayBuf.publish();
}

void FrictionEngine::pushAdvisorFrame (Analysis& a) noexcept
{
    if (! params.learn)
    {
        wasLearning = false;
        return;
    }
    AdvisorFrame f;
    if (! wasLearning)
    {
        wasLearning = true;
        learnStartTime = (double) t / fs;
        f.startOfLearn = true;
    }
    f.time = (double) t / fs - learnStartTime;
    f.hopSeconds = (float) ((double) a.H / fs);

    auto pickTop = [] (const Partial* list, int n, float* fo, float* ao, std::uint8_t* lo, float xo) -> std::uint8_t
    {
        int idx[kMaxPartials];
        for (int i = 0; i < n; ++i)
            idx[i] = i;
        std::sort (idx, idx + n, [&] (int x, int y) { return list[x].amp > list[y].amp; });
        const int m = std::min (n, AdvisorFrame::kMax);
        for (int k = 0; k < m; ++k)
        {
            fo[k] = list[idx[k]].freq;
            ao[k] = list[idx[k]].amp;
            lo[k] = (list[idx[k]].src == 1 || list[idx[k]].freq < xo) ? 1 : 0;
        }
        return (std::uint8_t) m;
    };
    f.nT = pickTop (curT, nCurT, f.tf, f.ta, f.tLow, params.crossoverHz);
    f.nR = selfMode ? 0 : pickTop (curR, nCurR, f.rf, f.ra, f.rLow, params.crossoverHz);

    const std::int64_t centre = t - a.N / 2;
    LfEstimate eT, eR;
    const std::int64_t maxDist = 2 * lfT.hopSamples();
    if (lfT.lookup (centre, eT, maxDist))
    {
        f.tVoiced = eT.voiced;
        f.tF0 = eT.f0;
        f.tLfLevel = eT.level;
    }
    if (! selfMode && lfR.lookup (centre, eR, maxDist))
    {
        f.rVoiced = eR.voiced;
        f.rF0 = eR.f0;
        f.rLfLevel = eR.level;
    }
    const std::int64_t lastOn = onsets.lastOnsetAtOrBefore (centre);
    f.sinceOnset = lastOn > INT64_MIN / 4 ? (float) ((double) (centre - lastOn) / fs) : 1.0e9f;
    advisorQ.push (f);
}

} // namespace fr
