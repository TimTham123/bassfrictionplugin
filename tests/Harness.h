// Friction - offline verification harness: shared declarations
#pragma once

#include "FrictionEngine.h"
#include "TestUtil.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ft
{

struct Result
{
    std::string group;
    std::string name;
    bool pass = false;
    std::string measured;   // human-readable key numbers
    std::string criterion;  // what was required
};

struct Context
{
    std::string outDir = "test_output";
    std::string renderDir = "renders";
    bool renders = true;
    bool quick = false;
    std::vector<Result> results;
    std::vector<std::string> notes;

    void add (const std::string& group, const std::string& name, bool pass, const std::string& measured, const std::string& criterion)
    {
        results.push_back ({ group, name, pass, measured, criterion });
        std::printf ("[%s] %-46s %s | %s\n", pass ? "PASS" : "FAIL", (group + ": " + name).c_str(), measured.c_str(), criterion.c_str());
        std::fflush (stdout);
    }
    void wav (const std::string& file, const std::vector<Buf>& ch, double fs)
    {
        if (renders)
            writeWav (renderDir + "/" + file, ch, fs, 16);
    }
};

std::string fmt (const char* f, ...);

/** Stereo or mono buffers. */
using Chans = std::vector<Buf>;

struct RunOptions
{
    int block = 512;
    bool sidechain = true;
    /** Called after each block with the number of samples processed so far. */
    std::function<void (std::int64_t, fr::FrictionEngine&)> afterBlock;
    /** Called before each block to change parameters (sample index of block start). */
    std::function<void (std::int64_t, fr::Params&)> beforeBlock;
};

/** Runs the engine over the whole input. ref may be empty (no sidechain bus). Returns output channels. */
Chans runEngine (fr::FrictionEngine& eng, const Chans& target, const Chans& ref, const RunOptions& opt);

/** Convenience: prepare a fresh engine. */
std::unique_ptr<fr::FrictionEngine> makeEngine (double fs, int block, int numCh, const fr::Params& p);

// test groups
void testsAnalysis (Context& ctx);
void testsSmooth (Context& ctx);
void testsAdvisor (Context& ctx);
void testsRetune (Context& ctx);
void testsTension (Context& ctx);
void testsRobustness (Context& ctx);
void testsDemos (Context& ctx);
void testsPerformance (Context& ctx);

} // namespace ft
