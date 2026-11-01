#include "games/emperor/ambient_rate.h"

#include <cmath>

namespace emperor {

void AmbientRate::SetRate(double perSecond)
{
    const bool wasRate = m_rate > 0;
    m_rate = perSecond > 0 ? perSecond : 0;
    if (wasRate && m_rate == 0) {
        m_restore = true;
    }
}

void AmbientRate::Pass(unsigned long now, bool city, int *n)
{
    if (m_fileN == 0 && *n != 0) {
        m_fileN = *n; // read from the file by the game: the value before any of ours
    }
    if (m_fileN == 0) {
        return; // the game has not read its file yet: nothing to keep, nothing to write
    }
    if (m_restore) {
        m_restore = false;
        *n = m_fileN;
    }
    if (!city) {
        m_windowStart = 0; // other screens: a new measurement when the city comes back
        return;
    }
    if (m_windowStart == 0) {
        m_windowStart = now;
        m_windowPasses = 0;
    }
    ++m_windowPasses;
    const unsigned long elapsed = now - m_windowStart;
    if (elapsed >= kWindowMs) {
        m_passesPerSecond = m_windowPasses * 1000.0 / elapsed;
        m_windowStart = now;
        m_windowPasses = 0;
    }
    if (m_rate > 0 && m_passesPerSecond > 0) {
        long v = std::lround(m_passesPerSecond / m_rate);
        v = v < kMinN ? kMinN : v > kMaxN ? kMaxN : v;
        *n = static_cast<int>(v);
    }
}

} // namespace emperor
