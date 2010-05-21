// Copyright 2010 Google Inc. All Rights Reserved.
// Use of this source code is governed by an Apache-style license that can be
// found in the COPYING file.

#include "base/logging.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

#include "rlz/win/lib/lib_values.h"
#include "rlz/win/lib/assert.h"

TEST(LibValuesUnittest, GetAccessPointFromName) {
  rlz_lib::expected_assertion_ = "GetAccessPointFromName: point is NULL";
  EXPECT_FALSE(rlz_lib::GetAccessPointFromName("", NULL));
  rlz_lib::expected_assertion_ = "";

  rlz_lib::AccessPoint point;
  EXPECT_FALSE(rlz_lib::GetAccessPointFromName(NULL, &point));
  EXPECT_EQ(rlz_lib::NO_ACCESS_POINT, point);

  EXPECT_TRUE(rlz_lib::GetAccessPointFromName("", &point));
  EXPECT_EQ(rlz_lib::NO_ACCESS_POINT, point);

  EXPECT_FALSE(rlz_lib::GetAccessPointFromName("i1", &point));
  EXPECT_EQ(rlz_lib::NO_ACCESS_POINT, point);

  EXPECT_TRUE(rlz_lib::GetAccessPointFromName("I7", &point));
  EXPECT_EQ(rlz_lib::IE_DEFAULT_SEARCH, point);

  EXPECT_TRUE(rlz_lib::GetAccessPointFromName("T4", &point));
  EXPECT_EQ(rlz_lib::IETB_SEARCH_BOX, point);

  EXPECT_FALSE(rlz_lib::GetAccessPointFromName("T4 ", &point));
  EXPECT_EQ(rlz_lib::NO_ACCESS_POINT, point);
}


TEST(LibValuesUnittest, GetEventFromName) {
  rlz_lib::expected_assertion_ = "GetEventFromName: event is NULL";
  EXPECT_FALSE(rlz_lib::GetEventFromName("", NULL));
  rlz_lib::expected_assertion_ = "";

  rlz_lib::Event event;
  EXPECT_FALSE(rlz_lib::GetEventFromName(NULL, &event));
  EXPECT_EQ(rlz_lib::INVALID_EVENT, event);

  EXPECT_TRUE(rlz_lib::GetEventFromName("", &event));
  EXPECT_EQ(rlz_lib::INVALID_EVENT, event);

  EXPECT_FALSE(rlz_lib::GetEventFromName("i1", &event));
  EXPECT_EQ(rlz_lib::INVALID_EVENT, event);

  EXPECT_TRUE(rlz_lib::GetEventFromName("I", &event));
  EXPECT_EQ(rlz_lib::INSTALL, event);

  EXPECT_TRUE(rlz_lib::GetEventFromName("F", &event));
  EXPECT_EQ(rlz_lib::FIRST_SEARCH, event);

  EXPECT_FALSE(rlz_lib::GetEventFromName("F ", &event));
  EXPECT_EQ(rlz_lib::INVALID_EVENT, event);
}
