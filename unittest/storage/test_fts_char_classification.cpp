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
#include <cstdint>
#include <type_traits>

// Load dependencies before exposing only this header's private classifier.
// Direct template access also covers the retained UTF16 branches, which the
// current public collation resolver does not expose.
#include "lib/charset/ob_charset.h"
#include "lib/charset/ob_charset_string_helper.h"
#include "lib/charset/ob_ctype.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/utils/unicode_utils.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "storage/fts/ik/ob_ik_char_util.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace oceanbase
{
namespace storage
{
namespace
{

using CharType = ObFTCharUtil::CharType;

// Keep the pre-optimization predicate chain as an oracle. The helpers below
// are the actual, unchanged production helpers, including their error paths;
// the test does not duplicate Unicode tables or substitute a mock decoder.
template <ObCharsetType CS_TYPE>
int legacy_classify(const char *input, const uint8_t char_len, CharType &type)
{
  int ret = OB_SUCCESS;
  bool checker = false;
  type = CharType::USELESS;
  if (OB_FAIL(ObFTCharUtil::is_alpha<CS_TYPE>(input, char_len, checker))) {
  } else if (checker) {
    type = CharType::ENGLISH_LETTER;
  } else if (OB_FAIL(ObFTCharUtil::is_arabic<CS_TYPE>(input, char_len, checker))) {
  } else if (checker) {
    type = CharType::ARABIC_LETTER;
  } else if (OB_FAIL(ObFTCharUtil::is_chinese<CS_TYPE>(input, char_len, checker))) {
  } else if (checker) {
    type = CharType::CHINESE;
  } else if (OB_FAIL(ObFTCharUtil::is_other_cjk<CS_TYPE>(input, char_len, checker))) {
  } else if (checker) {
    type = CharType::OTHER_CJK;
  } else if (OB_FAIL(ObFTCharUtil::is_surrogate_high<CS_TYPE>(input, char_len, checker))) {
  } else if (checker) {
    type = CharType::SURROGATE_HIGH;
  } else if (OB_FAIL(ObFTCharUtil::is_surrogate_low<CS_TYPE>(input, char_len, checker))) {
  } else if (checker) {
    type = CharType::SURROGATE_LOW;
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
::testing::AssertionResult matches_legacy(const char *input, const uint8_t char_len)
{
  CharType expected_type = static_cast<CharType>(-1);
  CharType actual_type = static_cast<CharType>(-1);
  const int expected_ret = legacy_classify<CS_TYPE>(input, char_len, expected_type);
  const int actual_ret = ObFTCharUtil::do_classify<CS_TYPE>(input, char_len, actual_type);
  if (expected_ret == actual_ret && expected_type == actual_type) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
      << "charset=" << static_cast<int>(CS_TYPE)
      << " length=" << static_cast<int>(char_len)
      << " expected ret/type=" << expected_ret << '/' << static_cast<int>(expected_type)
      << " actual ret/type=" << actual_ret << '/' << static_cast<int>(actual_type);
}

TEST(CharClassificationTest, every_valid_utf8_scalar_matches_legacy)
{
  for (uint32_t codepoint = 0; codepoint <= 0x10ffff; ++codepoint) {
    if (0xd800 <= codepoint && codepoint <= 0xdfff) {
      continue; // Surrogate code units are not Unicode scalar values.
    }
    unsigned char bytes[4] = {};
    const int length = common::ob_charset_encode_unicode<CHARSET_UTF8MB4>(
        codepoint, bytes, bytes + sizeof(bytes));
    ASSERT_GT(length, 0) << "codepoint=" << codepoint;
    ASSERT_LE(length, static_cast<int>(sizeof(bytes))) << "codepoint=" << codepoint;
    ASSERT_TRUE(matches_legacy<CHARSET_UTF8MB4>(
        reinterpret_cast<const char *>(bytes), static_cast<uint8_t>(length)))
        << "codepoint=" << codepoint;
  }
}

TEST(CharClassificationTest, utf8_invalid_and_truncated_inputs_keep_errors_and_type)
{
  struct Input
  {
    const char *bytes;
    uint8_t length;
  };
  const Input inputs[] = {
      {"", 0},
      {"\x80", 1},
      {"\xc0\x80", 2},
      {"\xc2", 1},
      {"\xc2" "A", 2},
      {"\xe0\x80\x80", 3},
      {"\xe2\x82", 2},
      {"\xed\xa0\x80", 3},
      {"\xf0\x80\x80\x80", 4},
      {"\xf0\x9f\x92", 3},
      {"\xf4\x90\x80\x80", 4},
      {"\xf5\x80\x80\x80", 4},
      {"\xff", 1},
  };
  // Keep the production decoder's acceptance/error behavior for these byte
  // patterns; this change must not tighten Unicode validation independently.
  for (const Input &input : inputs) {
    ASSERT_TRUE(matches_legacy<CHARSET_UTF8MB4>(input.bytes, input.length));
  }
  EXPECT_TRUE(matches_legacy<CHARSET_UTF8MB4>(nullptr, 0));
  EXPECT_TRUE(matches_legacy<CHARSET_UTF8MB4>(nullptr, 1));
}

TEST(CharClassificationTest, every_utf16_code_unit_keeps_existing_behavior)
{
  // Compare compatibility with the current decoder, including its generic
  // fallback for UTF16; this optimization must not introduce charset support.
  for (uint32_t code_unit = 0; code_unit <= 0xffff; ++code_unit) {
    const char big_endian[] = {
        static_cast<char>(code_unit >> 8), static_cast<char>(code_unit & 0xff)};
    const char little_endian[] = {big_endian[1], big_endian[0]};
    ASSERT_TRUE(matches_legacy<CHARSET_UTF16>(big_endian, sizeof(big_endian)))
        << "code_unit=" << code_unit;
    ASSERT_TRUE(matches_legacy<CHARSET_UTF16LE>(little_endian, sizeof(little_endian)))
        << "code_unit=" << code_unit;
  }
}

TEST(CharClassificationTest, utf16_surrogate_pairs_and_prefixes_keep_existing_behavior)
{
  const unsigned char inputs[][4] = {
      {0x00, 0x00, 0x00, 0x00},
      {0x00, 0x41, 0x00, 0x42},
      {0xd7, 0xff, 0x00, 0x41},
      {0xd8, 0x00, 0xdc, 0x00},
      {0xdb, 0xff, 0xdf, 0xff},
      {0xd8, 0x00, 0x00, 0x41},
      {0xdc, 0x00, 0xd8, 0x00},
      {0xdf, 0xff, 0xe0, 0x00},
      {0xfe, 0xff, 0xff, 0xff},
  };
  for (const auto &bytes : inputs) {
    const char little_endian[] = {
        static_cast<char>(bytes[1]), static_cast<char>(bytes[0]),
        static_cast<char>(bytes[3]), static_cast<char>(bytes[2])};
    for (uint8_t length = 0; length <= sizeof(bytes); ++length) {
      ASSERT_TRUE(matches_legacy<CHARSET_UTF16>(reinterpret_cast<const char *>(bytes), length));
      ASSERT_TRUE(matches_legacy<CHARSET_UTF16LE>(little_endian, length));
    }
  }
  EXPECT_TRUE(matches_legacy<CHARSET_UTF16>(nullptr, 0));
  EXPECT_TRUE(matches_legacy<CHARSET_UTF16>(nullptr, 2));
  EXPECT_TRUE(matches_legacy<CHARSET_UTF16LE>(nullptr, 0));
  EXPECT_TRUE(matches_legacy<CHARSET_UTF16LE>(nullptr, 2));
}

} // namespace
} // namespace storage
} // namespace oceanbase
