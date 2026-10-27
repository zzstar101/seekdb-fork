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
#include <utility>
#include <vector>

#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "storage/fts/ob_fts_stop_word.h"

namespace oceanbase
{
namespace storage
{
namespace
{

class WordAggregationTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    meta_.set_varchar();
    meta_.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
    flag_.set_groupby_word();
    ASSERT_EQ(OB_SUCCESS, words_.create(5, "FTSAggTest"));
  }

  void TearDown() override
  {
    words_.destroy();
  }

  using Entries = std::vector<std::pair<std::string, int64_t>>;
  static Entries entries(const ObFTWordMap &words)
  {
    Entries result;
    for (auto it = words.begin(); it != words.end(); ++it) {
      const auto str = it->first.get_word().get_string();
      result.emplace_back(std::string(str.ptr(), str.length()), it->second);
    }
    return result;
  }

  common::ObArenaAllocator allocator_;
  common::ObObjMeta meta_;
  ObFTParserProperty property_;
  ObAddWordFlag flag_;
  ObFTWordMap words_;
};

TEST_F(WordAggregationTest, new_and_repeated_words_keep_frequency_contract)
{
  ObAddWord add(property_, meta_, flag_, allocator_, words_);
  ASSERT_EQ(OB_SUCCESS, add.process_word("alpha", 5, 5, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("beta", 4, 4, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("alpha", 5, 5, 3));
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, words_.get_refactored(ObFTWord(5, "alpha", meta_), count));
  EXPECT_EQ(4, count);
  ASSERT_EQ(OB_SUCCESS, words_.get_refactored(ObFTWord(4, "beta", meta_), count));
  EXPECT_EQ(1, count);
  EXPECT_EQ(5, add.get_add_word_count());

  // Preserve the existing first-occurrence count, including a caller-provided
  // frequency above one. Changing that contract is separate from this optimization.
  ASSERT_EQ(OB_SUCCESS, add.process_word("gamma", 5, 5, 7));
  ASSERT_EQ(OB_SUCCESS, add.process_word("gamma", 5, 5, 2));
  ASSERT_EQ(OB_SUCCESS, words_.get_refactored(ObFTWord(5, "gamma", meta_), count));
  EXPECT_EQ(3, count);
  EXPECT_EQ(14, add.get_add_word_count());
}

TEST_F(WordAggregationTest, collation_equal_tokens_keep_first_spelling)
{
  meta_.set_collation_type(common::CS_TYPE_UTF8MB4_GENERAL_CI);
  ObAddWord add(property_, meta_, flag_, allocator_, words_);
  ASSERT_EQ(OB_SUCCESS, add.process_word("Cafe", 4, 4, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("caf\xc3\xa9", 5, 4, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("CAFE ", 5, 5, 1));
  ASSERT_EQ(1, words_.size());
  const Entries expected{{"Cafe", 3}};
  EXPECT_EQ(expected, entries(words_));
  EXPECT_EQ(3, add.get_add_word_count());
}

TEST_F(WordAggregationTest, colliding_new_words_and_updates_keep_all_entries)
{
  // More distinct keys than buckets guarantees collisions without depending on
  // the implementation of the collation hash.
  const int64_t key_count = words_.bucket_count() + 7;
  std::vector<std::string> inputs;
  inputs.reserve(key_count);
  for (int64_t i = 0; i < key_count; ++i) {
    inputs.emplace_back("token" + std::to_string(i));
  }
  ObAddWord add(property_, meta_, flag_, allocator_, words_);
  for (const auto &token : inputs) {
    ASSERT_EQ(OB_SUCCESS, add.process_word(token.data(), token.size(), token.size(), 1));
  }
  ASSERT_EQ(key_count, words_.size());
  Entries expected = entries(words_);
  for (auto &entry : expected) {
    entry.second = 3;
  }
  for (auto it = inputs.rbegin(); it != inputs.rend(); ++it) {
    ASSERT_EQ(OB_SUCCESS, add.process_word(it->data(), it->size(), it->size(), 2));
  }
  EXPECT_EQ(expected, entries(words_));
  EXPECT_EQ(3 * key_count, add.get_add_word_count());
}

TEST_F(WordAggregationTest, errors_do_not_add_words_or_document_length)
{
  ObAddWord add(property_, meta_, flag_, allocator_, words_);
  EXPECT_EQ(OB_INVALID_ARGUMENT, add.process_word(nullptr, 5, 5, 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add.process_word("alpha", 0, 5, 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add.process_word("alpha", 5, 0, 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add.process_word("alpha", 5, 5, 0));
  EXPECT_TRUE(words_.empty());
  EXPECT_EQ(0, add.get_add_word_count());

  ObFTWordMap uncreated;
  ObAddWord add_uncreated(property_, meta_, flag_, allocator_, uncreated);
  EXPECT_EQ(OB_NOT_INIT, add_uncreated.process_word("alpha", 5, 5, 1));
  EXPECT_EQ(0, add_uncreated.get_add_word_count());
}

TEST_F(WordAggregationTest, helper_reuse_clears_only_previous_document)
{
  // Ngram needs no process-global stopword or IK dictionary initialization.
  ObFTParser parser;
  ASSERT_EQ(OB_SUCCESS, parser.init(common::ObString::make_string("ngram")));
  ObFTParseHelper helper;
  ASSERT_EQ(OB_SUCCESS, helper.init(&allocator_, parser, property_));
  int64_t document_length = -1;
  ASSERT_EQ(OB_SUCCESS, helper.segment(meta_, "abab", 4, document_length, words_));
  ASSERT_EQ(2, words_.size());
  EXPECT_EQ(3, document_length);
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, words_.get_refactored(ObFTWord(2, "ab", meta_), count));
  EXPECT_EQ(2, count);

  ASSERT_EQ(OB_SUCCESS, helper.segment(meta_, "cd", 2, document_length, words_));
  EXPECT_EQ(Entries({{"cd", 1}}), entries(words_));
  EXPECT_EQ(1, document_length);

  ASSERT_EQ(OB_SUCCESS, helper.segment(meta_, " ", 1, document_length, words_));
  EXPECT_TRUE(words_.empty());
  EXPECT_EQ(0, document_length);
  ASSERT_EQ(OB_SUCCESS, helper.segment(meta_, " ", 1, document_length, words_));
  EXPECT_TRUE(words_.empty());
  EXPECT_EQ(0, document_length);

  ASSERT_EQ(OB_SUCCESS, helper.segment(meta_, "xy", 2, document_length, words_));
  EXPECT_EQ(Entries({{"xy", 1}}), entries(words_));
  EXPECT_EQ(1, document_length);
}

} // namespace
} // namespace storage
} // namespace oceanbase
