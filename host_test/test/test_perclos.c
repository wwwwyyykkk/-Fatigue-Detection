#include <assert.h>
#include <stdio.h>

#include "perclos.h"

static void test_normal_boundaries(void)
{
    assert(perclos_level(0.00f) == FATIGUE_NORMAL);
    assert(perclos_level(0.10f) == FATIGUE_NORMAL);
    assert(perclos_level(0.15f) == FATIGUE_NORMAL);
}

static void test_mild_boundaries(void)
{
    assert(perclos_level(0.16f) == FATIGUE_MILD);
    assert(perclos_level(0.20f) == FATIGUE_MILD);
    assert(perclos_level(0.30f) == FATIGUE_MILD);
}

static void test_severe_boundaries(void)
{
    assert(perclos_level(0.31f) == FATIGUE_SEVERE);
    assert(perclos_level(0.50f) == FATIGUE_SEVERE);
    assert(perclos_level(1.00f) == FATIGUE_SEVERE);
}

static void test_invalid_input(void)
{
    assert(perclos_level(-0.01f) == -1);
    assert(perclos_level(1.01f) == -1);
}

int main(void)
{
    test_normal_boundaries();
    test_mild_boundaries();
    test_severe_boundaries();
    test_invalid_input();

    printf("all perclos tests passed\n");
    return 0;
}
