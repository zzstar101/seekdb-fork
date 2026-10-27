/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "lib/charset/ob_charset.h"

namespace oceanbase
{
namespace common
{
namespace
{

std::vector<ObCollationType> supported_collations()
{
  std::vector<ObCollationType> result;
  for (int64_t id = CS_TYPE_INVALID + 1; id < CS_TYPE_MAX; ++id) {
    if (ObCharset::is_valid_collation(id)) {
      result.push_back(static_cast<ObCollationType>(id));
    }
  }
  return result;
}

TEST(CharsetStrcmpspDispatch, initialized_and_first_use_keep_handler_results)
{
  const std::vector<std::string> samples = {
      "", " ", "  ", "a", "A", "a ", "a  ", "alpha", "alphabet", "ALPHA",
      "é", "e", "İ", "i", "中文", "中文检索", "😀", "😀 ",
      std::string("a\0b", 3), std::string("a\0 ", 3), std::string("a\1", 2),
      std::string("\x80", 1), std::string("\xff", 1), std::string("\xc3", 1),
      std::string("\xc0\xaf", 2), std::string("\xe4\xb8", 2),
  };
  const auto collations = supported_collations();
  ASSERT_FALSE(collations.empty());
  for (const ObCollationType collation : collations) {
    // Exercise the public entry point before this test requests a descriptor.
    // Fresh processes can reach its lazy initialization path here.
    ASSERT_EQ(0, ObCharset::strcmpsp(collation, "seed", 4, "seed", 4, false));
    const ObCharsetInfo *cs = ObCharset::get_charset(collation);
    ASSERT_NE(nullptr, cs);
    ASSERT_NE(nullptr, cs->coll);
    ASSERT_NE(nullptr, cs->coll->strnncollsp);
    for (const bool compare_endspace : {false, true}) {
      for (size_t left = 0; left < samples.size(); ++left) {
        for (size_t right = 0; right < samples.size(); ++right) {
          const std::string &l = samples[left];
          const std::string &r = samples[right];
          SCOPED_TRACE(::testing::Message() << "collation=" << collation
              << " endspace=" << compare_endspace
              << " left=" << left << " right=" << right);
          const int expected = cs->coll->strnncollsp(cs,
              reinterpret_cast<const unsigned char *>(l.data()), l.size(),
              reinterpret_cast<const unsigned char *>(r.data()), r.size(),
              compare_endspace);
          EXPECT_EQ(expected, ObCharset::strcmpsp(collation,
              l.data(), l.size(), r.data(), r.size(), compare_endspace));
        }
      }
    }
  }
}

TEST(CharsetStrcmpspDispatch, null_pointer_with_zero_length_remains_empty)
{
  for (const ObCollationType collation : supported_collations()) {
    for (const bool compare_endspace : {false, true}) {
      EXPECT_EQ(0, ObCharset::strcmpsp(collation, nullptr, 0, nullptr, 0, compare_endspace));
      EXPECT_EQ(0, ObCharset::strcmpsp(collation, nullptr, 0, "", 0, compare_endspace));
      EXPECT_EQ(ObCharset::strcmpsp(collation, "", 0, "a ", 2, compare_endspace),
                ObCharset::strcmpsp(collation, nullptr, 0, "a ", 2, compare_endspace));
      EXPECT_EQ(ObCharset::strcmpsp(collation, "a ", 2, "", 0, compare_endspace),
                ObCharset::strcmpsp(collation, "a ", 2, nullptr, 0, compare_endspace));
    }
  }
}

TEST(CharsetStrcmpspDispatch, invalid_arguments_keep_the_existing_zero_result)
{
  for (const ObCollationType collation : supported_collations()) {
    // Ensure invalid strings are rejected even after the descriptor is ready.
    ASSERT_NE(nullptr, ObCharset::get_charset(collation));
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, nullptr, 1, "a", 1, false));
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, "a", 1, nullptr, 1, false));
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, "a", -1, "b", 1, false));
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, "a", 1, "b", -1, false));
  }
  const ObCollationType unsupported[] = {
      static_cast<ObCollationType>(-1), CS_TYPE_INVALID,
      CS_TYPE_LATIN1_SWEDISH_CI, CS_TYPE_MAX,
      static_cast<ObCollationType>(static_cast<int>(CS_TYPE_MAX) + 1),
  };
  for (const ObCollationType collation : unsupported) {
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, "a", 1, "b", 1, false));
  }
}

} // namespace
} // namespace common
} // namespace oceanbase
