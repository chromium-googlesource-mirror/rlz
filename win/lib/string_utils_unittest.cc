// Copyright 2010 Google Inc. All Rights Reserved.
// Use of this source code is governed by an Apache-style license that can be
// found in the COPYING file.
//
// Unit test for string manipulation functions used in the RLZ library.

#include "base/basictypes.h"
#include "base/logging.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

#include "rlz/win/lib/string_utils.h"
#include "rlz/win/lib/assert.h"

TEST(StringUtilsUnittest, IsAscii) {
  rlz_lib::expected_assertion_ = "";

  char bad_letters[] = {'\x80', '\xA0', '\xFF'};
  for (int i = 0; i < arraysize(bad_letters); ++i)
    EXPECT_FALSE(rlz_lib::IsAscii(bad_letters[i]));

  char good_letters[] = {'A', '~', '\n', 0x7F, 0x00};
  for (int i = 0; i < arraysize(good_letters); ++i)
    EXPECT_TRUE(rlz_lib::IsAscii(good_letters[i]));
}

TEST(StringUtilsUnittest, HexStringToInteger) {
  rlz_lib::expected_assertion_ = "HexStringToInteger: text is NULL.";
  EXPECT_EQ(0, rlz_lib::HexStringToInteger(NULL));

  rlz_lib::expected_assertion_ = "";
  EXPECT_EQ(0, rlz_lib::HexStringToInteger(""));
  EXPECT_EQ(0, rlz_lib::HexStringToInteger("   "));
  EXPECT_EQ(0, rlz_lib::HexStringToInteger("  0x  "));
  EXPECT_EQ(0, rlz_lib::HexStringToInteger("  0x0  "));
  EXPECT_EQ(0x12345, rlz_lib::HexStringToInteger("12345"));
  EXPECT_EQ(0xa34Ed0, rlz_lib::HexStringToInteger("a34Ed0"));
  EXPECT_EQ(0xa34Ed0, rlz_lib::HexStringToInteger("0xa34Ed0"));
  EXPECT_EQ(0xa34Ed0, rlz_lib::HexStringToInteger("   0xa34Ed0"));
  EXPECT_EQ(0xa34Ed0, rlz_lib::HexStringToInteger("0xa34Ed0   "));
  EXPECT_EQ(0xa34Ed0, rlz_lib::HexStringToInteger("   0xa34Ed0   "));
  EXPECT_EQ(0xa34Ed0, rlz_lib::HexStringToInteger("   0x000a34Ed0   "));
  EXPECT_EQ(0xa34Ed0, rlz_lib::HexStringToInteger("   000a34Ed0   "));

  rlz_lib::expected_assertion_ =
      "HexStringToInteger: text contains non-hex characters.";
  EXPECT_EQ(0x12ff, rlz_lib::HexStringToInteger("12ffg"));
  EXPECT_EQ(0x12f, rlz_lib::HexStringToInteger("12f 121"));
  EXPECT_EQ(0x12f, rlz_lib::HexStringToInteger("12f 121"));
  EXPECT_EQ(0, rlz_lib::HexStringToInteger("g12f"));
  EXPECT_EQ(0, rlz_lib::HexStringToInteger("  0x0  \n"));

  rlz_lib::expected_assertion_ = "";
}

TEST(StringUtilsUnittest, TestBytesToString) {
  unsigned char data[] = {0x1E, 0x00, 0x21, 0x67, 0xFF};
  std::wstring result;

  EXPECT_FALSE(rlz_lib::BytesToString(NULL, 5, &result));
  EXPECT_FALSE(rlz_lib::BytesToString(data, 5, NULL));
  EXPECT_FALSE(rlz_lib::BytesToString(NULL, 5, NULL));

  EXPECT_TRUE(rlz_lib::BytesToString(data, 5, &result));
  EXPECT_STREQ(L"1E002167FF", result.c_str());
  EXPECT_TRUE(rlz_lib::BytesToString(data, 4, &result));
  EXPECT_STREQ(L"1E002167", result.c_str());
}
