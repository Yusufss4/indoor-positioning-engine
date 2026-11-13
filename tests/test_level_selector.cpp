#include <gtest/gtest.h>

#include <array>

#include "ipe/level_selector.hpp"

using namespace ipe;

TEST(LevelSelector, EvenSplitDoesNotFlicker) {
  // Starts on level 0, then the weights swing 0.45 / 0.55 with every update.
  LevelSelector sel(0.5, 0.1);
  ASSERT_EQ(sel.update(0.0, std::array{0.6, 0.4}), 0);
  for (int i = 1; i <= 100; ++i) {
    const double w0 = i % 2 ? 0.45 : 0.55;
    EXPECT_EQ(sel.update(i * 0.1, std::array{w0, 1.0 - w0}), 0) << "update " << i;
  }
}

TEST(LevelSelector, DecisiveChangeIsFollowedWithinASecond) {
  LevelSelector sel(0.5, 0.1);
  sel.update(0.0, std::array{0.9, 0.1});
  double switched = -1.0;
  for (int i = 1; i <= 50 && switched < 0.0; ++i) {
    if (sel.update(i * 0.1, std::array{0.1, 0.9}) == 1) switched = i * 0.1;
  }
  // Not at the first update, and the average needs avg1 - avg0 >= 0.1: 1 - exp(-t / 0.5) >= 0.5625.
  EXPECT_GT(switched, 0.1);
  EXPECT_LE(switched, 0.5);
}

TEST(LevelSelector, NoSmoothingReportsTheMostLikelyLevel) {
  LevelSelector sel(0.0, 0.0);
  EXPECT_EQ(sel.update(0.0, std::array{0.6, 0.3, 0.1}), 0);
  EXPECT_EQ(sel.update(0.0, std::array{0.3, 0.6, 0.1}), 1);
  EXPECT_EQ(sel.update(0.1, std::array{0.2, 0.3, 0.5}), 2);
  EXPECT_EQ(sel.update(0.2, std::array{0.6, 0.3, 0.1}), 0);
}

TEST(LevelSelector, LeavesALevelWithNoWeightAtOnce) {
  LevelSelector sel(10.0, 0.5);  // a long average that would hold the old level for seconds
  sel.update(0.0, std::array{1.0, 0.0});
  EXPECT_EQ(sel.update(0.1, std::array{0.0, 1.0}), 1);
}

TEST(LevelSelector, ResetForgetsTheHistory) {
  LevelSelector sel(10.0, 0.5);
  sel.update(0.0, std::array{0.9, 0.1});
  sel.reset();
  EXPECT_EQ(sel.update(0.1, std::array{0.2, 0.8}), 1);
}
