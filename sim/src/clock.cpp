#include "taelia/sim/clock.h"

#include <cmath>

#include "taelia/shared_constants.h"

namespace taelia {

namespace {
constexpr double kLadder[] = {1.0 / 50, 1.0 / 20, 1.0 / 10, 1.0 / 5, 1.0 / 2, 1, 2, 5, 10, 20, 50};
constexpr int kLadderSize = int(sizeof(kLadder) / sizeof(kLadder[0]));

int ladderIndex(double s) {
    int best = 0;
    for (int i = 1; i < kLadderSize; ++i)
        if (std::fabs(std::log(kLadder[i] / s)) < std::fabs(std::log(kLadder[best] / s))) best = i;
    return best;
}
}  // namespace

double WorldClock::stepSeconds() { return 1.0 / k::SIM_STEP_HZ; }
double WorldClock::secondsPerHour() { return double(k::MINUTES_PER_HOUR) * k::SECONDS_PER_MINUTE; }
double WorldClock::secondsPerDay() { return secondsPerHour() * k::HOURS_PER_DAY; }

WorldClock::WorldClock(double startHour, int64_t startDay) {
    double h = std::fmod(startHour, double(k::HOURS_PER_DAY));
    if (h < 0) h += k::HOURS_PER_DAY;
    // Whole steps only, so the clock reads exactly what was asked.
    startSeconds_ = double(startDay) * secondsPerDay() + std::round(h * secondsPerHour() * k::SIM_STEP_HZ) / k::SIM_STEP_HZ;
}

int WorldClock::advance(double realSeconds) {
    if (realSeconds < 0) realSeconds = 0;
    accumulator_ += realSeconds * speed_;
    const double step = stepSeconds();
    double n = std::floor(accumulator_ / step + 1e-9);
    if (n > kMaxStepsPerAdvance) {
        n = kMaxStepsPerAdvance;
        accumulator_ = 0;  // drop the backlog rather than stall
    } else {
        accumulator_ -= n * step;
        if (accumulator_ < 0) accumulator_ = 0;
    }
    return int(n);
}

double WorldClock::seconds() const { return startSeconds_ + double(steps_) / k::SIM_STEP_HZ; }

double WorldClock::hourOfDay() const {
    double s = std::fmod(seconds(), secondsPerDay());
    return s / secondsPerHour();
}

CalendarTime WorldClock::calendar() const {
    // Count in whole steps so the result never drifts.
    const int64_t stepsPerSecond = k::SIM_STEP_HZ;
    const int64_t total = int64_t(std::llround(seconds() * stepsPerSecond)) / stepsPerSecond;
    const int64_t perDay = int64_t(k::HOURS_PER_DAY) * k::MINUTES_PER_HOUR * k::SECONDS_PER_MINUTE;
    CalendarTime c;
    c.day = total / perDay;
    int64_t r = total % perDay;
    c.hour = int(r / (int64_t(k::MINUTES_PER_HOUR) * k::SECONDS_PER_MINUTE));
    r %= int64_t(k::MINUTES_PER_HOUR) * k::SECONDS_PER_MINUTE;
    c.minute = int(r / k::SECONDS_PER_MINUTE);
    c.second = int(r % k::SECONDS_PER_MINUTE);
    return c;
}

void WorldClock::jumpToHour(double hour) {
    hour = std::fmod(hour, double(k::HOURS_PER_DAY));
    if (hour < 0) hour += k::HOURS_PER_DAY;
    const double now = seconds();
    const double dayStart = std::floor(now / secondsPerDay()) * secondsPerDay();
    double target = dayStart + hour * secondsPerHour();
    if (target < now) target += secondsPerDay();
    // Whole steps, so the clock stays on the step grid.
    startSeconds_ += std::round((target - now) * k::SIM_STEP_HZ) / k::SIM_STEP_HZ;
}

void WorldClock::setTimeSpeed(double s) {
    if (!(s > 0)) s = 1;
    if (s < k::TIME_SPEED_MIN) s = k::TIME_SPEED_MIN;
    if (s > k::TIME_SPEED_MAX) s = k::TIME_SPEED_MAX;
    speed_ = s;
}

void WorldClock::faster() {
    int i = ladderIndex(speed_);
    setTimeSpeed(kLadder[i + 1 < kLadderSize ? i + 1 : i]);
}

void WorldClock::slower() {
    int i = ladderIndex(speed_);
    setTimeSpeed(kLadder[i > 0 ? i - 1 : 0]);
}

}  // namespace taelia

namespace taelia {

void WorldClock::jumpDays(int days) {
    const double moved = startSeconds_ + double(days) * secondsPerDay();
    if (seconds() + double(days) * secondsPerDay() >= 0.0) startSeconds_ = moved;
}

}  // namespace taelia
