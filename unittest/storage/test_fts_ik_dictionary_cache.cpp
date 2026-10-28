/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/container/ob_se_array.h"
#include "storage/fts/dict/ob_ft_cache_dict.h"
#include "storage/fts/dict/ob_ft_dat_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/dict/ob_ft_trie.h"
#include "storage/fts/ik/ob_ik_cjk_processor.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
namespace
{

using CharType = ObFTCharUtil::CharType;

int build_dat(ObIAllocator &alloc,
              const std::initializer_list<const char *> &words,
              ObFTDAT *&dat)
{
  int ret = OB_SUCCESS;
  size_t dat_len = 0;
  ObFTTrie<void> trie(alloc, ObCollationType::CS_TYPE_UTF8MB4_BIN);
  ObFTTrieNodeData<void> node_data;
  ObFTDATBuilder<void> builder(alloc);
  for (const char *word : words) {
    if (OB_FAIL(trie.insert(ObString::make_string(word), node_data))) {
      break;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(builder.init(trie))) {
  } else if (OB_FAIL(builder.build_from_trie(trie))) {
  } else if (OB_FAIL(builder.get_mem_block(dat, dat_len))) {
  } else if (OB_ISNULL(dat) || dat_len < sizeof(ObFTDAT)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

struct TokenView
{
  bool operator==(const TokenView &other) const
  {
    return offset_ == other.offset_ && length_ == other.length_
           && char_cnt_ == other.char_cnt_ && type_ == other.type_;
  }

  int64_t offset_;
  int64_t length_;
  int64_t char_cnt_;
  ObIKTokenType type_;
};

std::vector<TokenView> collect_tokens(TokenizeContext &ctx)
{
  std::vector<TokenView> result;
  for (const ObIKToken &token : ctx.token_list().tokens()) {
    result.push_back({token.offset_, token.length_, token.char_cnt_, token.type_});
  }
  return result;
}

void expect_same_hit(const ObDATrieHit &expected, const ObDATrieHit &actual)
{
  EXPECT_EQ(expected.is_match(), actual.is_match());
  EXPECT_EQ(expected.is_prefix(), actual.is_prefix());
  EXPECT_EQ(expected.base_idx_, actual.base_idx_);
  EXPECT_EQ(expected.end_pos_, actual.end_pos_);
  EXPECT_EQ(expected.char_cnt_, actual.char_cnt_);
}

// Reference implementation of the original CJK matching loop. It deliberately
// uses only the pre-existing match_with_hit API for every active prefix hit.
int process_reference(TokenizeContext &ctx,
                      const ObIFTDict &dict,
                      ObIAllocator &hits_allocator)
{
  int ret = OB_SUCCESS;
  ObList<ObDATrieHit, ObIAllocator> hits(hits_allocator);
  while (OB_SUCC(ret) && !ctx.iter_end()) {
    const char *ch = nullptr;
    uint8_t char_len = 0;
    CharType type = CharType::USELESS;
    if (OB_FAIL(ctx.current_char(ch, char_len))) {
    } else if (OB_FAIL(ctx.current_char_type(type))) {
    } else if (CharType::USELESS != type) {
      const ObString current_char(char_len, ch);
      for (ObList<ObDATrieHit, ObIAllocator>::iterator iter = hits.begin();
           OB_SUCC(ret) && iter != hits.end();
           ++iter) {
        ObDATrieHit &hit = *iter;
        if (OB_FAIL(dict.match_with_hit(current_char, hit, hit))) {
        } else if (hit.is_match()) {
          if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                    hit.start_pos_,
                                    hit.end_pos_ - hit.start_pos_,
                                    hit.char_cnt_,
                                    ObIKTokenType::IK_CHINESE_TOKEN))) {
          }
        } else if (hit.is_prefix()) {
        } else if (hit.is_unmatch()) {
          hits.erase(hit);
        } else {
          ret = OB_UNEXPECT_INTERNAL_ERROR;
        }
      }
      if (OB_SUCC(ret)) {
        ObDATrieHit hit(&dict, ctx.get_cursor());
        if (OB_FAIL(dict.match(current_char, hit))) {
        } else if (hit.is_match()) {
          hits.push_back(hit);
          if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                    ctx.get_cursor(),
                                    char_len,
                                    1,
                                    ObIKTokenType::IK_CHINESE_TOKEN))) {
          }
        } else if (hit.is_prefix()) {
          hits.push_back(hit);
        }
      }
    } else {
      hits.clear();
    }

    if (OB_FAIL(ret)) {
      hits.clear();
    } else if (ctx.is_last()) {
      hits.clear();
    }
    if (OB_SUCC(ret)) {
      int step_ret = ctx.step_next();
      if (OB_ITER_END == step_ret) {
        ret = OB_SUCCESS;
      } else {
        ret = step_ret;
      }
    }
  }
  hits.reset();
  return ret;
}

int process_candidate(TokenizeContext &ctx, ObIKCJKProcessor &processor)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && !ctx.iter_end()) {
    if (OB_FAIL(processor.process(ctx))) {
    } else {
      int step_ret = ctx.step_next();
      if (OB_ITER_END == step_ret) {
        ret = OB_SUCCESS;
      } else {
        ret = step_ret;
      }
    }
  }
  return ret;
}

class CountingCacheDict final : public ObIFTDict
{
public:
  explicit CountingCacheDict(ObFTDAT *dat) : reader_(dat) {}

  int init() override { return OB_SUCCESS; }

  int match(const ObString &single_word, ObDATrieHit &hit) const override
  {
    ++single_match_calls_;
    hit.dict_ = this;
    return reader_.match_with_hit(single_word, hit, hit);
  }

  int match(const ObString &words, bool &is_match) const override
  {
    int ret = OB_SUCCESS;
    int64_t char_len = 0;
    ObDATrieHit hit(this, 0);
    is_match = false;
    for (int64_t offset = 0; OB_SUCC(ret) && offset < words.length(); offset += char_len) {
      if (OB_FAIL(ObCharset::first_valid_char(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                              words.ptr() + offset,
                                              words.length() - offset,
                                              char_len))) {
      } else if (OB_FAIL(match_with_hit(ObString(char_len, words.ptr() + offset), hit, hit))) {
      } else if (hit.is_match() && offset + char_len == words.length()) {
        is_match = true;
        break;
      } else if (hit.is_unmatch()) {
        break;
      }
    }
    return ret;
  }

  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const override
  {
    ++legacy_match_calls_;
    return reader_.match_with_hit(single_word, last_hit, hit);
  }

  int find_word_code(const ObString &single_word,
                     ObFTWordCode &code,
                     bool &found) const override
  {
    ++code_lookup_calls_;
    return reader_.find_word_code(single_word, code, found);
  }

  int match_with_code(const ObString &single_word,
                      const ObFTWordCode code,
                      const ObDATrieHit &last_hit,
                      ObDATrieHit &hit) const override
  {
    ++code_match_calls_;
    return reader_.match_with_code(single_word, code, last_hit, hit);
  }

  int64_t code_lookup_calls() const { return code_lookup_calls_; }
  int64_t code_match_calls() const { return code_match_calls_; }
  int64_t legacy_match_calls() const { return legacy_match_calls_; }
  int64_t single_match_calls() const { return single_match_calls_; }

private:
  ObFTDATReader<void> reader_;
  mutable int64_t code_lookup_calls_ = 0;
  mutable int64_t code_match_calls_ = 0;
  mutable int64_t legacy_match_calls_ = 0;
  mutable int64_t single_match_calls_ = 0;
};

class IKDictionaryCacheTest : public ::testing::Test
{
protected:
  ObArenaAllocator dat_allocator_;
  ObArenaAllocator token_allocator_;
};

TEST_F(IKDictionaryCacheTest, code_lookup_matches_oracle_for_each_dat_and_miss)
{
  std::vector<ObFTDAT *> dats(3, nullptr);
  ASSERT_EQ(OB_SUCCESS,
            build_dat(dat_allocator_, {"一", "一乙", "一乙丙", "乙", "乙丙"}, dats[0]));
  ASSERT_EQ(OB_SUCCESS, build_dat(dat_allocator_, {"三", "三四", "文", "文三"}, dats[1]));
  ASSERT_EQ(OB_SUCCESS, build_dat(dat_allocator_, {"尔法", "阿", "阿尔"}, dats[2]));

  std::vector<std::string> probes;
  for (int value = 0; value <= UINT8_MAX; ++value) {
    probes.emplace_back(1, static_cast<char>(value));
  }
  probes.emplace_back(std::string("一"));
  probes.emplace_back(std::string("乙"));
  probes.emplace_back(std::string("丙"));
  probes.emplace_back(std::string("丁"));
  probes.emplace_back(std::string("文"));
  probes.emplace_back(std::string("三"));
  probes.emplace_back(std::string("四"));
  probes.emplace_back(std::string("阿"));
  probes.emplace_back(std::string("尔"));
  probes.emplace_back(std::string("法"));
  probes.emplace_back(std::string{{static_cast<char>(0xC0), static_cast<char>(0x80)}});
  probes.emplace_back(std::string{{static_cast<char>(0xE0), static_cast<char>(0x80),
                                   static_cast<char>(0x80)}});
  probes.emplace_back(std::string{{static_cast<char>(0xF5), static_cast<char>(0xFF),
                                   static_cast<char>(0xFF), static_cast<char>(0xFF)}});

  for (ObFTDAT *dat : dats) {
    ObFTCacheDict dict(ObCollationType::CS_TYPE_UTF8MB4_BIN, dat);
    ASSERT_EQ(OB_SUCCESS, dict.init());
    for (const std::string &probe : probes) {
      const ObString word(static_cast<int32_t>(probe.size()), probe.data());
      ObDATrieHit oracle(&dict, 0);
      ASSERT_EQ(OB_SUCCESS, dict.match(word, oracle));

      ObFTWordCode code = 0;
      bool found = false;
      ASSERT_EQ(OB_SUCCESS, dict.find_word_code(word, code, found));
      ObDATrieHit candidate(&dict, 0);
      if (found) {
        ASSERT_EQ(OB_SUCCESS, dict.match_with_code(word, code, candidate, candidate));
      } else {
        candidate.set_unmatch();
      }
      expect_same_hit(oracle, candidate);
    }
  }
}

TEST_F(IKDictionaryCacheTest, character_codes_are_local_to_each_dat)
{
  ObFTDAT *single_root_dat = nullptr;
  ObFTDAT *second_root_dat = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_dat(dat_allocator_, {"一"}, single_root_dat));
  ASSERT_EQ(OB_SUCCESS, build_dat(dat_allocator_, {"A一", "一"}, second_root_dat));

  ObFTCacheDict single_root_dict(ObCollationType::CS_TYPE_UTF8MB4_BIN, single_root_dat);
  ObFTCacheDict second_root_dict(ObCollationType::CS_TYPE_UTF8MB4_BIN, second_root_dat);
  ASSERT_EQ(OB_SUCCESS, single_root_dict.init());
  ASSERT_EQ(OB_SUCCESS, second_root_dict.init());

  ObFTWordCode single_root_code = 0;
  ObFTWordCode second_root_code = 0;
  bool found = false;
  ASSERT_EQ(OB_SUCCESS, single_root_dict.find_word_code(ObString::make_string("一"),
                                                         single_root_code,
                                                         found));
  ASSERT_TRUE(found);
  ASSERT_EQ(OB_SUCCESS, second_root_dict.find_word_code(ObString::make_string("一"),
                                                         second_root_code,
                                                         found));
  ASSERT_TRUE(found);
  EXPECT_NE(single_root_code, second_root_code);
}

TEST_F(IKDictionaryCacheTest, cjk_processor_matches_legacy_loop_across_dat_ranges)
{
  ObFTDAT *dat1 = nullptr;
  ObFTDAT *dat2 = nullptr;
  ASSERT_EQ(OB_SUCCESS,
            build_dat(dat_allocator_,
                      {"一", "一丁", "一丁七", "一丁七八", "丁", "丁七", "丁七八"},
                      dat1));
  ASSERT_EQ(OB_SUCCESS, build_dat(dat_allocator_,
                                  {"七", "七万", "万"},
                                  dat2));

  ObFTRangeDict dict(dat_allocator_,
                     nullptr,
                     ObFTDictDesc(ObString::make_string("ik-cache-test"),
                                  ObFTDictType::DICT_IK_MAIN,
                                  ObCharsetType::CHARSET_UTF8MB4,
                                  ObCollationType::CS_TYPE_UTF8MB4_BIN));
  ObSEArray<ObFTDAT *, 2> dat_blocks;
  ASSERT_EQ(OB_SUCCESS, dat_blocks.push_back(dat1));
  ASSERT_EQ(OB_SUCCESS, dat_blocks.push_back(dat2));
  ASSERT_EQ(OB_SUCCESS, dict.build_dict_from_dat_blocks(dat_blocks));
  ObFTWordCode range_code = 0;
  bool range_found = false;
  ASSERT_EQ(OB_SUCCESS, dict.find_word_code(ObString::make_string("七"), range_code, range_found));
  ASSERT_TRUE(range_found);
  ASSERT_EQ(OB_SUCCESS, dict.find_word_code(ObString::make_string("乙"), range_code, range_found));
  EXPECT_FALSE(range_found);

  std::string text;
  text.reserve(4 * 512);
  for (int64_t i = 0; i < 512; ++i) {
    text.append("一丁七万");
  }

  TokenizeContext reference_ctx(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                token_allocator_,
                                text.data(),
                                text.size(),
                                false);
  TokenizeContext candidate_ctx(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                token_allocator_,
                                text.data(),
                                text.size(),
                                false);
  ASSERT_EQ(OB_SUCCESS, reference_ctx.init());
  ASSERT_EQ(OB_SUCCESS, candidate_ctx.init());
  ASSERT_EQ(OB_SUCCESS, process_reference(reference_ctx, dict, token_allocator_));
  ObIKCJKProcessor processor(dict, token_allocator_);
  ASSERT_EQ(OB_SUCCESS, process_candidate(candidate_ctx, processor));
  EXPECT_EQ(collect_tokens(reference_ctx), collect_tokens(candidate_ctx));
  EXPECT_FALSE(collect_tokens(candidate_ctx).empty());
}

TEST_F(IKDictionaryCacheTest, cjk_processor_reuses_code_for_active_prefixes)
{
  ObFTDAT *dat = nullptr;
  ASSERT_EQ(OB_SUCCESS,
            build_dat(dat_allocator_, {"丁", "丙丁", "乙丙丁", "甲乙丙丁"}, dat));
  CountingCacheDict dict(dat);

  const int64_t repeats = 200;
  std::string text;
  text.reserve(5 * repeats);
  for (int64_t i = 0; i < repeats; ++i) {
    text.append("甲乙丙丁 ");
  }
  TokenizeContext ctx(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                      token_allocator_,
                      text.data(),
                      text.size(),
                      false);
  ASSERT_EQ(OB_SUCCESS, ctx.init());
  ObIKCJKProcessor processor(dict, token_allocator_);
  ASSERT_EQ(OB_SUCCESS, process_candidate(ctx, processor));

  // Each four-character unit has two overlapping-prefix steps. One DAT code
  // lookup serves three active hits at the fourth character, so each unit saves
  // three hashed dictionary lookups without caching any word result.
  EXPECT_EQ(2 * repeats, dict.code_lookup_calls());
  EXPECT_EQ(5 * repeats, dict.code_match_calls());
  EXPECT_EQ(3 * repeats,
            dict.code_match_calls() - dict.code_lookup_calls());
  EXPECT_EQ(repeats, dict.legacy_match_calls());
  EXPECT_EQ(4 * repeats, dict.single_match_calls());
  EXPECT_FALSE(ctx.token_list().tokens().empty());
}

} // namespace
} // namespace storage
} // namespace oceanbase
