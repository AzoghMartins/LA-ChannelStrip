#include "Modules.h"

namespace lacs
{
namespace
{
/** Soft-knee gain computer on the amount above threshold with an arbitrary slope. */
inline float kneeCurve (float over, float slope, float knee) noexcept
{
    if (slope <= 0.0f)
        return 0.0f;
    if (knee <= 0.01f)
        return over > 0.0f ? slope * over : 0.0f;
    if (2.0f * over < -knee)
        return 0.0f;
    if (2.0f * std::abs (over) <= knee)
    {
        const float x = over + knee * 0.5f;
        return slope * x * x / (2.0f * knee);
    }
    return slope * over;
}

inline float coeffAtLeast (float c, float minimum) noexcept { return c < minimum ? minimum : c; }
} // namespace

//==============================================================================
// Tolerances
Tolerance Tolerance::ideal() noexcept
{
    Tolerance t;
    for (auto& b : t.eqFreq) b = { 1.0f, 1.0f };
    for (auto& b : t.eqQ) b = { 1.0f, 1.0f };
    t.filterFreq = { 1.0f, 1.0f };
    t.harmonics = { 1.0f, 1.0f };
    return t;
}

Tolerance Tolerance::forChannel (int channel) noexcept
{
    Tolerance t;
    Random rng (0x9e3779b9u * (uint32_t) std::max (1, channel) + 0x7f4a7c15u);
    for (int i = 0; i < 8; ++i)
        rng.nextInt(); // decorrelate neighbouring seeds
    for (int b = 0; b < 4; ++b)
        for (int s = 0; s < 2; ++s)
        {
            t.eqFreq[(size_t) b][(size_t) s] = 1.0f + 0.03f * rng.nextBipolar();
            t.eqGain[(size_t) b][(size_t) s] = 0.4f * rng.nextBipolar();
            t.eqQ[(size_t) b][(size_t) s] = 1.0f + 0.05f * rng.nextBipolar();
        }
    for (int s = 0; s < 2; ++s)
    {
        t.filterFreq[(size_t) s] = 1.0f + 0.04f * rng.nextBipolar();
        t.levelDb[(size_t) s] = 0.08f * rng.nextBipolar();
        t.harmonics[(size_t) s] = 1.0f + 0.15f * rng.nextBipolar();
    }
    t.thresholdDb = 0.4f * rng.nextBipolar();
    return t;
}

//==============================================================================
// Console stage
void ConsoleStage::prepare (double sampleRate, uint32_t seed)
{
    sr = sampleRate;
    rng = { Random (seed * 2u + 1u), Random (seed * 2u + 7u) };
    lastType = ConsoleType::numTypes;
    reset();
}

void ConsoleStage::reset()
{
    for (auto* arr : { &dcBlock, &lowBump, &topRoll })
        for (auto& b : *arr) b.reset();
    pinkState = {};
}

float ConsoleStage::shape (float x, float k2, float k3) noexcept
{
    float odd = x;
    if (k3 > 1.0e-6f)
    {
        const float g = std::sqrt (3.0f * k3);
        odd = std::tanh (g * x) / g;
    }
    return odd + k2 * x * x / (1.0f + x * x);
}

void ConsoleStage::setParams (const ConsoleParams& p, const Tolerance& t, bool outputStage) noexcept
{
    params = p;
    output = outputStage;
    active = p.analogue;
    if (! active)
        return;

    const float base = 0.05f * std::pow (50.0f, clampf (p.drive, 0.0f, 1.0f)) * (outputStage ? 0.6f : 1.0f);
    float e = 0.12f, o = 1.0f;
    switch (p.type)
    {
        case ConsoleType::british:  e = 0.12f; o = 1.0f; break;
        case ConsoleType::american: e = 0.45f; o = 0.8f; break;
        case ConsoleType::vintage:  e = 0.7f;  o = 0.6f; break;
        case ConsoleType::numTypes: break;
    }
    for (size_t s = 0; s < 2; ++s)
    {
        k2[s] = base * e * t.harmonics[s];
        k3[s] = base * o * t.harmonics[s];
        level[s] = outputStage ? 1.0f : dbToGain (t.levelDb[s]);
    }
    noiseGain = outputStage && p.noiseDb > -119.0f ? 3.0f * dbToGain (p.noiseDb) : 0.0f;

    if (p.type != lastType || outputStage != lastOutput)
    {
        lastType = p.type;
        lastOutput = outputStage;
        float dcHz = 5.0f, bumpHz = 0.0f, bumpDb = 0.0f, rollHz = 0.0f;
        if (! outputStage)
        {
            switch (p.type)
            {
                case ConsoleType::british:  dcHz = 10.0f; break;
                case ConsoleType::american: dcHz = 12.0f; bumpHz = 70.0f; bumpDb = 0.4f; break;
                case ConsoleType::vintage:  dcHz = 18.0f; bumpHz = 45.0f; bumpDb = 0.8f; break;
                case ConsoleType::numTypes: break;
            }
        }
        else
        {
            switch (p.type)
            {
                case ConsoleType::british:  rollHz = 40000.0f; break;
                case ConsoleType::american: rollHz = 32000.0f; break;
                case ConsoleType::vintage:  rollHz = 18000.0f; break;
                case ConsoleType::numTypes: break;
            }
        }
        for (size_t s = 0; s < 2; ++s)
        {
            dcBlock[s].setHighPass (sr, dcHz, 0.70710678);
            if (bumpHz > 0.0f) lowBump[s].setPeak (sr, bumpHz, 0.75, bumpDb);
            if (rollHz > 0.0f && rollHz < sr * 0.42) topRoll[s].setFirstOrderLowPass (sr, rollHz);
        }
        bump = bumpHz > 0.0f;
        roll = rollHz > 0.0f && rollHz < sr * 0.42;
    }
}

void ConsoleStage::process (float* l, float* r, int n) noexcept
{
    if (! active)
        return;
    float* ch[2] = { l, r };
    for (size_t s = 0; s < 2; ++s)
    {
        float* x = ch[s];
        auto& dc = dcBlock[s];
        for (int i = 0; i < n; ++i)
        {
            float y = shape (x[i] * level[s], k2[s], k3[s]);
            if (bump) y = lowBump[s].process (y);
            y = dc.process (y);
            if (roll) y = topRoll[s].process (y);
            if (noiseGain > 0.0f)
            {
                pinkState[s] = 0.5f * pinkState[s] + 0.5f * rng[s].nextBipolar();
                y += noiseGain * pinkState[s];
            }
            x[i] = y;
        }
    }
}

//==============================================================================
// Filters
void FilterModule::prepare (double sampleRate)
{
    sr = sampleRate;
    lastHp = lastLp = -1.0f;
    lastScale = { -1.0f, -1.0f };
    reset();
}

void FilterModule::reset()
{
    for (auto* arr : { &hp1, &hp2, &lp })
        for (auto& b : *arr) b.reset();
}

void FilterModule::setParams (const FilterParams& p, const ConsoleParams& c, const Tolerance& t) noexcept
{
    float hpQ = 1.0f, lpQ = 0.70710678f;
    if (c.analogue)
    {
        switch (c.type)
        {
            case ConsoleType::british:  hpQ = 1.3f; lpQ = 0.85f; break;
            case ConsoleType::american: hpQ = 1.1f; lpQ = 0.75f; break;
            case ConsoleType::vintage:  hpQ = 0.9f; lpQ = 0.6f; break;
            case ConsoleType::numTypes: break;
        }
    }
    const std::array<float, 2> scale = c.analogue ? t.filterFreq : std::array<float, 2> { 1.0f, 1.0f };
    const bool hOn = p.hpfHz > 16.5f;
    const bool lOn = p.lpfHz < 21000.0f && p.lpfHz < sr * 0.45;
    const bool changed = p.hpfHz != lastHp || p.lpfHz != lastLp || hpQ != lastHpQ || lpQ != lastLpQ || scale != lastScale;
    if (changed)
    {
        for (size_t s = 0; s < 2; ++s)
        {
            const double fh = std::max (10.0, (double) p.hpfHz * scale[s]);
            const double fl = std::min ((double) p.lpfHz * scale[s], sr * 0.45);
            hp1[s].setFirstOrderHighPass (sr, fh);
            hp2[s].setHighPass (sr, fh, hpQ);
            lp[s].setLowPass (sr, fl, lpQ);
        }
        lastHp = p.hpfHz;
        lastLp = p.lpfHz;
        lastHpQ = hpQ;
        lastLpQ = lpQ;
        lastScale = scale;
    }
    if (hOn && ! hpfOn) { for (auto& b : hp1) b.reset(); for (auto& b : hp2) b.reset(); }
    if (lOn && ! lpfOn) for (auto& b : lp) b.reset();
    hpfOn = hOn;
    lpfOn = lOn;
}

void FilterModule::process (float* l, float* r, int n) noexcept
{
    if (! hpfOn && ! lpfOn)
        return;
    float* ch[2] = { l, r };
    for (size_t s = 0; s < 2; ++s)
    {
        float* x = ch[s];
        for (int i = 0; i < n; ++i)
        {
            float y = x[i];
            if (hpfOn) y = hp2[s].process (hp1[s].process (y));
            if (lpfOn) y = lp[s].process (y);
            x[i] = y;
        }
    }
}

double FilterModule::magnitudeDb (double f) const noexcept
{
    double db = 0.0;
    if (hpfOn) db += hp1[0].magnitudeDb (sr, f) + hp2[0].magnitudeDb (sr, f);
    if (lpfOn) db += lp[0].magnitudeDb (sr, f);
    return db;
}

//==============================================================================
// EQ
void EqModule::design (Biquad& m, Biquad& x, double sr, int band, float freq, float gainDb, float q, bool bell,
                       const ConsoleParams& c, const Tolerance& t, int side) noexcept
{
    const bool shelf = (band == 0 || band == kEqBands - 1) && ! bell;
    x.setBypass();
    if (! c.analogue)
    {
        if (! shelf)
            m.setPeak (sr, freq, std::max (0.1f, q), gainDb);
        else if (band == 0)
            m.setLowShelf (sr, freq, gainDb, clampf (q, 0.3f, 1.5f));
        else
            m.setHighShelf (sr, freq, gainDb, clampf (q, 0.3f, 1.5f));
        return;
    }

    const auto b = (size_t) band, s = (size_t) side;
    const float g = gainDb + t.eqGain[b][s] * std::abs (gainDb) / 18.0f;
    const float f = freq * t.eqFreq[b][s];
    float qq = q * t.eqQ[b][s];
    const float ag = std::abs (g);
    float dipAmount = 0.12f, dipRatio = 2.2f, dipQ = 0.8f, slopeScale = 1.0f;
    switch (c.type)
    {
        case ConsoleType::british:  qq *= clampf (0.5f + 0.07f * ag, 0.5f, 1.6f); break;          // proportional Q
        case ConsoleType::american: qq *= clampf (0.4f + 0.1f * ag, 0.4f, 2.0f); dipAmount = 0.18f; dipRatio = 2.0f; dipQ = 0.7f; break;
        case ConsoleType::vintage:  qq *= 0.75f; dipAmount = 0.2f; dipRatio = 2.6f; dipQ = 0.6f; slopeScale = 0.8f; break;
        case ConsoleType::numTypes: break;
    }

    if (! shelf)
    {
        m.setPeak (sr, f, std::max (0.1f, qq), g);
        return;
    }
    const double slope = clampf (q * t.eqQ[b][s] * slopeScale, 0.3f, 1.5f);
    if (band == 0)
    {
        m.setLowShelf (sr, f, g, slope);
        if (ag > 0.01f) x.setPeak (sr, std::min ((double) f * dipRatio, sr * 0.45), dipQ, -dipAmount * g);
    }
    else
    {
        m.setHighShelf (sr, f, g, slope);
        if (ag > 0.01f) x.setPeak (sr, std::max (20.0, (double) f / dipRatio), dipQ, -dipAmount * g);
    }
}

double EqModule::responseDb (double sr, const EqParams& p, const ConsoleParams& c, const Tolerance& t, double f) noexcept
{
    double db = 0.0;
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto k = (size_t) b;
        if (std::abs (p.gain[k]) <= 0.01f)
            continue;
        Biquad m, x;
        const bool bell = (b == 0 && p.lfBell) || (b == kEqBands - 1 && p.hfBell);
        design (m, x, sr, b, p.freq[k], p.gain[k], p.q[k], bell, c, t, 0);
        db += m.magnitudeDb (sr, f) + x.magnitudeDb (sr, f);
    }
    return db;
}

void EqModule::prepare (double sampleRate)
{
    sr = sampleRate;
    first = true;
    reset();
}

void EqModule::reset()
{
    for (auto* arr : { &main, &extra })
        for (auto& band : *arr)
            for (auto& f : band) f.reset();
}

void EqModule::setParams (const EqParams& p, const ConsoleParams& c, const Tolerance& t) noexcept
{
    const bool consoleChanged = c.analogue != console.analogue || c.type != console.type || c.channel != console.channel;
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto k = (size_t) b;
        const bool bell = (b == 0 && p.lfBell) || (b == kEqBands - 1 && p.hfBell);
        const bool wasBell = (b == 0 && params.lfBell) || (b == kEqBands - 1 && params.hfBell);
        if (first || consoleChanged || bell != wasBell || p.freq[k] != params.freq[k] || p.gain[k] != params.gain[k] || p.q[k] != params.q[k])
            for (int s = 0; s < 2; ++s)
                design (main[k][(size_t) s], extra[k][(size_t) s], sr, b, p.freq[k], p.gain[k], p.q[k], bell, c, t, s);
        const bool nowActive = std::abs (p.gain[k]) > 0.01f;
        if (nowActive && ! active[k])
            for (auto& f : main[k]) f.reset();
        active[k] = nowActive;
        const bool ex = nowActive && c.analogue && (b == 0 || b == kEqBands - 1) && ! bell;
        if (ex && ! extraActive[k])
            for (auto& f : extra[k]) f.reset();
        extraActive[k] = ex;
    }
    params = p;
    console = c;
    tol = t;
    first = false;
}

void EqModule::process (float* l, float* r, int n) noexcept
{
    float* ch[2] = { l, r };
    for (size_t s = 0; s < 2; ++s)
    {
        float* x = ch[s];
        for (size_t k = 0; k < (size_t) kEqBands; ++k)
        {
            if (! active[k])
                continue;
            auto& m = main[k][s];
            auto& e = extra[k][s];
            if (extraActive[k])
                for (int i = 0; i < n; ++i) x[i] = e.process (m.process (x[i]));
            else
                for (int i = 0; i < n; ++i) x[i] = m.process (x[i]);
        }
    }
}

//==============================================================================
// Gate / expander
void GateModule::prepare (double sampleRate)
{
    sr = sampleRate;
    lastHp = lastLp = -1.0f;
    reset();
}

void GateModule::reset()
{
    for (auto* arr : { &keyHp, &keyLp })
        for (auto& b : *arr) b.reset();
    env = 0.0f;
    gainDb = params.enabled ? -params.rangeDb : 0.0f;
    grDb = 0.0f;
    keyDb = -120.0f;
    holdCounter = 0;
    open = false;
}

void GateModule::setParams (const GateParams& p, const Tolerance& t) noexcept
{
    if (p.keyHpfHz != lastHp)
    {
        for (auto& b : keyHp) b.setHighPass (sr, std::max (20.0f, p.keyHpfHz), 0.70710678);
        lastHp = p.keyHpfHz;
    }
    if (p.keyLpfHz != lastLp)
    {
        for (auto& b : keyLp) b.setLowPass (sr, std::min ((double) p.keyLpfHz, sr * 0.45), 0.70710678);
        lastLp = p.keyLpfHz;
    }
    params = p;
    thresholdOffset = t.thresholdDb;
    envRel = msCoeff (sr, 8.0f);
}

float GateModule::staticReduction (const GateParams& p, float levelDb) noexcept
{
    if (p.expander)
        return clampf (p.thresholdDb - levelDb, 0.0f, p.rangeDb);
    return levelDb < p.thresholdDb ? p.rangeDb : 0.0f;
}

void GateModule::process (float* l, float* r, const float* keyL, const float* keyR, int n) noexcept
{
    const bool hp = params.keyHpfHz > 20.5f, lp = params.keyLpfHz < 19500.0f;
    const float thr = params.thresholdDb + thresholdOffset;
    const float closeAt = thr - std::max (0.0f, params.hysteresisDb);
    const int holdSamples = (int) (params.holdMs * 0.001 * sr);
    const float att = msCoeff (sr, std::max (0.02f, params.attackMs));
    const float rel = msCoeff (sr, std::max (1.0f, params.releaseMs));
    float maxGr = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float kl = keyL != nullptr ? keyL[i] : l[i];
        float kr = keyR != nullptr ? keyR[i] : r[i];
        if (hp) { kl = keyHp[0].process (kl); kr = keyHp[1].process (kr); }
        if (lp) { kl = keyLp[0].process (kl); kr = keyLp[1].process (kr); }
        const float pk = std::max (std::abs (kl), std::abs (kr));
        env = pk > env ? pk : pk + envRel * (env - pk);
        const float lvl = gainToDb (env);

        float target;
        if (params.expander)
        {
            if (lvl >= thr)
                holdCounter = holdSamples;
            else if (holdCounter > 0)
                --holdCounter;
            open = lvl >= thr || holdCounter > 0;
            target = open ? 0.0f : -clampf (thr - lvl, 0.0f, params.rangeDb);
        }
        else
        {
            if (! open)
            {
                if (lvl > thr) { open = true; holdCounter = holdSamples; }
            }
            else if (lvl >= closeAt)
                holdCounter = holdSamples;
            else if (holdCounter > 0)
                --holdCounter;
            else
                open = false;
            target = open ? 0.0f : -params.rangeDb;
        }

        gainDb = target + (target > gainDb ? att : rel) * (gainDb - target);
        maxGr = std::max (maxGr, -gainDb);
        if (params.listen)
        {
            l[i] = kl;
            r[i] = kr;
        }
        else
        {
            const float g = dbToGain (gainDb);
            l[i] *= g;
            r[i] *= g;
        }
    }
    grDb = maxGr;
    keyDb = gainToDb (env);
}

//==============================================================================
// Compressor
float CompressorModule::staticReduction (float level, float thr, float ratio, float knee) noexcept
{
    return ratio <= 1.0f ? 0.0f : kneeCurve (level - thr, 1.0f - 1.0f / ratio, knee);
}

float CompressorModule::effectiveRatio (CompCharacter c, float ratio, float overDb) noexcept
{
    ratio = std::max (1.0f, ratio);
    if (c == CompCharacter::varimu) // ratio grows with level
        return 1.0f + (ratio - 1.0f) * clampf ((overDb + 6.0f) / 14.0f, 0.15f, 1.0f);
    return ratio;
}

float CompressorModule::effectiveKnee (CompCharacter c, float knee) noexcept
{
    switch (c)
    {
        case CompCharacter::fet:    return std::min (knee, 3.0f);
        case CompCharacter::opto:   return std::max (knee, 10.0f);
        case CompCharacter::varimu: return std::max (knee, 12.0f);
        case CompCharacter::vca: case CompCharacter::numCharacters: break;
    }
    return knee;
}

void CompressorModule::prepare (double sampleRate)
{
    sr = sampleRate;
    lastHp = -1.0f;
    makeupSm.setTime (sr, 30.0f);
    mixSm.setTime (sr, 30.0f);
    for (auto& b : dcBlock) b.setHighPass (sr, 8.0, 0.70710678);
    reset();
}

void CompressorModule::reset()
{
    for (auto& b : scHp) b.reset();
    for (auto& b : dcBlock) b.reset();
    envDb = slowDb = rms = peak = charge = grDb = 0.0f;
    keyDb = -120.0f;
    avgGr = autoMakeupDb = 0.0f;
    lastOutL = lastOutR = 0.0f;
    makeupSm.snap (dbToGain (params.makeupDb));
    mixSm.snap (params.mix);
}

void CompressorModule::setParams (const CompParams& p, const ConsoleParams& c, const Tolerance& t) noexcept
{
    if (p.scHpfHz != lastHp)
    {
        for (auto& b : scHp) b.setHighPass (sr, std::max (20.0f, p.scHpfHz), 0.70710678);
        lastHp = p.scHpfHz;
    }
    params = p;
    console = c;
    thresholdOffset = c.analogue ? t.thresholdDb : 0.0f;
}

void CompressorModule::process (float* l, float* r, const float* keyL, const float* keyR, int n) noexcept
{
    const auto ch = params.character;
    const bool analogue = console.analogue;
    const bool feedback = analogue && params.key == KeySource::internal;
    const bool useRms = ch == CompCharacter::opto || ch == CompCharacter::varimu;
    const bool hp = params.scHpfHz > 20.5f;
    const float thr = params.thresholdDb + thresholdOffset;
    const float knee = effectiveKnee (ch, params.kneeDb);

    float attackMs = std::max (0.02f, params.attackMs), releaseMs = std::max (5.0f, params.releaseMs);
    switch (ch)
    {
        case CompCharacter::fet:    attackMs = std::max (0.02f, attackMs * 0.25f); break;
        case CompCharacter::opto:   attackMs = std::max (attackMs, 3.0f); break;
        case CompCharacter::varimu: attackMs *= 1.5f; releaseMs *= 1.2f; break;
        case CompCharacter::vca: case CompCharacter::numCharacters: break;
    }
    if (params.autoRelease)
        releaseMs = 60.0f;
    const float attC = msCoeff (sr, attackMs);
    const float rmsC = msCoeff (sr, 10.0f), peakC = msCoeff (sr, ch == CompCharacter::fet ? 10.0f : 20.0f), chargeC = msCoeff (sr, 1500.0f);
    const float slowUp = msCoeff (sr, 400.0f), slowDown = msCoeff (sr, 1600.0f);
    const float avgC = msCoeff (sr, 1500.0f), keyRel = msCoeff (sr, 60.0f);
    const float fetGrit = analogue ? 1.0f : 0.35f;
    float maxGr = 0.0f;
    float keyEnv = dbToGain (keyDb);

    for (int i = 0; i < n; ++i)
    {
        const float xl = l[i], xr = r[i];
        float kl = keyL != nullptr ? keyL[i] : xl;
        float kr = keyR != nullptr ? keyR[i] : xr;
        const float rawPk = std::max (std::abs (kl), std::abs (kr));
        keyEnv = rawPk > keyEnv ? rawPk : rawPk + keyRel * (keyEnv - rawPk);
        if (feedback)
        {
            kl = lastOutL;
            kr = lastOutR;
        }
        if (hp)
        {
            kl = scHp[0].process (kl);
            kr = scHp[1].process (kr);
        }
        const float pk = std::max (std::abs (kl), std::abs (kr));
        float level;
        if (useRms)
        {
            rms = pk * pk + rmsC * (rms - pk * pk);
            level = 10.0f * std::log10 (rms * 2.0f + 1.0e-12f);
        }
        else
        {
            // peak detector: instant rise, short decay, so the gain computer sees the waveform's level
            peak = pk > peak ? pk : pk + peakC * (peak - pk);
            level = gainToDb (peak);
        }

        const float ratio = effectiveRatio (ch, params.ratio, level - thr);
        float target, a = attC;
        float relMs = releaseMs;
        if (ch == CompCharacter::opto)
        {
            const float c = envDb > 3.0f ? 1.0f : 0.0f;
            charge = c + chargeC * (charge - c);
            relMs *= 0.6f + 2.5f * charge; // longer release after sustained compression
        }
        float rel = msCoeff (sr, relMs);
        if (feedback)
        {
            const float slope = ratio - 1.0f;
            target = kneeCurve (level - thr, slope, knee);
            const float minC = slope / (slope + 1.0f); // keeps the loop stable and free of ringing
            a = coeffAtLeast (a, minC);
            rel = coeffAtLeast (rel, minC);
        }
        else
            target = staticReduction (level, thr, ratio, knee);

        envDb = target + (target > envDb ? a : rel) * (envDb - target);
        float gr = envDb;
        if (params.autoRelease)
        {
            slowDb = envDb + (envDb > slowDb ? slowUp : slowDown) * (slowDb - envDb);
            gr = std::max (envDb, slowDb);
        }
        gr = std::max (0.0f, gr);
        maxGr = std::max (maxGr, gr);

        const float g = dbToGain (-gr);
        float yl = xl * g, yr = xr * g;
        if (ch == CompCharacter::fet)
        {
            const float amt = clampf (gr / 24.0f, 0.0f, 0.4f) * fetGrit;
            yl += amt * (std::tanh (1.5f * yl) / 1.5f - yl);
            yr += amt * (std::tanh (1.5f * yr) / 1.5f - yr);
        }
        else if (ch == CompCharacter::varimu && analogue)
        {
            const float amt = 0.08f * clampf (gr / 12.0f, 0.0f, 1.0f);
            yl = dcBlock[0].process (yl + amt * yl * yl);
            yr = dcBlock[1].process (yr + amt * yr * yr);
        }
        lastOutL = yl;
        lastOutR = yr;

        if (params.autoMakeup && gainToDb (rawPk) > thr - 24.0f)
            avgGr = gr + avgC * (avgGr - gr);
        const float mk = makeupSm.next (dbToGain (params.makeupDb + (params.autoMakeup ? avgGr : 0.0f)));
        const float m = mixSm.next (clampf (params.mix, 0.0f, 1.0f));
        l[i] = xl + m * (yl * mk - xl);
        r[i] = xr + m * (yr * mk - xr);
    }
    grDb = maxGr;
    keyDb = gainToDb (keyEnv);
    autoMakeupDb = params.autoMakeup ? avgGr : 0.0f;
}

//==============================================================================
// De-esser
void DeEsserModule::prepare (double sampleRate)
{
    sr = sampleRate;
    env.setTimes (sr, 0.3f, 60.0f);
    lastFreq = -1.0f;
    setParams (params);
    reset();
}

void DeEsserModule::reset()
{
    scHp1.reset();
    scHp2.reset();
    for (auto& s : split) s.reset();
    env.reset();
    gainSm = 1.0f;
    grDb = 0.0f;
    bandDb = -120.0f;
}

void DeEsserModule::setParams (const DeEsserParams& p) noexcept
{
    params = p;
    if (std::abs (p.freqHz - lastFreq) > 0.5f)
    {
        lastFreq = p.freqHz;
        scHp1.setHighPass (sr, p.freqHz, 0.54119610);
        scHp2.setHighPass (sr, p.freqHz, 1.30656296);
        for (auto& s : split) s.setFrequency (sr, p.freqHz * 0.8);
    }
}

void DeEsserModule::process (float* l, float* r, int n) noexcept
{
    const float att = msCoeff (sr, 0.5f), rel = msCoeff (sr, 45.0f);
    float maxRed = 0.0f, lvl = -120.0f;
    for (int i = 0; i < n; ++i)
    {
        const float s = scHp2.process (scHp1.process (0.5f * (l[i] + r[i])));
        lvl = gainToDb (env.process (std::abs (s)));
        const float over = lvl - params.thresholdDb;
        float red = 0.0f;
        if (over > 0.0f)
            red = over < 4.0f ? over * over / 8.0f : over - 2.0f; // soft knee
        red = std::min (red * 0.8f, params.rangeDb);
        const float target = dbToGain (-red);
        gainSm = target + (target < gainSm ? att : rel) * (gainSm - target);
        maxRed = std::max (maxRed, -gainToDb (gainSm));

        float ll, hl, lr, hr;
        split[0].process (l[i], ll, hl);
        split[1].process (r[i], lr, hr);
        if (params.listen)
        {
            l[i] = hl;
            r[i] = hr;
        }
        else if (! params.wide)
        {
            // Linkwitz-Riley split: low + high has a flat magnitude, so only the sibilance band moves
            l[i] = ll + hl * gainSm;
            r[i] = lr + hr * gainSm;
        }
        else
        {
            l[i] *= gainSm;
            r[i] *= gainSm;
        }
    }
    grDb = maxRed;
    bandDb = lvl;
}

//==============================================================================
// Transient shaper
void TransientModule::prepare (double sampleRate)
{
    sr = sampleRate;
    mixSm.setTime (sr, 30.0f);
    setParams (params);
    reset();
}

void TransientModule::reset()
{
    fast.reset();
    slow.reset();
    hold.reset();
    gainDb = shownDb = 0.0f;
    mixSm.snap (params.mix);
}

void TransientModule::setParams (const TransientParams& p) noexcept
{
    params = p;
    fast.setTimes (sr, 0.3f, 40.0f);
    slow.setTimes (sr, clampf (p.speedMs, 2.0f, 100.0f), 40.0f);
    hold.setTimes (sr, 0.3f, 250.0f);
    smooth = msCoeff (sr, 1.0f);
    smoothDown = msCoeff (sr, 0.1f);
}

void TransientModule::process (float* l, float* r, int n) noexcept
{
    const float atk = clampf (params.attack, -1.0f, 1.0f) * 1.5f;
    const float sus = clampf (params.sustain, -1.0f, 1.0f) * 1.2f;
    float shown = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float d = std::max (std::abs (l[i]), std::abs (r[i]));
        const float f = fast.process (d), s = slow.process (d), h = hold.process (d);
        const float fDb = gainToDb (f);
        float target = 0.0f;
        if (fDb > -70.0f)
        {
            const float attackDb = std::max (0.0f, fDb - gainToDb (s));
            // sustain only acts on the decay, never on the next onset
            const float sustainDb = std::max (0.0f, gainToDb (h) - fDb) * clampf (1.0f - attackDb / 10.0f, 0.0f, 1.0f);
            target = clampf (atk * attackDb + sus * sustainDb, -18.0f, 18.0f);
        }
        // falling gain is fast, so a sustain boost never spills onto the next hit
        gainDb = target + (target < gainDb ? smoothDown : smooth) * (gainDb - target);
        if (std::abs (gainDb) > std::abs (shown)) shown = gainDb;
        const float g = dbToGain (gainDb);
        const float m = mixSm.next (clampf (params.mix, 0.0f, 1.0f));
        const float w = 1.0f + m * (g - 1.0f);
        l[i] *= w;
        r[i] *= w;
    }
    shownDb = shown;
}

//==============================================================================
// Drive
float DriveModule::shape (DriveType t, float x) noexcept
{
    switch (t)
    {
        case DriveType::tape:        return std::tanh (x + 0.1f) - 0.09966800f;
        case DriveType::tube:        return x >= 0.0f ? std::tanh (x) : std::tanh (0.6f * x) / 0.6f;
        case DriveType::transformer: return x / std::sqrt (1.0f + x * x);
        case DriveType::transistor:  return x / std::sqrt (std::sqrt (1.0f + x * x * x * x));
        case DriveType::numTypes: break;
    }
    return x;
}

void DriveModule::prepare (double sampleRate)
{
    sr = sampleRate;
    for (auto& d : dry)
    {
        d.prepare (getLatency() + 4);
        d.setDelay (getLatency());
    }
    for (size_t c = 0; c < 2; ++c)
    {
        dcBlock[c].setHighPass (sr, 10.0, 0.70710678);
        xfLow[c].setLowShelf (sr * 2.0, 120.0, 6.0, 0.7);
        tapeTop[c].setLowPass (sr, std::min (16000.0, sr * 0.45), 0.6);
    }
    for (auto* s : { &driveSm, &compSm, &mixSm, &outSm })
        s->setTime (sr, 25.0f);
    lastTone = 100.0f;
    lastDrive = -100.0f;
    lastType = DriveType::numTypes;
    setParams (params);
    reset();
}

void DriveModule::reset()
{
    for (auto& o : os) o.reset();
    for (auto& d : dry) d.reset();
    for (auto* arr : { &dcBlock, &toneLow, &toneHigh, &xfLow, &tapeTop })
        for (auto& b : *arr) b.reset();
    driveSm.snap (dbToGain (params.driveDb));
    compSm.snap (comp);
    mixSm.snap (params.mix);
    outSm.snap (dbToGain (params.outputDb));
}

void DriveModule::setParams (const DriveParams& p) noexcept
{
    if (std::abs (p.tone - lastTone) > 1.0e-4f)
    {
        lastTone = p.tone;
        for (size_t c = 0; c < 2; ++c)
        {
            toneLow[c].setLowShelf (sr, 250.0, -p.tone * 4.0);
            toneHigh[c].setHighShelf (sr, 3000.0, p.tone * 4.0);
        }
    }
    if (p.driveDb != lastDrive || p.type != lastType)
    {
        // Keeps a -12 dBFS peak at about the same level whatever the drive
        lastDrive = p.driveDb;
        lastType = p.type;
        const float g = dbToGain (p.driveDb), x = 0.25f;
        const float y = 0.5f * (shape (p.type, g * x) - shape (p.type, -g * x));
        comp = y > 1.0e-6f ? x / y : 1.0f;
    }
    params = p;
}

void DriveModule::process (float* l, float* r, int n) noexcept
{
    float* chans[2] = { l, r };
    if (! params.enabled)
    {
        for (size_t c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                chans[c][i] = dry[c].process (chans[c][i]);
        return;
    }

    const auto type = params.type;
    for (int i = 0; i < n; ++i)
    {
        const float g = driveSm.next (dbToGain (params.driveDb));
        const float cmp = compSm.next (comp);
        const float m = mixSm.next (clampf (params.mix, 0.0f, 1.0f));
        const float og = outSm.next (dbToGain (params.outputDb));
        for (size_t c = 0; c < 2; ++c)
        {
            const float x = chans[c][i];
            const float d = dry[c].process (x);
            float u0, u1;
            os[c].upsample (x * g, u0, u1);
            if (type == DriveType::transformer)
            {
                // the low end saturates first: emphasise it before the curve, take it back afterwards
                const float e0 = xfLow[c].process (u0), e1 = xfLow[c].process (u1);
                u0 = shape (type, e0) - (e0 - u0);
                u1 = shape (type, e1) - (e1 - u1);
                u0 = clampf (u0, -2.0f, 2.0f);
                u1 = clampf (u1, -2.0f, 2.0f);
            }
            else
            {
                u0 = shape (type, u0);
                u1 = shape (type, u1);
            }
            float w = os[c].downsample (u0, u1) * cmp;
            w = dcBlock[c].process (w);
            if (type == DriveType::tape) w = tapeTop[c].process (w);
            if (std::abs (params.tone) > 1.0e-3f) w = toneHigh[c].process (toneLow[c].process (w));
            chans[c][i] = (d + m * (w - d)) * og;
        }
    }
}
} // namespace lacs
