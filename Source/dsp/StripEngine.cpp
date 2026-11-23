#include "StripEngine.h"

#include <cstdio>
#include <cstring>

namespace lacs
{
const char* moduleShortName (ModuleId m) noexcept
{
    switch (m)
    {
        case ModuleId::gate:       return "GATE";
        case ModuleId::transient:  return "TRANSIENT";
        case ModuleId::comp:       return "COMP";
        case ModuleId::eq:         return "EQ";
        case ModuleId::deesser:    return "DE-ESS";
        case ModuleId::drive:      return "DRIVE";
        case ModuleId::numModules: break;
    }
    return "";
}

ModuleOrder defaultModuleOrder() noexcept
{
    return { (int) ModuleId::gate, (int) ModuleId::transient, (int) ModuleId::comp, (int) ModuleId::eq,
             (int) ModuleId::deesser, (int) ModuleId::drive };
}

ModuleOrder orderFromString (const char* text) noexcept
{
    ModuleOrder order {};
    if (text == nullptr || std::strlen (text) != (size_t) kNumOrderable)
        return defaultModuleOrder();
    bool seen[kNumOrderable] {};
    for (int i = 0; i < kNumOrderable; ++i)
    {
        const int v = text[i] - '0';
        if (v < 0 || v >= kNumOrderable || seen[v])
            return defaultModuleOrder();
        seen[v] = true;
        order[(size_t) i] = v;
    }
    return order;
}

void orderToString (const ModuleOrder& order, char* out) noexcept
{
    for (int i = 0; i < kNumOrderable; ++i)
        out[i] = (char) ('0' + order[(size_t) i]);
    out[kNumOrderable] = 0;
}

//==============================================================================
int AnalysisStats::binOf (float db) noexcept
{
    const int b = (int) std::floor ((db + 100.0f) * 2.0f);
    return b < 0 ? 0 : (b >= bins ? bins - 1 : b);
}

void AnalysisStats::add (float rmsDb, float peakDb, float sibDb) noexcept
{
    ++rms[(size_t) binOf (rmsDb)];
    ++peak[(size_t) binOf (peakDb)];
    ++sib[(size_t) binOf (sibDb)];
    ++frames;
}

float AnalysisStats::percentile (const std::array<uint32_t, bins>& h, float p, float fromDb) noexcept
{
    const int first = binOf (fromDb);
    uint64_t total = 0;
    for (int b = first; b < bins; ++b) total += h[(size_t) b];
    if (total == 0)
        return -120.0f;
    const auto target = (uint64_t) std::ceil (clampf (p, 0.0f, 1.0f) * (float) total);
    uint64_t acc = 0;
    for (int b = first; b < bins; ++b)
    {
        acc += h[(size_t) b];
        if (acc >= std::max<uint64_t> (1, target))
            return dbOf (b);
    }
    return dbOf (bins - 1);
}

float AnalysisStats::energyMean (const std::array<uint32_t, bins>& h, float fromDb) noexcept
{
    double power = 0.0;
    uint64_t total = 0;
    for (int b = binOf (fromDb); b < bins; ++b)
    {
        power += (double) h[(size_t) b] * std::pow (10.0, dbOf (b) / 10.0);
        total += h[(size_t) b];
    }
    return total == 0 ? -120.0f : (float) (10.0 * std::log10 (power / (double) total));
}

AssistResult analyseStats (const AnalysisStats& s, float compRatio) noexcept
{
    AssistResult r;
    if (s.frames < 100) // 1 second
    {
        std::snprintf (r.summary, sizeof (r.summary), "not enough audio");
        return r;
    }
    const float loudest = AnalysisStats::percentile (s.rms, 0.99f);
    if (loudest < -75.0f)
    {
        std::snprintf (r.summary, sizeof (r.summary), "not enough audio (input is silent)");
        return r;
    }
    // "Active" frames: within 30 dB of the loudest part. Their energy average is what a VU meter reads.
    const float activeFrom = std::max (-90.0f, loudest - 30.0f);
    const float body = AnalysisStats::energyMean (s.rms, activeFrom);
    r.trimDb = clampf (std::round ((-18.0f - body) * 2.0f) / 2.0f, -24.0f, 24.0f);

    // Gate: between the noise floor and the quiet end of the performance, only if they are far apart
    const float floor = AnalysisStats::percentile (s.rms, 0.1f);
    const float quiet = AnalysisStats::percentile (s.rms, 0.1f, activeFrom);
    const float quietPeak = AnalysisStats::percentile (s.peak, 0.1f, activeFrom);
    r.gateOn = quiet - floor > 14.0f;
    r.gateThresholdDb = clampf (0.5f * (floor + quietPeak) + r.trimDb, -80.0f, -10.0f);

    // Compressor: loud peaks exceed the threshold by about 8 dB, so they see 4-6 dB of gain reduction
    const float loudPeak = AnalysisStats::percentile (s.peak, 0.8f, activeFrom);
    const float wantGr = 5.0f;
    const float slope = 1.0f - 1.0f / std::max (1.5f, compRatio);
    r.compThresholdDb = clampf (loudPeak + r.trimDb - wantGr / slope, -50.0f, -3.0f);

    // De-esser: only when there is noticeable energy above 5 kHz in the loud frames
    const float sibLoud = AnalysisStats::percentile (s.sib, 0.95f, activeFrom - 20.0f);
    r.deessOn = sibLoud - body > -12.0f;
    r.deessThresholdDb = clampf (sibLoud + r.trimDb - 5.0f, -50.0f, -6.0f);

    r.ok = true;
    std::snprintf (r.summary, sizeof (r.summary), "trim %+.1f dB (body %.1f dBFS RMS -> -18), compressor threshold %.1f dB%s%s",
                   (double) r.trimDb, (double) body, (double) r.compThresholdDb, r.gateOn ? ", gate on" : "",
                   r.deessOn ? ", de-esser on" : "");
    return r;
}

//==============================================================================
void StripEngine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    maxBlock = std::max (16, maxBlockSize);

    consoleIn.prepare (sr, 11u);
    consoleOut.prepare (sr, 23u);
    filters.prepare (sr);
    gate.prepare (sr);
    transient.prepare (sr);
    comp.prepare (sr);
    for (auto* e : { &eq, &keyEqGate, &keyEqComp }) e->prepare (sr);
    deess.prepare (sr);
    drive.prepare (sr);

    for (auto* v : { &rawL, &rawR, &workL, &workR, &keyL, &keyR, &analysisIn, &analysisOut })
        v->assign ((size_t) maxBlock, 0.0f);
    for (auto& d : dryDelay)
    {
        d.prepare (getLatency() + 8);
        d.setDelay (getLatency());
    }
    for (auto* s : { &inGain, &outGain, &panSm, &widthSm, &mixSm })
        s->setTime (sr, 20.0f);
    vuCoeff = msCoeff (sr, 300.0f);
    hopLength = std::max (1, (int) (sr * 0.01));
    sibHp1.setHighPass (sr, 5000.0, 0.54119610);
    sibHp2.setHighPass (sr, 5000.0, 1.30656296);
    lastChannel = -1;
    setParams (params);
    reset();
}

void StripEngine::reset()
{
    consoleIn.reset();
    consoleOut.reset();
    filters.reset();
    gate.reset();
    transient.reset();
    comp.reset();
    for (auto* e : { &eq, &keyEqGate, &keyEqComp }) e->reset();
    deess.reset();
    drive.reset();
    for (auto& d : dryDelay) d.reset();
    inGain.snap (dbToGain (params.inputGainDb));
    outGain.snap (dbToGain (params.outputGainDb));
    panSm.snap (params.pan);
    widthSm.snap (params.width);
    mixSm.snap (params.mix);
    vuIn = vuOut = 0.0f;
    hopPos = 0;
    hop = {};
    sibHp1.reset();
    sibHp2.reset();
    statSum = sibSum = 0.0;
    statPeak = 0.0f;
    meters = {};
}

void StripEngine::setStatsEnabled (bool on) noexcept
{
    if (on && ! statsOn)
        stats.reset();
    statsOn = on;
}

void StripEngine::setParams (const EngineParams& p) noexcept
{
    params = p;
    if (p.console.channel != lastChannel || p.console.analogue != lastAnalogue)
    {
        tolerance = p.console.analogue ? Tolerance::forChannel (p.console.channel) : Tolerance::ideal();
        lastChannel = p.console.channel;
        lastAnalogue = p.console.analogue;
    }
    const auto& t = tolerance;
    consoleIn.setParams (p.console, t, false);
    consoleOut.setParams (p.console, t, true);
    filters.setParams (p.filters, p.console, t);
    gate.setParams (p.gate, p.console.analogue ? t : Tolerance::ideal());
    transient.setParams (p.transient);
    comp.setParams (p.comp, p.console, t);
    eq.setParams (p.eq, p.console, t);
    keyEqGate.setParams (p.eq, p.console, t);
    keyEqComp.setParams (p.eq, p.console, t);
    deess.setParams (p.deess);
    drive.setParams (p.drive);
}

bool StripEngine::eqFeedsKey() const noexcept
{
    return params.eq.enabled
           && ((params.gate.enabled && params.gate.key == KeySource::eq) || (params.comp.enabled && params.comp.key == KeySource::eq));
}

void StripEngine::runModule (ModuleId id, float* l, float* r, const float* scL, const float* scR, int n) noexcept
{
    auto keyFor = [&] (KeySource src, EqModule& keyEq, const float*& kl, const float*& kr)
    {
        kl = kr = nullptr;
        if (src == KeySource::external && scL != nullptr)
        {
            kl = scL;
            kr = scR;
        }
        else if (src == KeySource::eq && params.eq.enabled)
        {
            std::copy (l, l + n, keyL.begin());
            std::copy (r, r + n, keyR.begin());
            keyEq.process (keyL.data(), keyR.data(), n);
            kl = keyL.data();
            kr = keyR.data();
        }
    };

    switch (id)
    {
        case ModuleId::gate:
            if (params.gate.enabled)
            {
                const float *kl, *kr;
                keyFor (params.gate.key, keyEqGate, kl, kr);
                gate.process (l, r, kl, kr, n);
            }
            break;
        case ModuleId::comp:
            if (params.comp.enabled)
            {
                const float *kl, *kr;
                keyFor (params.comp.key, keyEqComp, kl, kr);
                comp.process (l, r, kl, kr, n);
            }
            break;
        case ModuleId::transient: if (params.transient.enabled) transient.process (l, r, n); break;
        case ModuleId::eq:        if (params.eq.enabled && ! eqFeedsKey()) eq.process (l, r, n); break;
        case ModuleId::deesser:   if (params.deess.enabled) deess.process (l, r, n); break;
        case ModuleId::drive:     drive.process (l, r, n); break; // always runs: constant latency
        case ModuleId::numModules: break;
    }
}

void StripEngine::process (float* const* io, int channels, int n, const float* const* sidechain, DynListener* listener) noexcept
{
    n = std::min (n, maxBlock);
    float* inL = io[0];
    float* inR = channels > 1 ? io[1] : io[0];
    float* l = workL.data();
    float* r = workR.data();

    // ---- Input --------------------------------------------------------------------------------------
    const float ig = dbToGain (params.inputGainDb) * (params.polarity ? -1.0f : 1.0f);
    float inPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = inL[i], b = inR[i];
        rawL[(size_t) i] = a;
        rawR[(size_t) i] = b;
        inPeak = std::max (inPeak, std::max (std::abs (a), std::abs (b)));
        const float g = inGain.next (ig);
        l[i] = a * g;
        r[i] = b * g;
    }
    consoleIn.process (l, r, n);
    filters.process (l, r, n);

    // Input VU (after trim: shows where the channel sits against 0 VU = -18 dBFS)
    for (int i = 0; i < n; ++i)
    {
        const float ms = 0.5f * (l[i] * l[i] + r[i] * r[i]);
        vuIn = ms + vuCoeff * (vuIn - ms);
        analysisIn[(size_t) i] = 0.5f * (l[i] + r[i]);
    }

    // ---- Chain ----------------------------------------------------------------------------------------
    const float* scL = sidechain != nullptr ? sidechain[0] : nullptr;
    const float* scR = sidechain != nullptr ? sidechain[1] : nullptr;
    for (int idx : params.order)
        runModule ((ModuleId) idx, l, r, scL, scR, n);
    consoleOut.process (l, r, n);

    // ---- Output: gain, pan, width, dry/wet --------------------------------------------------------------
    const float og = dbToGain (params.outputGainDb);
    float outPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float g = outGain.next (og);
        const float pan = panSm.next (clampf (params.pan, -1.0f, 1.0f));
        const float w = widthSm.next (clampf (params.width, 0.0f, 2.0f));
        const float mx = mixSm.next (clampf (params.mix, 0.0f, 1.0f));
        float a = l[i] * g, b = r[i] * g;
        const float m = 0.5f * (a + b), s = 0.5f * (a - b) * w;
        a = m + s;
        b = m - s;
        if (params.monoSource)
        {
            // constant power, unity at the centre
            const float ang = (pan + 1.0f) * (float) kPi * 0.25f;
            a *= std::cos (ang) * 1.41421356f;
            b *= std::sin (ang) * 1.41421356f;
        }
        else
        {
            a *= std::min (1.0f, 1.0f - pan);
            b *= std::min (1.0f, 1.0f + pan);
        }
        const float dl = dryDelay[0].process (rawL[(size_t) i]);
        const float dr = dryDelay[1].process (rawR[(size_t) i]);
        a = dl + mx * (a - dl);
        b = dr + mx * (b - dr);
        if (! std::isfinite (a)) a = 0.0f;
        if (! std::isfinite (b)) b = 0.0f;
        l[i] = a;
        r[i] = b;
        outPeak = std::max (outPeak, std::max (std::abs (a), std::abs (b)));
        const float ms = 0.5f * (a * a + b * b);
        vuOut = ms + vuCoeff * (vuOut - ms);
        analysisOut[(size_t) i] = 0.5f * (a + b);
    }

    // ---- 10 ms frames: dynamics history and assistant statistics ----------------------------------------
    const float compGr = params.comp.enabled ? comp.getReductionDb() : 0.0f;
    const float gateGr = params.gate.enabled ? gate.getReductionDb() : 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = rawL[(size_t) i], b = rawR[(size_t) i];
        const float pk = std::max (std::abs (a), std::abs (b));
        hop.inDb = std::max (hop.inDb, gainToDb (pk));
        hop.outDb = std::max (hop.outDb, gainToDb (std::max (std::abs (l[i]), std::abs (r[i]))));
        if (statsOn)
        {
            statSum += 0.5 * ((double) a * a + (double) b * b);
            statPeak = std::max (statPeak, pk);
            const float sb = sibHp2.process (sibHp1.process (0.5f * (a + b)));
            sibSum += (double) sb * sb;
        }
        if (++hopPos >= hopLength)
        {
            hop.compGrDb = compGr;
            hop.gateGrDb = gateGr;
            if (listener != nullptr)
                listener->dynFrame (hop);
            hop = {};
            if (statsOn)
            {
                const auto len = (double) hopLength;
                stats.add (10.0f * (float) std::log10 (statSum / len + 1.0e-12),
                           gainToDb (statPeak),
                           10.0f * (float) std::log10 (sibSum / len + 1.0e-12));
            }
            statSum = sibSum = 0.0;
            statPeak = 0.0f;
            hopPos = 0;
        }
    }

    // ---- Write back ---------------------------------------------------------------------------------------
    for (int i = 0; i < n; ++i)
    {
        if (channels > 1)
        {
            inL[i] = l[i];
            inR[i] = r[i];
        }
        else
            inL[i] = 0.5f * (l[i] + r[i]);
    }

    // ---- Meters -------------------------------------------------------------------------------------------
    meters.inPeak = inPeak;
    meters.outPeak = outPeak;
    meters.vuInDb = 10.0f * std::log10 (vuIn + 1.0e-12f);
    meters.vuOutDb = 10.0f * std::log10 (vuOut + 1.0e-12f);
    meters.gateDb = gateGr;
    meters.compDb = compGr;
    meters.deessDb = params.deess.enabled ? deess.getReductionDb() : 0.0f;
    meters.transientDb = params.transient.enabled ? transient.getGainDb() : 0.0f;
    meters.gateKeyDb = gate.getKeyLevelDb();
    meters.compKeyDb = comp.getKeyLevelDb();
    meters.deessBandDb = deess.getBandLevelDb();
    meters.autoMakeupDb = params.comp.enabled ? comp.getAutoMakeupDb() : 0.0f;
    meters.gateOpen = ! params.gate.enabled || gate.isOpen();
    meters.eqInSidechain = eqFeedsKey();
}
} // namespace lacs
