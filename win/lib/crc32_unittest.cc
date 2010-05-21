// Copyright 2010 Google Inc. All Rights Reserved.
// Use of this source code is governed by an Apache-style license that can be
// found in the COPYING file.
//
// A test for ZLib's checksum function.

#include "base/logging.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

#include "rlz/win/lib/crc32.h"

TEST(Crc32Unittest, ByteTest) {
  struct {
    char* data;
    int len;
    // Externally calculated at http://crc32-checksum.waraxe.us/
    int crc;
  } kData[] = {
    {"Hello"           ,  5, 0xF7D18982},
    {"Google"          ,  6, 0x62B0F067},
    {""                ,  0, 0x0},
    {"One more string.", 16, 0x0CA14970},
    {NULL              ,  0, 0x0},
  };

  for (int i = 0; kData[i].data; i++)
    EXPECT_EQ(kData[i].crc,
        rlz_lib::Crc32(reinterpret_cast<unsigned char*>(kData[i].data),
                       kData[i].len));
}

TEST(Crc32Unittest, CharTest) {
  struct {
    char* data;
    // Externally calculated at http://crc32-checksum.waraxe.us/
    int crc;
  } kData[] = {
    {"Hello"           , 0xF7D18982},
    {"Google"          , 0x62B0F067},
    {""                , 0x0},
    {"One more string.", 0x0CA14970},
    {"Google\r\n"      , 0x83A3E860},
    {NULL              , 0x0},
  };

  int crc;
  for (int i = 0; kData[i].data; i++) {
    EXPECT_TRUE(rlz_lib::Crc32(kData[i].data, &crc));
    EXPECT_EQ(kData[i].crc, crc);
  }
}
