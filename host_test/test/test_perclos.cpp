#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "perclos.hpp"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

static bool near(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

template <int N>
static void fill(PerclosCalculator<N> &p, int closed, int open)
{
    for (int i = 0; i < closed; ++i) {
        p.add_sample(true);
    }
    for (int i = 0; i < open; ++i) {
        p.add_sample(false);
    }
}

static void test_empty_window(void)
{
    std::puts("test_empty_window");
    PerclosCalculator<4> p;
    CHECK(near(p.get_perclos(), 0.0f));
    CHECK(p.get_fatigue_level() == FatigueLevel::NORMAL);
    CHECK(p.get_valid_count() == 0);
    CHECK(!p.is_warm());
}

static void test_boundaries(void)
{
    std::puts("test_boundaries");

    // Clearly below the 15% threshold
    PerclosCalculator<20> p10;
    fill(p10, 2, 18);
    CHECK(near(p10.get_perclos(), 10.0f));
    CHECK(p10.get_fatigue_level() == FatigueLevel::NORMAL);

    // Clearly inside the mild band
    PerclosCalculator<20> p20;
    fill(p20, 4, 16);
    CHECK(near(p20.get_perclos(), 20.0f));
    CHECK(p20.get_fatigue_level() == FatigueLevel::MILD);

    // Clearly above the severe threshold
    PerclosCalculator<10> p50;
    fill(p50, 5, 5);
    CHECK(near(p50.get_perclos(), 50.0f));
    CHECK(p50.get_fatigue_level() == FatigueLevel::SEVERE);
}

static void test_window_replacement(void)
{
    std::puts("test_window_replacement");
    PerclosCalculator<4> p;
    fill(p, 4, 0);
    CHECK(near(p.get_perclos(), 100.0f));
    CHECK(p.is_warm());

    fill(p, 0, 4);
    CHECK(near(p.get_perclos(), 0.0f));
    CHECK(p.get_valid_count() == 4);
    CHECK(p.get_fatigue_level() == FatigueLevel::NORMAL);
}

static void test_weather_thresholds(void)
{
    std::puts("test_weather_thresholds");
    PerclosCalculator<10> p;

    p.set_weather_risk(WeatherRisk::CLEAR);
    CHECK(p.get_fatigue_level_for(16.0f) == FatigueLevel::MILD);
    CHECK(p.get_fatigue_level_for(31.0f) == FatigueLevel::SEVERE);
    CHECK(p.get_drive_warning_minutes() == 210);

    p.set_weather_risk(WeatherRisk::RAIN);
    CHECK(p.get_fatigue_level_for(13.0f) == FatigueLevel::MILD);
    CHECK(p.get_fatigue_level_for(26.0f) == FatigueLevel::SEVERE);
    CHECK(p.get_drive_warning_minutes() == 180);

    p.set_weather_risk(WeatherRisk::FOG);
    CHECK(p.get_fatigue_level_for(11.0f) == FatigueLevel::MILD);
    CHECK(p.get_fatigue_level_for(21.0f) == FatigueLevel::SEVERE);
    CHECK(p.get_drive_warning_minutes() == 150);
}

static void test_reset(void)
{
    std::puts("test_reset");
    PerclosCalculator<4> p;
    fill(p, 4, 0);
    CHECK(p.is_warm());

    p.reset();
    CHECK(near(p.get_perclos(), 0.0f));
    CHECK(p.get_valid_count() == 0);
    CHECK(!p.is_warm());
    CHECK(p.get_fatigue_level() == FatigueLevel::NORMAL);
}

static void test_fatigue_level_for(void)
{
    std::puts("test_fatigue_level_for");
    PerclosCalculator<30> p;
    CHECK(p.get_fatigue_level_for(15.0f) == FatigueLevel::NORMAL);
    CHECK(p.get_fatigue_level_for(15.1f) == FatigueLevel::MILD);
    CHECK(p.get_fatigue_level_for(30.0f) == FatigueLevel::MILD);
    CHECK(p.get_fatigue_level_for(30.1f) == FatigueLevel::SEVERE);
    CHECK(PerclosCalculator<30>::window_size() == 30);
}

int main(void)
{
    test_empty_window();
    test_boundaries();
    test_window_replacement();
    test_weather_thresholds();
    test_reset();
    test_fatigue_level_for();

    std::puts("all PERCLOS tests passed");
    return 0;
}
