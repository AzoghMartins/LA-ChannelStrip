// Headless tests for the LA-ChannelStrip DSP engine (no JUCE needed).
#include "dsp/StripEngine.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace lacs;

namespace
{
int failures = 0;
void check (bool ok, const std::string& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (! ok)
        ++failures;
}

std::string num (double v, int decimals = 2)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.*f", decimals, v);
    return buf;
}

constexpr double sr = 48000.0;
constexpr int block = 256;

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
};

Stereo sine (double hz, double seconds, float amp)
{
    Stereo s;
    const size_t n = (size_t) (seconds * sr);
    s.l.resize (n);
    s.r.resize (n);
    for (size_t i = 0; i < n; ++i)
        s.l[i] = s.r[i] = amp * (float) std::sin (2.0 * kPi * hz * (double) i / sr);
    return s;
}

Stereo silence (double seconds)
{
    Stereo s;
    s.l.assign ((size_t) (seconds * sr), 0.0f);
    s.r = s.l;
    return s;
}

Stereo noise (double seconds, float amp, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    Stereo s;
    const size_t n = (size_t) (seconds * sr);
    s.l.resize (n);
    s.r.resize (n);
    for (size_t i = 0; i < n; ++i)
    {
        s.l[i] = amp * nd (rng);
        s.r[i] = amp * nd (rng);
    }
    return s;
}

/** Decaying drum-like hits (sine bursts with a sharp attack) every 250 ms. */
Stereo hits (double seconds, float amp)
{
    Stereo s;
    const size_t n = (size_t) (seconds * sr);
    s.l.resize (n);
    s.r.resize (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = std::fmod ((double) i / sr, 0.25);
        const float v = amp * (float) (std::exp (-t * 18.0) * std::sin (2.0 * kPi * 180.0 * t));
        s.l[i] = s.r[i] = v;
    }
    return s;
}

void run (StripEngine& e, Stereo& s, const Stereo* sc = nullptr, DynListener* listener = nullptr)
{
    for (size_t start = 0; start < s.size(); start += block)
    {
        const int n = (int) std::min ((size_t) block, s.size() - start);
        float* io[2] = { s.l.data() + start, s.r.data() + start };
        const float* side[2] = { nullptr, nullptr };
        if (sc != nullptr)
        {
            side[0] = sc->l.data() + start;
            side[1] = sc->r.data() + start;
        }
        e.process (io, 2, n, sc != nullptr ? side : nullptr, listener);
    }
}

StripEngine makeEngine (const EngineParams& p)
{
    StripEngine e;
    e.setParams (p);
    e.prepare (sr, block);
    return e;
}

double rmsDb (const std::vector<float>& v, size_t from = 0, size_t to = 0)
{
    if (to == 0 || to > v.size()) to = v.size();
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += (double) v[i] * v[i];
    return 10.0 * std::log10 (acc / (double) std::max<size_t> (1, to - from) + 1.0e-20);
}

/** Amplitude (linear) of the component at `hz` (single-bin DFT over whole cycles). */
double amplitudeAt (const std::vector<float>& v, double hz, size_t from, size_t count)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const double ph = 2.0 * kPi * hz * (double) (from + i) / sr;
        re += v[from + i] * std::cos (ph);
        im += v[from + i] * std::sin (ph);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (double) count;
}

/** THD (fraction) of a 1 kHz tone: harmonics 2..7 relative to the fundamental. */
double thd (const std::vector<float>& v, double f0)
{
    const size_t from = v.size() / 2, count = (size_t) sr / 10; // 100 ms = whole cycles of 1 kHz
    const double fund = amplitudeAt (v, f0, from, count);
    double h = 0.0;
    for (int k = 2; k <= 7; ++k)
    {
        const double a = amplitudeAt (v, f0 * k, from, count);
        h += a * a;
    }
    return std::sqrt (h) / std::max (1.0e-12, fund);
}

/** Steady-state gain (dB) of the engine for a sine at f. */
double gainAt (const EngineParams& p, double f, float amp = 0.05f, int side = 0)
{
    auto e = makeEngine (p);
    auto s = sine (f, 0.6, amp);
    run (e, s);
    const size_t from = (size_t) (0.3 * sr);
    const size_t count = (size_t) (0.25 * sr);
    const auto& v = side == 0 ? s.l : s.r;
    return 20.0 * std::log10 (amplitudeAt (v, f, from, count) / amp);
}

bool allFinite (const Stereo& s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (! std::isfinite (s.l[i]) || ! std::isfinite (s.r[i]))
            return false;
    return true;
}

EngineParams analogue (ConsoleType t = ConsoleType::british)
{
    EngineParams p;
    p.console.analogue = true;
    p.console.type = t;
    return p;
}
} // namespace

int main()
{
    std::printf ("LA-ChannelStrip engine tests\n");

    // ---- Transparency and latency ------------------------------------------------------------------------
    {
        EngineParams p;
        auto e = makeEngine (p);
        auto in = noise (1.0, 0.2f);
        auto out = in;
        run (e, out);
        const int lat = e.getLatency();
        float err = 0.0f;
        for (size_t i = (size_t) lat; i < in.size(); ++i)
            err = std::max ({ err, std::abs (out.l[i] - in.l[i - (size_t) lat]), std::abs (out.r[i] - in.r[i - (size_t) lat]) });
        check (err < 1.0e-5f, "Digital mode at default settings is transparent (latency-aligned, max error " + num (err, 7) + ")");
        check (lat == DriveModule::getLatency() && lat > 0 && lat < 64, "latency is " + std::to_string (lat) + " samples (" + num (1000.0 * lat / sr) + " ms)");

        auto pa = analogue();
        pa.drive.enabled = true;
        pa.comp.enabled = true;
        auto ea = makeEngine (pa);
        check (ea.getLatency() == lat, "latency does not change with mode or settings");
    }

    // ---- Analogue console ----------------------------------------------------------------------------------
    {
        EngineParams pd;
        auto ed = makeEngine (pd);
        auto sd = sine (1000.0, 0.5, 0.125f); // -18 dBFS peak
        run (ed, sd);
        const double thdDigital = thd (sd.l, 1000.0);

        auto pa = analogue();
        auto ea = makeEngine (pa);
        auto sa = sine (1000.0, 0.5, 0.125f);
        run (ea, sa);
        const double thdAnalogue = thd (sa.l, 1000.0);
        check (thdDigital < 1.0e-5 && thdAnalogue > 2.0e-4 && thdAnalogue < 0.01,
               "Analogue mode adds gentle harmonics at -18 dBFS (THD " + num (thdAnalogue * 100.0, 3) + " %, digital " + num (thdDigital * 100.0, 5) + " %)");

        pa.console.drive = 1.0f;
        auto eh = makeEngine (pa);
        auto sh = sine (1000.0, 0.5, 0.5f);
        run (eh, sh);
        const double thdHot = thd (sh.l, 1000.0);
        check (thdHot > thdAnalogue * 5.0, "driving the desk harder at a hotter level gives more harmonics (" + num (thdHot * 100.0, 2) + " %)");

        // voicing balance: British is mostly odd, Vintage has more 2nd harmonic
        auto h2ratio = [] (ConsoleType t)
        {
            auto p = analogue (t);
            p.console.drive = 0.7f;
            auto e = makeEngine (p);
            auto s = sine (1000.0, 0.5, 0.3f);
            run (e, s);
            const size_t from = s.size() / 2, count = (size_t) sr / 10;
            return amplitudeAt (s.l, 2000.0, from, count) / amplitudeAt (s.l, 3000.0, from, count);
        };
        const double brit = h2ratio (ConsoleType::british), vint = h2ratio (ConsoleType::vintage);
        check (vint > brit * 2.0, "Vintage voicing has a stronger 2nd harmonic than British (H2/H3 " + num (vint) + " vs " + num (brit) + ")");

        auto pn = analogue();
        pn.console.noiseDb = -80.0f;
        auto en = makeEngine (pn);
        auto sn = silence (1.0);
        run (en, sn);
        const double nl = rmsDb (sn.l, 4800), nr = rmsDb (sn.r, 4800);
        float corr = 0.0f;
        double ll = 0, rr = 0, lr = 0;
        for (size_t i = 4800; i < sn.size(); ++i) { ll += sn.l[i] * sn.l[i]; rr += sn.r[i] * sn.r[i]; lr += sn.l[i] * sn.r[i]; }
        corr = (float) (lr / std::sqrt (ll * rr + 1.0e-30));
        check (std::abs (nl + 80.0) < 2.0 && std::abs (nr + 80.0) < 2.0 && std::abs (corr) < 0.1f,
               "noise floor at -80 dBFS, independent per side (" + num (nl, 1) + " / " + num (nr, 1) + " dB, corr " + num (corr) + ")");
        auto sq = silence (0.5);
        auto eq0 = makeEngine (EngineParams {});
        run (eq0, sq);
        check (rmsDb (sq.l) < -199.0, "Digital mode adds no noise");
    }

    // ---- Tolerances ---------------------------------------------------------------------------------------
    {
        auto p = analogue();
        p.eq.gain[1] = 10.0f;
        const double l1 = gainAt (p, 400.0, 0.02f, 0), r1 = gainAt (p, 400.0, 0.02f, 1);
        p.console.channel = 2;
        const double l2 = gainAt (p, 400.0, 0.02f, 0);
        EngineParams d;
        d.eq.gain[1] = 10.0f;
        const double dl = gainAt (d, 400.0, 0.02f, 0), dr = gainAt (d, 400.0, 0.02f, 1);
        check (std::abs (l1 - r1) > 0.01 && std::abs (l1 - l2) > 0.01 && std::abs (dl - dr) < 1.0e-4,
               "Analogue channels have small tolerances (ch1 L/R " + num (l1) + " / " + num (r1) + " dB, ch2 L " + num (l2) + " dB); Digital is exact");
        const auto t1 = Tolerance::forChannel (7), t2 = Tolerance::forChannel (7);
        check (t1.eqGain == t2.eqGain && t1.thresholdDb == t2.thresholdDb, "tolerances are repeatable for a channel number");
    }

    // ---- Filters ------------------------------------------------------------------------------------------
    {
        EngineParams p;
        p.filters.hpfHz = 100.0f;
        const double at50 = gainAt (p, 50.0), at400 = gainAt (p, 400.0);
        check (std::abs (at50 + 18.1) < 1.0 && std::abs (at400) < 0.1, "high-pass is 18 dB/oct (" + num (at50, 1) + " dB an octave below)");
        EngineParams q;
        q.filters.lpfHz = 2000.0f;
        const double at4k = gainAt (q, 4000.0), at500 = gainAt (q, 500.0);
        check (std::abs (at4k + 12.3) < 1.0 && std::abs (at500) < 0.1, "low-pass is 12 dB/oct (" + num (at4k, 1) + " dB an octave above)");
        auto a = analogue();
        a.filters.hpfHz = 100.0f;
        const double bump = gainAt (a, 150.0);
        check (bump > 0.2, "analogue high-pass has a slight resonance above the cut-off (+" + num (bump) + " dB)");
    }

    // ---- EQ -----------------------------------------------------------------------------------------------
    {
        auto band = [] (int b, float f, float g, double at)
        {
            EngineParams e;
            e.eq.freq[(size_t) b] = f;
            e.eq.gain[(size_t) b] = g;
            return gainAt (e, at);
        };
        const double g1k = band (2, 1000.0f, 6.0f, 1000.0), g200 = band (1, 200.0f, -4.0f, 200.0);
        const double g20 = band (0, 80.0f, 3.0f, 25.0), g18k = band (3, 6000.0f, -5.0f, 18000.0);
        check (std::abs (g1k - 6.0) < 0.1 && std::abs (g200 + 4.0) < 0.1 && std::abs (g20 - 3.0) < 0.3 && std::abs (g18k + 5.0) < 0.3,
               "Digital EQ bands hit their gains (mid " + num (g1k) + ", low-mid " + num (g200) + ", LF shelf " + num (g20) + ", HF shelf " + num (g18k) + ")");

        EngineParams p;
        p.eq.freq[0] = 80.0f;
        p.eq.gain[0] = 3.0f;
        p.eq.lfBell = true;
        p.eq.q[0] = 2.0f;
        const double lfBellFar = gainAt (p, 25.0);
        check (std::abs (lfBellFar) < 1.0, "LF band switches to a bell (" + num (lfBellFar) + " dB two octaves below)");

        // Proportional Q: an octave away from the centre, a small boost is relatively wider than a big one
        auto relWidth = [] (bool analog, float gain)
        {
            EngineParams e = analog ? analogue() : EngineParams {};
            e.eq.freq[2] = 1000.0f;
            e.eq.gain[2] = gain;
            e.eq.q[2] = 1.0f;
            return gainAt (e, 2000.0) / gain;
        };
        const double aSmall = relWidth (true, 3.0f), aBig = relWidth (true, 15.0f);
        const double dSmall = relWidth (false, 3.0f), dBig = relWidth (false, 15.0f);
        check (aSmall > aBig + 0.05 && std::abs (dSmall - dBig) < 0.05,
               "Analogue bells have proportional Q (" + num (aSmall) + " vs " + num (aBig) + "), Digital constant Q (" + num (dSmall) + " vs " + num (dBig) + ")");

        auto shelf = analogue();
        shelf.eq.freq[0] = 100.0f;
        shelf.eq.gain[0] = 10.0f;
        const double dip = gainAt (shelf, 260.0);
        EngineParams dshelf;
        dshelf.eq.freq[0] = 100.0f;
        dshelf.eq.gain[0] = 10.0f;
        const double nodip = gainAt (dshelf, 260.0);
        check (dip < nodip - 0.5, "Analogue LF shelf has the classic dip above the boost (" + num (dip) + " vs " + num (nodip) + " dB at 260 Hz)");

        // display curve matches processing
        const double disp = EqModule::responseDb (sr, shelf.eq, shelf.console, Tolerance::forChannel (1), 260.0);
        check (std::abs (disp - dip) < 0.15, "EQ display response matches the processed curve");
    }

    // ---- Gate ---------------------------------------------------------------------------------------------
    {
        EngineParams p;
        p.gate.enabled = true;
        p.gate.thresholdDb = -40.0f;
        p.gate.rangeDb = 40.0f;
        auto e = makeEngine (p);
        auto quiet = sine (300.0, 0.6, dbToGain (-55.0f));
        run (e, quiet);
        const double q = rmsDb (quiet.l, (size_t) (0.4 * sr));
        auto loud = sine (300.0, 0.6, dbToGain (-20.0f));
        run (e, loud);
        const double ld = rmsDb (loud.l, (size_t) (0.4 * sr));
        check (std::abs (q - (-58.0 - 40.0)) < 2.0 && std::abs (ld + 23.0) < 0.3,
               "gate closes below the threshold (" + num (q, 1) + " dB) and opens above it (" + num (ld, 1) + " dB)");

        // Hysteresis: after opening, a level between threshold - hysteresis and threshold keeps it open
        p.gate.hysteresisDb = 8.0f;
        p.gate.holdMs = 0.0f;
        auto eh = makeEngine (p);
        auto open = sine (300.0, 0.3, dbToGain (-30.0f));
        run (eh, open);
        auto mid = sine (300.0, 0.5, dbToGain (-44.0f)); // peak -44: below thr, above thr - 8
        run (eh, mid);
        const double kept = rmsDb (mid.l, (size_t) (0.3 * sr));
        check (std::abs (kept + 47.0) < 0.5, "hysteresis keeps the gate open just below the threshold (" + num (kept, 1) + " dB)");

        p.gate.expander = true;
        p.gate.hysteresisDb = 0.0f;
        auto ex = makeEngine (p);
        auto s = sine (300.0, 0.6, dbToGain (-50.0f));
        run (ex, s);
        const double exp = rmsDb (s.l, (size_t) (0.4 * sr));
        check (std::abs (exp - (-53.0 - 10.0)) < 1.5, "expander mode attenuates 1:2 below the threshold (" + num (exp, 1) + " dB)");

        // listen outputs the key filter
        EngineParams lp;
        lp.gate.enabled = true;
        lp.gate.listen = true;
        lp.gate.keyHpfHz = 2000.0f;
        const double low = gainAt (lp, 200.0, 0.3f);
        check (low < -30.0, "gate key listen plays the filtered key (" + num (low, 1) + " dB at 200 Hz)");
    }

    // ---- Compressor ---------------------------------------------------------------------------------------
    {
        auto staticGr = [] (EngineParams p)
        {
            p.comp.enabled = true;
            p.comp.thresholdDb = -20.0f;
            p.comp.ratio = 4.0f;
            p.comp.kneeDb = 0.0f;
            p.comp.attackMs = 5.0f;
            p.comp.releaseMs = 100.0f;
            return gainAt (p, 1000.0, dbToGain (-8.0f));
        };
        const double ff = staticGr (EngineParams {});
        const double fb = staticGr (analogue());
        check (std::abs (ff + 9.0) < 0.6, "Digital (feed-forward) compressor: 4:1, 12 dB over -> " + num (ff) + " dB");
        check (std::abs (fb + 9.0) < 1.5, "Analogue (feedback) compressor reaches the same static ratio (" + num (fb) + " dB)");

        bool stable = true;
        for (int c = 0; c < (int) CompCharacter::numCharacters; ++c)
            for (int mode = 0; mode < 2; ++mode)
            {
                EngineParams p = mode == 1 ? analogue() : EngineParams {};
                p.comp.enabled = true;
                p.comp.character = (CompCharacter) c;
                p.comp.thresholdDb = -40.0f;
                p.comp.ratio = 20.0f;
                p.comp.attackMs = 0.05f;
                p.comp.releaseMs = 5.0f;
                p.comp.autoMakeup = true;
                auto e = makeEngine (p);
                auto s = hits (1.0, 0.9f);
                run (e, s);
                float peak = 0.0f;
                for (size_t i = 0; i < s.size(); ++i) peak = std::max (peak, std::abs (s.l[i]));
                stable &= allFinite (s) && peak < 4.0f;
            }
        check (stable, "all four characters are stable at extreme settings in both modes");

        // character static curves differ: vari-mu ratio grows with level
        const float lowR = CompressorModule::effectiveRatio (CompCharacter::varimu, 8.0f, 0.0f);
        const float highR = CompressorModule::effectiveRatio (CompCharacter::varimu, 8.0f, 10.0f);
        check (highR > lowR * 1.5f, "Vari-Mu ratio grows with level (" + num (lowR) + " -> " + num (highR) + ")");

        // Auto makeup brings the average level back up
        EngineParams am;
        am.comp.enabled = true;
        am.comp.thresholdDb = -30.0f;
        am.comp.ratio = 4.0f;
        am.comp.autoMakeup = true;
        auto e = makeEngine (am);
        auto s = sine (500.0, 4.0, dbToGain (-12.0f));
        run (e, s);
        const double after = rmsDb (s.l, (size_t) (3.0 * sr));
        check (std::abs (after - (-15.0)) < 1.5, "auto makeup restores the level (" + num (after, 1) + " dB RMS, input -15)");

        // External key
        EngineParams ek;
        ek.comp.enabled = true;
        ek.comp.thresholdDb = -30.0f;
        ek.comp.ratio = 10.0f;
        ek.comp.key = KeySource::external;
        auto e1 = makeEngine (ek);
        auto a = sine (500.0, 0.6, 0.1f);
        auto quietKey = silence (0.6);
        run (e1, a, &quietKey);
        auto e2 = makeEngine (ek);
        auto b = sine (500.0, 0.6, 0.1f);
        auto loudKey = sine (60.0, 0.6, 0.8f);
        run (e2, b, &loudKey);
        const double free = rmsDb (a.l, 12000), ducked = rmsDb (b.l, 12000);
        check (std::abs (free + 23.0) < 0.3 && ducked < free - 15.0, "external side-chain keys the compressor (" + num (free, 1) + " -> " + num (ducked, 1) + " dB)");

        // EQ to the side-chain: EQ leaves the audio path and drives the detector instead
        EngineParams sc;
        sc.comp.enabled = true;
        sc.comp.thresholdDb = -24.0f;
        sc.comp.ratio = 4.0f;
        sc.comp.key = KeySource::eq;
        sc.eq.freq[2] = 5000.0f;
        sc.eq.gain[2] = 15.0f;
        const double lowTone = gainAt (sc, 200.0, dbToGain (-26.0f));
        const double highTone = gainAt (sc, 5000.0, dbToGain (-26.0f));
        check (std::abs (lowTone) < 0.3 && highTone < -5.0,
               "EQ as side-chain: the boosted band triggers compression, the EQ itself is not heard (" + num (lowTone) + " / " + num (highTone) + " dB)");
    }

    // ---- De-esser -------------------------------------------------------------------------------------------
    {
        EngineParams p;
        p.deess.enabled = true;
        p.deess.freqHz = 6000.0f;
        p.deess.thresholdDb = -30.0f;
        p.deess.rangeDb = 10.0f;
        const double ess = gainAt (p, 8000.0, 0.3f), body = gainAt (p, 500.0, 0.3f);
        check (ess < -6.0 && std::abs (body) < 0.3, "split-band de-esser reduces 8 kHz (" + num (ess, 1) + " dB) and leaves 500 Hz (" + num (body) + " dB)");
    }

    // ---- Transient shaper ------------------------------------------------------------------------------------
    {
        auto crest = [] (float attack, float sustain)
        {
            EngineParams p;
            p.transient.enabled = true;
            p.transient.attack = attack;
            p.transient.sustain = sustain;
            auto e = makeEngine (p);
            auto s = hits (2.0, 0.3f);
            run (e, s);
            float peak = 0.0f;
            for (size_t i = s.size() / 2; i < s.size(); ++i) peak = std::max (peak, std::abs (s.l[i]));
            return 20.0 * std::log10 (peak) - rmsDb (s.l, s.size() / 2);
        };
        const double flat = crest (0.0f, 0.0f), punchy = crest (1.0f, 0.0f), soft = crest (-1.0f, 0.0f), longer = crest (0.0f, 1.0f);
        check (punchy > flat + 1.5 && soft < flat - 1.0 && longer < flat - 1.0,
               "transient shaper: crest factor " + num (flat, 1) + " dB -> attack+ " + num (punchy, 1) + ", attack- " + num (soft, 1) + ", sustain+ " + num (longer, 1));
    }

    // ---- Drive ----------------------------------------------------------------------------------------------
    {
        bool levels = true, harmonics = true;
        std::string info;
        for (int t = 0; t < (int) DriveType::numTypes; ++t)
        {
            EngineParams p;
            p.drive.enabled = true;
            p.drive.type = (DriveType) t;
            p.drive.driveDb = 12.0f;
            auto e = makeEngine (p);
            auto s = sine (1000.0, 0.5, 0.25f);
            run (e, s);
            const double lvl = 20.0 * std::log10 (amplitudeAt (s.l, 1000.0, s.size() / 2, 4800) / 0.25);
            const double h = thd (s.l, 1000.0);
            levels &= std::abs (lvl) < 1.5;
            harmonics &= h > 0.01;
            info += " " + num (lvl, 1) + "dB/" + num (h * 100.0, 1) + "%";
        }
        check (levels && harmonics, "all drive types add harmonics and stay level-matched at -12 dBFS (" + info + " )");

        EngineParams p;
        p.drive.enabled = true;
        p.drive.driveDb = 18.0f;
        p.drive.mix = 0.0f;
        auto e = makeEngine (p);
        auto in = noise (0.5, 0.2f);
        auto out = in;
        run (e, out);
        float err = 0.0f;
        const int lat = e.getLatency();
        for (size_t i = 4800; i < in.size(); ++i) err = std::max (err, std::abs (out.l[i] - in.l[i - (size_t) lat]));
        check (err < 1.0e-4f, "drive mix 0 % is the clean, latency-aligned signal");
    }

    // ---- Output ----------------------------------------------------------------------------------------------
    {
        EngineParams p;
        p.mix = 0.0f;
        p.eq.gain[2] = 12.0f;
        p.comp.enabled = true;
        auto e = makeEngine (p);
        auto in = noise (0.5, 0.2f);
        auto out = in;
        run (e, out);
        float err = 0.0f;
        const int lat = e.getLatency();
        for (size_t i = 4800; i < in.size(); ++i) err = std::max (err, std::abs (out.l[i] - in.l[i - (size_t) lat]));
        check (err < 1.0e-5f, "global mix 0 % is the dry, latency-aligned input");

        EngineParams w;
        w.width = 0.0f;
        auto ew = makeEngine (w);
        auto s = noise (0.3, 0.2f);
        run (ew, s);
        float diff = 0.0f;
        for (size_t i = 2400; i < s.size(); ++i) diff = std::max (diff, std::abs (s.l[i] - s.r[i]));
        check (diff < 1.0e-6f, "width 0 % folds to mono");

        EngineParams pan;
        pan.pan = -1.0f;
        auto ep = makeEngine (pan);
        auto sp = noise (0.3, 0.2f);
        run (ep, sp);
        check (rmsDb (sp.r, 2400) < -100.0 && rmsDb (sp.l, 2400) > -20.0, "pan hard left silences the right side");

        EngineParams mono;
        mono.monoSource = true;
        const double centre = gainAt (mono, 1000.0, 0.1f, 0);
        mono.pan = -1.0f;
        const double hardL = gainAt (mono, 1000.0, 0.1f, 0);
        check (std::abs (centre) < 0.05 && std::abs (hardL - 3.01) < 0.1, "mono sources pan with constant power (centre " + num (centre) + ", hard left +" + num (hardL) + " dB)");

        EngineParams g;
        g.inputGainDb = 6.0f;
        g.outputGainDb = -3.0f;
        g.polarity = true;
        auto eg = makeEngine (g);
        auto sg = sine (1000.0, 0.3, 0.1f);
        auto ref = sg;
        run (eg, sg);
        const int lat2 = eg.getLatency();
        double corr = 0.0;
        for (size_t i = 4800; i < sg.size(); ++i) corr += (double) sg.l[i] * ref.l[i - (size_t) lat2];
        check (std::abs (gainAt (g, 1000.0) - 3.0) < 0.05 && corr < 0.0, "input trim, output gain and polarity");
    }

    // ---- Module order ------------------------------------------------------------------------------------------
    {
        ModuleOrder o = { 5, 4, 3, 2, 1, 0 };
        char buf[kNumOrderable + 1];
        orderToString (o, buf);
        check (orderFromString (buf) == o && orderFromString ("001234") == defaultModuleOrder() && orderFromString ("12") == defaultModuleOrder(),
               "module order string round trip and validation");

        auto render = [] (ModuleOrder order)
        {
            EngineParams p;
            p.order = order;
            p.comp.enabled = true;
            p.comp.thresholdDb = -30.0f;
            p.comp.ratio = 8.0f;
            p.eq.freq[2] = 1000.0f;
            p.eq.gain[2] = 12.0f;
            return gainAt (p, 1000.0, 0.1f);
        };
        ModuleOrder eqFirst = defaultModuleOrder();
        std::swap (eqFirst[2], eqFirst[3]); // EQ before the compressor
        const double compThenEq = render (defaultModuleOrder()), eqThenComp = render (eqFirst);
        check (compThenEq > eqThenComp + 3.0, "order matters: EQ boost after the compressor " + num (compThenEq, 1) + " dB, before it " + num (eqThenComp, 1) + " dB");
    }

    // ---- Dynamics history frames ---------------------------------------------------------------------------------
    {
        struct Counter : DynListener
        {
            int frames = 0;
            float maxGr = 0.0f;
            void dynFrame (const DynFrame& f) override { ++frames; maxGr = std::max (maxGr, f.compGrDb); }
        } counter;
        EngineParams p;
        p.comp.enabled = true;
        p.comp.thresholdDb = -30.0f;
        auto e = makeEngine (p);
        auto s = sine (200.0, 1.0, 0.5f);
        run (e, s, nullptr, &counter);
        check (counter.frames == 100 && counter.maxGr > 5.0f, "dynamics history gets a frame every 10 ms (" + std::to_string (counter.frames) + " per second)");
    }

    // ---- Assistant statistics ---------------------------------------------------------------------------------------
    {
        EngineParams p;
        auto e = makeEngine (p);
        e.setStatsEnabled (true);
        // phrases at about -27 dBFS RMS with a -75 dBFS noise floor in the gaps
        Stereo s;
        std::mt19937 rng (5);
        std::normal_distribution<float> nd (0.0f, 1.0f);
        const size_t n = (size_t) (8.0 * sr);
        s.l.resize (n);
        s.r.resize (n);
        for (size_t i = 0; i < n; ++i)
        {
            const bool phrase = std::fmod ((double) i / sr, 2.0) < 1.4;
            const float v = phrase ? 0.0447f * (float) std::sin (2.0 * kPi * 220.0 * (double) i / sr) * 1.41421f : 0.0f;
            s.l[i] = s.r[i] = v + dbToGain (-75.0f) * nd (rng);
        }
        run (e, s);
        e.setStatsEnabled (false);
        const auto r = analyseStats (e.getStats(), 4.0f);
        std::printf ("        %s\n", r.summary);
        check (r.ok && std::abs (r.trimDb - 9.0f) < 1.0f && r.gateOn && r.gateThresholdDb > -63.0f + 12.0f - 30.0f && r.gateThresholdDb < -30.0f + 12.0f - 6.0f,
               "assistant: gain staging to -18 dBFS RMS (trim " + num (r.trimDb, 1) + " dB) and a gate between the floor and the phrases ("
                   + num (r.gateThresholdDb, 1) + " dB)");
        AnalysisStats empty;
        check (! analyseStats (empty, 4.0f).ok, "assistant refuses to guess from silence");
    }

    // ---- Robustness and speed -------------------------------------------------------------------------------------------
    {
        std::mt19937 rng (11);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        bool finite = true;
        for (int trial = 0; trial < 40; ++trial)
        {
            EngineParams p;
            p.console.analogue = u (rng) > 0.5f;
            p.console.type = (ConsoleType) (int) (u (rng) * 2.99f);
            p.console.drive = u (rng);
            p.console.noiseDb = u (rng) > 0.5f ? -60.0f : -120.0f;
            p.console.channel = 1 + (int) (u (rng) * 71.0f);
            p.inputGainDb = -24.0f + 48.0f * u (rng);
            p.filters.hpfHz = 16.0f + 500.0f * u (rng);
            p.filters.lpfHz = 1500.0f + 21000.0f * u (rng);
            p.gate.enabled = u (rng) > 0.5f;
            p.gate.expander = u (rng) > 0.5f;
            p.gate.thresholdDb = -80.0f * u (rng);
            p.gate.attackMs = 0.05f + 50.0f * u (rng);
            p.transient.enabled = u (rng) > 0.5f;
            p.transient.attack = 2.0f * u (rng) - 1.0f;
            p.transient.sustain = 2.0f * u (rng) - 1.0f;
            p.comp.enabled = u (rng) > 0.3f;
            p.comp.character = (CompCharacter) (int) (u (rng) * 3.99f);
            p.comp.ratio = 1.0f + 19.0f * u (rng);
            p.comp.thresholdDb = -60.0f * u (rng);
            p.comp.attackMs = 0.05f + 100.0f * u (rng);
            p.comp.releaseMs = 5.0f + 2000.0f * u (rng);
            p.comp.autoRelease = u (rng) > 0.5f;
            p.comp.autoMakeup = u (rng) > 0.5f;
            p.comp.key = (KeySource) (int) (u (rng) * 2.99f);
            for (int b = 0; b < kEqBands; ++b)
            {
                p.eq.gain[(size_t) b] = -18.0f + 36.0f * u (rng);
                p.eq.q[(size_t) b] = 0.3f + 5.7f * u (rng);
            }
            p.eq.freq = { 20.0f + 580.0f * u (rng), 60.0f + 2940.0f * u (rng), 500.0f + 15500.0f * u (rng), 1500.0f + 18500.0f * u (rng) };
            p.deess.enabled = u (rng) > 0.5f;
            p.drive.enabled = u (rng) > 0.5f;
            p.drive.type = (DriveType) (int) (u (rng) * 3.99f);
            p.drive.driveDb = 24.0f * u (rng);
            p.drive.tone = 2.0f * u (rng) - 1.0f;
            p.width = 2.0f * u (rng);
            p.pan = 2.0f * u (rng) - 1.0f;
            std::shuffle (p.order.begin(), p.order.end(), rng);
            auto e = makeEngine (p);
            auto s = noise (0.25, 0.5f, (unsigned) trial);
            auto sc = noise (0.25, 0.5f, (unsigned) trial + 100u);
            run (e, s, &sc);
            finite &= allFinite (s);
        }
        check (finite, "40 random settings (orders, modes, extremes) all produce finite output");

        auto p = analogue();
        p.console.noiseDb = -90.0f;
        p.gate.enabled = p.transient.enabled = p.comp.enabled = p.deess.enabled = p.drive.enabled = true;
        p.comp.character = CompCharacter::opto;
        p.filters.hpfHz = 80.0f;
        for (auto& g : p.eq.gain) g = 3.0f;
        auto e = makeEngine (p);
        auto s = noise (10.0, 0.2f);
        const auto t0 = std::chrono::steady_clock::now();
        run (e, s);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        check (secs < 2.0, "everything on, analogue: 10 s of stereo audio in " + num (secs * 1000.0, 0) + " ms (" + num (10.0 / secs, 0) + "x real time)");
    }

    std::printf (failures == 0 ? "All engine tests passed\n" : "%d engine test(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
