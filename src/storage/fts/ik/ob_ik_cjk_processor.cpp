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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_cjk_processor.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
namespace
{
struct CharCodeCacheEntry
{
  const ObIFTDict *dict_;
  ObFTWordCode code_;
  bool found_;
};

// Four slots bound stack use; additional dictionaries fall back to the original
// matcher without growing the cache.
static constexpr int64_t CHAR_CODE_CACHE_SIZE = 4;

// The code belongs to one immutable DAT. A processor call handles one current
// character, so this cache only removes repeated lookups of that character among
// simultaneously active prefix hits.
int get_current_char_code(const ObIFTDict &dict,
                          const ObString &current_char,
                          CharCodeCacheEntry *cache,
                          int64_t &cache_size,
                          const CharCodeCacheEntry *&entry,
                          bool &is_cached)
{
  int ret = OB_SUCCESS;
  entry = nullptr;
  is_cached = true;
  for (int64_t i = 0; i < cache_size; ++i) {
    if (cache[i].dict_ == &dict) {
      entry = &cache[i];
      break;
    }
  }
  if (OB_ISNULL(entry)) {
    if (cache_size >= CHAR_CODE_CACHE_SIZE) {
      is_cached = false;
    } else {
      CharCodeCacheEntry &new_entry = cache[cache_size];
      new_entry.dict_ = &dict;
      if (OB_FAIL(dict.find_word_code(current_char, new_entry.code_, new_entry.found_))) {
      } else {
        entry = &new_entry;
        ++cache_size;
      }
    }
  }
  return ret;
}
} // namespace

int ObIKCJKProcessor::do_process(TokenizeContext &ctx,
                                 const char *ch,
                                 const uint8_t char_len,
                                 const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;

  if (ObFTCharUtil::CharType::USELESS != type) {
    const ObString current_char(char_len, ch);
    CharCodeCacheEntry code_cache[CHAR_CODE_CACHE_SIZE];
    int64_t code_cache_size = 0;
    const bool use_code_cache = hits_.size() > 1;
    // handle previous hits_ first and then check from this char
    for (ObList<ObDATrieHit, ObIAllocator>::iterator iter = hits_.begin();
         OB_SUCC(ret) && iter != hits_.end();
         iter++) {
      ObDATrieHit &hit = *iter;
      bool matched = false;
      if (use_code_cache && OB_NOT_NULL(hit.dict_)) {
        const CharCodeCacheEntry *entry = nullptr;
        bool is_cached = false;
        if (OB_FAIL(get_current_char_code(*hit.dict_,
                                          current_char,
                                          code_cache,
                                          code_cache_size,
                                          entry,
                                          is_cached))) {
        } else if (is_cached) {
          matched = true;
          if (!entry->found_) {
            hit.set_unmatch();
          } else if (OB_FAIL(hit.dict_->match_with_code(current_char,
                                                        entry->code_,
                                                        hit,
                                                        hit))) {
          }
        }
      }
      if (OB_SUCC(ret) && !matched
          && OB_FAIL(dict_main_.match_with_hit(current_char, hit, hit))) {
      }
      if (OB_FAIL(ret)) {
      } else if (hit.is_match()) {
        if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                  hit.start_pos_,
                                  hit.end_pos_ - hit.start_pos_,
                                  hit.char_cnt_,
                                  ObIKTokenType::IK_CHINESE_TOKEN))) {
        } else if (hit.is_prefix()) {
          // match will record the start_cursor
        }
      } else if (hit.is_prefix()) {
        // nothing
      } else if (hit.is_unmatch()) {
        hits_.erase(hit);
      } else {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
      }
    }
    if (OB_SUCC(ret)) {
      // Start from this char
      // find range of dict
      ObDATrieHit hit(&dict_main_, ctx.get_cursor());
      if (OB_FAIL(dict_main_.match(current_char, hit))) {
      } else if (hit.is_match()) {
        // output token
        hits_.push_back(hit);
        ctx.add_token(ctx.fulltext(),
                      ctx.get_cursor(),
                      char_len,
                      1,
                      ObIKTokenType::IK_CHINESE_TOKEN);
      } else if (hit.is_prefix()) {
        hits_.push_back(hit);
      } else {
        // ignore mismatch
      }
    }
  } else {
    // stop previous match
    hits_.clear();
  }

  if (OB_SUCC(ret)) {
    if (ctx.is_last()) {
      hits_.clear();
    }
  } else {
    hits_.clear();
  }

  return ret;
}

} //  namespace storage
} //  namespace oceanbase
