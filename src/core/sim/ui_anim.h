// Speed-0 animations outside the city (the flags on a world map): the games step a picture of animation speed 0 on
// every drawing (their anim_advance_ui), so at the original's 20 frames a second they moved 20 steps a second and
// with frames that follow the display they move much faster. A game layer's detour of that function asks Step once
// per call; it is decided on the first call of each game frame (every call in one frame gets the same answer):
// true at most once every kSpeed0Ms, as at 20 frames a second. Speeds other than 0 are the game's own timers.
// Nothing here knows a game address (Emperor's china_map.cpp and Zeus's resolution.cpp use it).
#pragma once

namespace core::sim {

class Speed0Gate {
public:
    static constexpr unsigned kSpeed0Ms = 50;

    // frame: any value that changes once per game frame (the frame's start time or a frame count); now: the time in
    // milliseconds (timeGetTime or the like).
    bool Step(unsigned frame, unsigned now)
    {
        if (!m_started || frame != m_frame) { // the first call in this game frame decides for all of them
            m_started = true;
            m_frame = frame;
            m_step = now - m_last >= kSpeed0Ms;
            if (m_step) {
                m_last = now;
            }
        }
        return m_step;
    }

private:
    bool m_started = false;
    unsigned m_frame = 0, m_last = 0;
    bool m_step = true;
};

} // namespace core::sim
