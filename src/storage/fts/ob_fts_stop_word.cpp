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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(build_ascii_cache())) {
    } else {
      inited_ = true;
    }
  }
  return ret;
}

int ObStopWordChecker::build_ascii_cache()
{
  int ret = OB_SUCCESS;
  ascii_cache_cnt_ = 0;
  ascii_cache_valid_ = false;
  const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
  for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
    const char *word = ob_stop_word_list[i];
    const int64_t word_len = STRLEN(word);
    bool is_lower_ascii = (word_len <= FTS_STOP_WORD_MAX_LENGTH);
    for (int64_t j = 0; is_lower_ascii && j < word_len; ++j) {
      if (!ob_isascii(word[j]) || (word[j] >= 'A' && word[j] <= 'Z')) {
        is_lower_ascii = false;
      }
    }
    if (OB_UNLIKELY(!is_lower_ascii)) {
      // A stop word that is not lower case ascii can not be matched by the fast path, so
      // disable the fast path instead of building an incomplete cache.
      ret = OB_ERR_UNEXPECTED;
    } else {
      // keep the cache sorted by (length, bytes) and skip the duplicated entries
      int64_t pos = 0;
      bool dup = false;
      while (pos < ascii_cache_cnt_ && !dup) {
        const int64_t cmp_len = ascii_cache_len_[pos];
        const int64_t cmp = (word_len != cmp_len)
                            ? (word_len < cmp_len ? -1 : 1)
                            : MEMCMP(word, ascii_cache_[pos], word_len);
        if (0 == cmp) {
          dup = true;
        } else if (cmp < 0) {
          break;
        } else {
          ++pos;
        }
      }
      if (!dup) {
        for (int64_t k = ascii_cache_cnt_; k > pos; --k) {
          MEMCPY(ascii_cache_[k], ascii_cache_[k - 1], FTS_STOP_WORD_MAX_LENGTH);
          ascii_cache_len_[k] = ascii_cache_len_[k - 1];
        }
        MEMCPY(ascii_cache_[pos], word, word_len);
        ascii_cache_len_[pos] = static_cast<int8_t>(word_len);
        ++ascii_cache_cnt_;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ascii_cache_valid_ = true;
  }
  return ret;
}

OB_INLINE bool ObStopWordChecker::in_ascii_cache(const char *word, const int64_t word_len) const
{
  bool found = false;
  int64_t left = 0;
  int64_t right = ascii_cache_cnt_ - 1;
  while (!found && left <= right) {
    const int64_t mid = left + ((right - left) >> 1);
    const int64_t cmp_len = ascii_cache_len_[mid];
    int64_t cmp = 0;
    if (word_len != cmp_len) {
      cmp = (word_len < cmp_len) ? -1 : 1;
    } else {
      cmp = MEMCMP(word, ascii_cache_[mid], word_len);
    }
    if (0 == cmp) {
      found = true;
    } else if (cmp < 0) {
      right = mid - 1;
    } else {
      left = mid + 1;
    }
  }
  return found;
}

OB_INLINE bool ObStopWordChecker::try_check_ascii_stopword(
    const ObFTWord &word,
    bool &is_stopword) const
{
  bool decided = false;
  if (OB_LIKELY(ascii_cache_valid_)) {
    const ObString word_str = word.get_word().get_string();
    const char *ptr = word_str.ptr();
    const int64_t len = word_str.length();
    // utf8mb4_general_ci ignores trailing spaces in both hashing and comparison, such
    // words are left to the general path.
    if (OB_LIKELY(len > 0 && ' ' != ptr[len - 1])) {
      bool is_ascii = true;
      char fold_buf[FTS_STOP_WORD_MAX_LENGTH];
      if (len <= FTS_STOP_WORD_MAX_LENGTH) {
        for (int64_t i = 0; is_ascii && i < len; ++i) {
          const char c = ptr[i];
          if (!ob_isascii(c)) {
            is_ascii = false;
          } else {
            fold_buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
          }
        }
      } else {
        for (int64_t i = 0; is_ascii && i < len; ++i) {
          if (!ob_isascii(ptr[i])) {
            is_ascii = false;
          }
        }
      }
      // For an ascii word the general path is equivalent to a case insensitive byte
      // comparison with the (ascii) stop word list: the charset conversion from any
      // ascii compatible charset to utf8mb4 is the identity, and utf8mb4_general_ci
      // compares ascii characters case insensitively.
      if (is_ascii) {
        decided = true;
        is_stopword = (len <= FTS_STOP_WORD_MAX_LENGTH) ? in_ascii_cache(fold_buf, len) : false;
      }
    }
  }
  return decided;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    ascii_cache_cnt_ = 0;
    ascii_cache_valid_ = false;
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  
  
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_LIKELY(try_check_ascii_stopword(word, is_stopword))) {
    // decided without a charset conversion nor a collation aware hash lookup
  } else {
    common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
    common::ObString cmp_str;
    // do nothing set out with in if type is the same.
    if (OB_FAIL(common::ObCharset::charset_convert(
                                       allocator,
                                       word.get_word().get_string(),
                                       word.get_collation_type(),
                                       stopword_type_.get_collation_type(),
                                       cmp_str))) {
    } else {
      ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.casedown()) {
    dst = src;
  } else {
    const ObString src_str = src.get_word().get_string();
    const ObCollationType coll_type = word_meta_.get_collation_type();
    const ObCharsetInfo *cs_info = ObCharset::get_charset(coll_type);
    if (OB_ISNULL(cs_info) || OB_ISNULL(cs_info->cset)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_UNLIKELY(1 != cs_info->casedn_multiply)) {
      // a character may be expanded when being lower cased, nothing can be shared
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(coll_type, src_str, dst_str, allocator_))) {
      } else {
        ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
        dst = tmp;
      }
    } else {
      const int64_t src_len = src_str.length();
      char *lower_buf = case_down_buf_;
      if (src_len > static_cast<int64_t>(sizeof(case_down_buf_))
          && OB_ISNULL(lower_buf = static_cast<char *>(allocator_.alloc(src_len)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        const size_t lower_len = cs_info->cset->casedn(
            cs_info, const_cast<char *>(src_str.ptr()), src_len, lower_buf, src_len);
        if (static_cast<int64_t>(lower_len) == src_len
            && 0 == MEMCMP(lower_buf, src_str.ptr(), src_len)) {
          // the word is already lower case, share the source string instead of copying it
          dst = src;
        } else if (lower_buf != case_down_buf_) {
          ObFTWord tmp(static_cast<int64_t>(lower_len), lower_buf, word_meta_);
          dst = tmp;
        } else {
          char *word_buf = static_cast<char *>(allocator_.alloc(lower_len));
          if (OB_ISNULL(word_buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(word_buf, lower_buf, lower_len);
            ObFTWord tmp(static_cast<int64_t>(lower_len), word_buf, word_meta_);
            dst = tmp;
          }
        }
      }
    }
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  int64_t word_count = 0;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else if (OB_FAIL(word_map_.get_refactored(word, word_count)) && OB_HASH_NOT_EXIST != ret) {
  } else {
    if (OB_HASH_NOT_EXIST == ret) {
      word_count = 1;
    } else {
      word_count += word_freq;
    }
    if (OB_FAIL(word_map_.set_refactored(word, word_count, 1/*overwrite*/))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
