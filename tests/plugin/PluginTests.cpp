// Friction - plugin-level tests (JUCE AudioProcessor + editor), headless
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "AllocCounter.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestUtil.h"

#include <cstdio>
#include <fstream>
#include <random>

using friction::FrictionAudioProcessor;

namespace
{

struct Row { bool pass; std::string name, measured, criterion; };
std::vector<Row> rows;

void add (bool pass, const std::string& name, const std::string& measured, const std::string& criterion)
{
    rows.push_back ({ pass, name, measured, criterion });
    std::printf ("[%s] Plugin: %-50s %s | %s\n", pass ? "PASS" : "FAIL", name.c_str(), measured.c_str(), criterion.c_str());
    std::fflush (stdout);
}

std::string fmt (const char* f, double a = 0, double b = 0, double c = 0, double d = 0)
{
    char buf[512];
    std::snprintf (buf, sizeof (buf), f, a, b, c, d);
    return buf;
}

/** Processes `seconds` of a stereo target + stereo sidechain through the processor. Returns main output. */
std::vector<ft::Buf> run (FrictionAudioProcessor& p, const std::vector<ft::Buf>& tgt, const std::vector<ft::Buf>& sc, int block)
{
    const int n = (int) tgt[0].size();
    const int totalIn = p.getTotalNumInputChannels();
    juce::AudioBuffer<float> buf (std::max (totalIn, p.getTotalNumOutputChannels()), block);
    juce::MidiBuffer midi;
    std::vector<ft::Buf> out (2, ft::Buf ((size_t) n, 0.0f));
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        buf.setSize (buf.getNumChannels(), len, false, false, true);
        buf.clear();
        const int mainCh = p.getMainBusNumInputChannels();
        for (int c = 0; c < mainCh; ++c)
            buf.copyFrom (c, 0, tgt[(size_t) std::min (c, (int) tgt.size() - 1)].data() + pos, len);
        if (! sc.empty() && p.getBusCount (true) > 1 && p.getBus (true, 1)->isEnabled())
        {
            const int off = p.getChannelIndexInProcessBlockBuffer (true, 1, 0);
            const int scCh = p.getBus (true, 1)->getNumberOfChannels();
            for (int c = 0; c < scCh; ++c)
                buf.copyFrom (off + c, 0, sc[(size_t) std::min (c, (int) sc.size() - 1)].data() + pos, len);
        }
        p.processBlock (buf, midi);
        for (int c = 0; c < std::min (2, p.getMainBusNumOutputChannels()); ++c)
            std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].data() + pos);
    }
    return out;
}

bool setParam (FrictionAudioProcessor& p, const char* id, float plain)
{
    if (auto* prm = p.state().getParameter (id))
    {
        prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
        return true;
    }
    return false;
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::string outDir = "test_output", renderDir = "renders", appendReport;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--renders" && i + 1 < argc) renderDir = argv[++i];
        else if (a == "--append-report" && i + 1 < argc) appendReport = argv[++i];
    }
    juce::File (juce::String (outDir)).createDirectory();
    juce::File (juce::String (renderDir)).createDirectory();

    // ---- bus layouts
    {
        FrictionAudioProcessor p;
        using CS = juce::AudioChannelSet;
        auto layout = [] (CS in, CS out, CS sc)
        {
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (in);
            l.inputBuses.add (sc);
            l.outputBuses.add (out);
            return l;
        };
        const bool ok = p.checkBusesLayoutSupported (layout (CS::stereo(), CS::stereo(), CS::stereo()))
                        && p.checkBusesLayoutSupported (layout (CS::mono(), CS::mono(), CS::mono()))
                        && p.checkBusesLayoutSupported (layout (CS::stereo(), CS::stereo(), CS::disabled()))
                        && p.checkBusesLayoutSupported (layout (CS::mono(), CS::mono(), CS::stereo()))
                        && ! p.checkBusesLayoutSupported (layout (CS::stereo(), CS::mono(), CS::stereo()))
                        && ! p.checkBusesLayoutSupported (layout (CS::create5point1(), CS::create5point1(), CS::stereo()));
        add (ok, "Bus layouts (mono/stereo target and sidechain)", ok ? "as expected" : "unexpected", "mono/stereo accepted, others rejected");
    }

    // ---- latency reporting through the AudioProcessor API, both modes, several rates
    {
        bool pass = true;
        std::string meas;
        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            FrictionAudioProcessor p;
            p.setRateAndBufferSizeDetails (sr, 512);
            setParam (p, friction::ids::latency, 0.0f);
            p.prepareToPlay (sr, 512);
            const int mixL = p.getLatencySamples();
            const int expectMix = p.engine().latencyFor (fr::LatencyMode::Mix);
            // switch to Track while running: engine switches, host latency follows via the message thread
            setParam (p, friction::ids::latency, 1.0f);
            std::vector<ft::Buf> t (2, ft::sine (sr, 0.2, 220.0, 0.2)), s (2, ft::sine (sr, 0.2, 223.0, 0.2));
            run (p, t, s, 512);
            p.syncLatency();
            const int trackL = p.getLatencySamples();
            if (mixL != expectMix || trackL != 0) pass = false;
            meas += fmt ("%.0fk: Mix %.0f, Track %.0f; ", sr / 1000.0, (double) mixL, (double) trackL);
        }
        add (pass, "Reported latency (prepare + live mode switch)", meas, "Mix = N (4096/8192/16384), Track = 0");
    }

    // ---- bypass stays latency compensated (parameter and processBlockBypassed)
    {
        const double sr = 48000.0;
        bool pass = true;
        std::string meas;
        for (int variant = 0; variant < 2; ++variant)
        {
            FrictionAudioProcessor p;
            p.setRateAndBufferSizeDetails (sr, 256);
            setParam (p, friction::ids::amount, 100.0f);
            setParam (p, friction::ids::bypass, variant == 0 ? 1.0f : 0.0f);
            p.prepareToPlay (sr, 256);
            const int L = p.getLatencySamples();
            std::vector<ft::Buf> t (2, ft::saw (sr, 1.0, 230.0, 0.3)), s (2, ft::saw (sr, 1.0, 220.0, 0.3));
            std::vector<ft::Buf> out;
            if (variant == 0)
                out = run (p, t, s, 256);
            else
            {
                out.assign (2, ft::Buf (t[0].size(), 0.0f));
                juce::AudioBuffer<float> buf (4, 256);
                juce::MidiBuffer midi;
                for (int pos = 0; pos < (int) t[0].size(); pos += 256)
                {
                    const int len = std::min (256, (int) t[0].size() - pos);
                    buf.setSize (4, len, false, false, true);
                    buf.clear();
                    for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, t[(size_t) c].data() + pos, len);
                    p.processBlockBypassed (buf, midi);
                    for (int c = 0; c < 2; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].data() + pos);
                }
            }
            double worst = 0.0;
            for (size_t i = (size_t) L + 2048; i < t[0].size(); ++i)
                worst = std::max (worst, (double) std::abs (out[0][i] - t[0][i - (size_t) L]));
            if (worst > 1e-5) pass = false;
            meas += fmt (variant == 0 ? "bypass param: max diff %.2e (L=%.0f); " : "processBlockBypassed: max diff %.2e (L=%.0f)", worst, L);
        }
        add (pass, "Bypass is latency-compensated dry", meas, "<= -100 dBFS vs input delayed by L");
    }

    // ---- state save / restore
    {
        FrictionAudioProcessor a;
        setParam (a, friction::ids::mode, 1.0f);
        setParam (a, friction::ids::amount, 37.5f);
        setParam (a, friction::ids::rangeLow, 123.0f);
        setParam (a, friction::ids::retuneStyle, 2.0f);
        setParam (a, friction::ids::wholeOffset, -123.4f);
        setParam (a, friction::ids::beatRate, 77.0f);
        setParam (a, friction::ids::latency, 1.0f);
        a.editorWidth = 1234;
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        FrictionAudioProcessor b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        int mismatches = 0;
        for (auto* prm : a.getParameters())
        {
            auto* ra = dynamic_cast<juce::RangedAudioParameter*> (prm);
            auto* rb = dynamic_cast<juce::RangedAudioParameter*> (b.state().getParameter (ra->getParameterID()));
            if (rb == nullptr || std::abs (ra->getValue() - rb->getValue()) > 1e-6f)
                ++mismatches;
        }
        const bool ok = mismatches == 0 && b.editorWidth == 1234;
        add (ok, "State save/restore round trip (all parameters + UI size)", fmt ("%.0f mismatches, %.0f parameters", mismatches, (double) a.getParameters().size()),
             "0 mismatches");
    }

    // ---- presets
    {
        FrictionAudioProcessor p;
        const int n = p.getNumPrograms();
        bool ok = n >= 10;
        std::string names;
        for (int i = 0; i < n; ++i)
        {
            p.setCurrentProgram (i);
            ok = ok && p.getCurrentProgram() == i && p.getProgramName (i).isNotEmpty();
            names += p.getProgramName (i).toStdString() + (i + 1 < n ? ", " : "");
        }
        p.setCurrentProgram (1); // Kick <> Bass: Focus Low
        const float focus = p.state().getRawParameterValue (friction::ids::focus)->load();
        ok = ok && focus < 0.5f;
        add (ok, "Factory presets load", fmt ("%.0f presets: ", n) + names, ">= 10 presets, values applied");
    }

    // ---- sidechain bus disabled -> "No sidechain", Self analysis
    {
        FrictionAudioProcessor p;
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (juce::AudioChannelSet::stereo());
        l.inputBuses.add (juce::AudioChannelSet::disabled());
        l.outputBuses.add (juce::AudioChannelSet::stereo());
        const bool set = p.setBusesLayout (l);
        p.setPlayConfigDetails (2, 2, 48000.0, 512);
        p.prepareToPlay (48000.0, 512);
        std::vector<ft::Buf> t (2, ft::saw (48000.0, 1.0, 220.0, 0.3));
        run (p, t, {}, 512);
        const auto& pr = p.engine().probe();
        const bool ok = set && pr.sidechain == fr::SidechainStatus::Absent && pr.selfMode;
        add (ok, "Missing sidechain -> 'No sidechain' + Self mode", ok ? "status Absent, self analysis on" : "unexpected", "Absent + Self");
    }

    // ---- automation stress: random parameter changes every block
    {
        FrictionAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, 128);
        p.prepareToPlay (48000.0, 128);
        std::mt19937 rng (5);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        const double sr = 48000.0;
        std::vector<ft::Buf> t (2, ft::saw (sr, 6.0, 220.0, 0.3)), s (2, ft::saw (sr, 6.0, 226.0, 0.3));
        juce::AudioBuffer<float> buf (4, 128);
        juce::MidiBuffer midi;
        bool finite = true;
        double peak = 0.0;
        long long allocs = 0, blocks = 0;
        for (int pos = 0; pos + 128 <= (int) t[0].size(); pos += 128)
        {
            if ((pos / 128) % 8 == 0)
                for (auto* prm : p.getParameters())
                    if (u (rng) < 0.3f)
                        prm->setValueNotifyingHost (u (rng));
            if ((pos / 128) % 200 == 0)
                p.setLearning ((pos / 128) % 400 == 0); // Advisor learning toggled from the "UI"
            buf.clear();
            for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, t[(size_t) c].data() + pos, 128);
            for (int c = 0; c < 2; ++c) buf.copyFrom (2 + c, 0, s[(size_t) c].data() + pos, 128);
            // only the audio-thread calls are counted
            ft::AllocCounter::begin();
            if ((pos / 128) % 16 == 15)
                p.processBlockBypassed (buf, midi);
            else
                p.processBlock (buf, midi);
            allocs += ft::AllocCounter::end();
            ++blocks;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 128; ++i)
                {
                    const float v = buf.getSample (c, i);
                    finite = finite && std::isfinite (v);
                    peak = std::max (peak, (double) std::abs (v));
                }
            if ((pos / 128) % 64 == 0) p.syncLatency();
        }
        p.setLearning (false);
        add (finite && peak < 8.0, "Automation stress (random params every 8 blocks, 6 s)", fmt ("finite=%.0f, peak %.2f", finite ? 1.0 : 0.0, peak),
             "finite output, no blow-up (trim up to +24 dB)");
        add (allocs == 0, "Real-time safety: no heap allocation in processBlock",
             fmt ("%.0f allocations in %.0f processBlock / processBlockBypassed calls (random automation, Advisor learning on/off)", (double) allocs, (double) blocks),
             "0 calls to operator new on the audio thread");
    }

    // ---- Advisor: Learn -> Stop -> result -> Apply (kick vs bass)
    {
        const double sr = 48000.0;
        FrictionAudioProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512);
        p.prepareToPlay (sr, 512);
        ft::KickParams kp;
        ft::Buf kick ((size_t) (8.0 * sr), 0.0f), bass ((size_t) (8.0 * sr), 0.0f);
        for (double h = 0.05; h < 7.4; h += 0.5)
            ft::addKick (kick, sr, (size_t) (h * sr), kp);
        double ph = 0.0;
        for (size_t n = 0; n < bass.size(); ++n)
        {
            const double t = (double) n / sr;
            const double f = (int) t % 2 == 0 ? 55.0 : 65.406;
            ph += 2.0 * ft::kPi * f / sr;
            bass[n] = (float) (0.35 * std::sin (ph));
        }
        p.setLearning (true);
        run (p, { kick, kick }, { bass, bass }, 512);
        p.setLearning (false);
        double secs = 0.0;
        bool fin = false;
        fr::AdvisorResult r;
        for (int i = 0; i < 300 && ! fin; ++i)
        {
            juce::Thread::sleep (20);
            if (p.advisor().hasResult())
                r = p.advisor().getResult (secs, fin);
        }
        const bool applied = p.applyAdvisorOffset (true);
        auto& st = p.state();
        const float mode = st.getRawParameterValue (friction::ids::mode)->load();
        const float style = st.getRawParameterValue (friction::ids::retuneStyle)->load();
        const float off = st.getRawParameterValue (friction::ids::wholeOffset)->load();
        const float amt = st.getRawParameterValue (friction::ids::amount)->load();
        const bool ok = fin && r.lowValid && applied && mode > 0.5f && mode < 1.5f && style > 1.5f
                        && std::abs (off - r.lowBestCents) < 0.11f && std::abs (amt - 100.0f) < 0.01f;
        add (ok, "Advisor: Learn -> Stop -> Apply sets Retune Whole Fixed",
             fmt ("learned %.1f s, best low-band offset %+.0f c, kick %.1f Hz -> %.1f Hz", secs, r.lowBestCents, r.kickF0, r.kickNewF0)
                 + fmt ("; applied: offset %+.1f c, mode %.0f, style %.0f, amount %.0f %%", off, mode, style, amt),
             "final result, parameters set to Retune / Whole Fixed / offset / 100 %");
    }

    // ---- editor: create, feed frames, render screenshots in every mode and at two sizes
    {
        const double sr = 48000.0;
        FrictionAudioProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512);
        setParam (p, friction::ids::amount, 80.0f);
        setParam (p, friction::ids::sensitivity, 75.0f);
        p.prepareToPlay (sr, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorAndMakeActive());
        bool ok = ed != nullptr;
        int shots = 0;
        if (ok)
        {
            auto* fe = dynamic_cast<friction::FrictionAudioProcessorEditor*> (ed.get());
            // piano-ish detuned part vs pad, plus a low kick/bass clash
            ft::Buf tgt = ft::harmonic (sr, 4.0, 220.0 * std::pow (2.0, 18.0 / 1200.0), { 0.25, 0.18, 0.12, 0.08, 0.05, 0.04, 0.03 }, 0.0002);
            ft::Buf pad = ft::saw (sr, 4.0, 220.0, 0.12);
            ft::addInto (pad, ft::saw (sr, 4.0, 277.18, 0.08));
            ft::addInto (tgt, ft::sine (sr, 4.0, 52.0, 0.3));
            ft::addInto (pad, ft::sine (sr, 4.0, 55.0, 0.3));
            auto feed = [&] (double seconds)
            {
                const int block = 512;
                juce::AudioBuffer<float> buf (4, block);
                juce::MidiBuffer midi;
                const int n = (int) (seconds * sr);
                for (int pos = 0; pos + block <= n; pos += block)
                {
                    const int src = pos % ((int) tgt.size() - block);
                    buf.clear();
                    for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, tgt.data() + src, block);
                    for (int c = 0; c < 2; ++c) buf.copyFrom (2 + c, 0, pad.data() + src, block);
                    p.processBlock (buf, midi);
                    if ((pos / block) % 3 == 0 && fe != nullptr)
                        fe->refreshNow();
                }
                p.syncLatency();
            };
            auto shoot = [&] (const juce::String& name, int w, int h)
            {
                ed->setSize (w, h);
                if (fe != nullptr) fe->refreshNow();
                juce::Image img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
                juce::File f (juce::String (renderDir) + "/" + name);
                f.deleteFile();
                juce::FileOutputStream os (f);
                juce::PNGImageFormat png;
                if (os.openedOk() && png.writeImageToStream (img, os))
                    ++shots;
            };
            // Learn during the Smooth run so the Advisor panel shows a result
            p.setLearning (true);
            feed (4.0);
            p.setLearning (false);
            for (int i = 0; i < 100 && ! p.advisor().hasResult(); ++i) juce::Thread::sleep (20);
            juce::Thread::sleep (100);
            feed (0.5);
            shoot ("ui_smooth_1080x700.png", 1080, 700);
            shoot ("ui_smooth_900x580.png", 900, 580);
            setParam (p, friction::ids::mode, 1.0f);
            setParam (p, friction::ids::retuneStyle, 0.0f);
            feed (3.0);
            shoot ("ui_retune_partial_1080x700.png", 1080, 700);
            setParam (p, friction::ids::mode, 2.0f);
            feed (3.0);
            shoot ("ui_tension_1080x700.png", 1080, 700);
            setParam (p, friction::ids::mode, 0.0f);
            setParam (p, friction::ids::latency, 1.0f);
            feed (3.0);
            shoot ("ui_track_1080x700.png", 1080, 700);
            ok = shots == 5;
        }
        ed.reset();
        add (ok, "Editor renders in every mode and resizes (PNG snapshots)", fmt ("%.0f snapshots in renders/ui_*.png", shots),
             "no crash, 5 snapshots written");
    }

    int fails = 0;
    for (const auto& r : rows)
        if (! r.pass) ++fails;
    if (! appendReport.empty())
    {
        std::ofstream f (appendReport, std::ios::app);
        f << "\n## Plugin (JUCE AudioProcessor / editor)\n\n| Result | Check | Measured | Criterion |\n|---|---|---|---|\n";
        auto esc = [] (const std::string& s) { std::string e; for (char ch : s) { if (ch == '|') e += '\\'; e += ch; } return e; };
        for (const auto& r : rows)
            f << "| " << (r.pass ? "PASS" : "**FAIL**") << " | " << esc (r.name) << " | " << esc (r.measured) << " | " << esc (r.criterion) << " |\n";
    }
    std::printf ("\nPlugin tests: %d checks, %d failed\n", (int) rows.size(), fails);
    return fails == 0 ? 0 : 1;
}
