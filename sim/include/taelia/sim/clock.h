// World time: a fixed simulation step, time speed changes how many steps run (never their size),
// and the invented time units (hours a day, minutes an hour, seconds a minute) from shared/constants.txt.
#pragma once
#include <cstdint>

namespace taelia {

struct CalendarTime {
    int64_t day = 0;
    int hour = 0, minute = 0, second = 0;
};

class WorldClock {
public:
    static double stepSeconds();
    static double secondsPerDay();
    static double secondsPerHour();

    // Starts the world at `hour` (fractional) of day 0.
    explicit WorldClock(double startHour = 9.5, int64_t startDay = 0);

    // Feeds real time; returns how many fixed steps to run now (time speed scales it, capped against stalls).
    int advance(double realSeconds);
    // Marks one fixed step as run.
    void onStep() { ++steps_; }

    int64_t steps() const { return steps_; }
    double seconds() const;   // simulated seconds since day 0, 00:00:00
    double hourOfDay() const; // fractional hour in [0, HOURS_PER_DAY)
    CalendarTime calendar() const;

    // Moves forward to the next time the clock reads `hour` (the world settings' time-of-day slider).
    void jumpToHour(double hour);
    // Moves whole days forward or back (never before day 0): the sky's cycles span days.
    void jumpDays(int days);

    double timeSpeed() const { return speed_; }
    void setTimeSpeed(double s);
    // The +/- ladder from x1/50 to x50.
    void faster();
    void slower();

    static constexpr int kMaxStepsPerAdvance = 4000;

private:
    double startSeconds_ = 0;
    int64_t steps_ = 0;
    double accumulator_ = 0;
    double speed_ = 1.0;
};

}  // namespace taelia
