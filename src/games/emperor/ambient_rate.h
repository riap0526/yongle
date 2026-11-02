// O4b (docs/archive/optimization.md): the city's random ambient sounds a given number of times per second instead of once in
// N main-loop passes. The game keeps its own test (rand() * N >> 15 == 1 on each city pass, a chance of about 1/N);
// AmbientRate only sets N from the passes per second it measures, so that passes/s / N = the rate wanted. With no
// rate (original) N is the value the game read from Audio\sound_fxU.txt (FREQUENCY_L2), put back when switched to
// original. Knows nothing of the game: it works on the value it is given (tested offline, test_ambient).
#pragma once

namespace emperor {

class AmbientRate {
public:
    // 0: original (the game's own N). Otherwise sounds per second.
    void SetRate(double perSecond);
    double Rate() const { return m_rate; }

    // Once per main-loop pass, before the game's pass. now: milliseconds; city: the city screen (the only one the
    // game asks for ambient sounds on); n: the game's N, read and written here. The file's N is kept the first time
    // n is not 0 (the game reads the file after the DLL is attached), and n is not written before that.
    void Pass(unsigned long now, bool city, int *n);

    int FileN() const { return m_fileN; } // 0 until known
    double PassesPerSecond() const { return m_passesPerSecond; } // 0 until measured

    static constexpr int kMinN = 2;     // the game's test never holds with N = 1 (rand() >> 15 is always 0)
    static constexpr int kMaxN = 32767;
    static constexpr unsigned long kWindowMs = 500; // passes/s measured over about this long (city passes only)

private:
    double m_rate = 0;
    int m_fileN = 0;
    bool m_restore = false; // put the file's N back at the next pass (switched to original)
    unsigned long m_windowStart = 0;
    long m_windowPasses = 0;
    double m_passesPerSecond = 0;
};

} // namespace emperor
