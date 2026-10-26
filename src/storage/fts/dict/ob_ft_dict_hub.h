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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_

#include "lib/allocator/page_arena.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/charset/ob_charset.h"
#include "lib/lock/ob_bucket_lock.h"
#include "lib/lock/ob_mutex.h"
#include "storage/fts/dict/ob_ft_dict_def.h"

namespace oceanbase
{
namespace storage
{
// typedef uint64_t ObFTTableID;
// typedef ObFTTableID ObFTDictTableID;

class ObFTDictInfo
{
public:
  ObFTDictInfo()
      : name_(""),
        type_(ObFTDictType::DICT_TYPE_INVALID),
        charset_(CHARSET_INVALID),
        version_(0),
        range_count_(0)
  {
  }

public:
  char name_[2048]; // for now
  ObFTDictType type_;
  ObCharsetType charset_;
  int64_t version_; // in memory
  int32_t range_count_;
};

struct ObFTDictInfoKey
{
public:
  ObFTDictInfoKey()
      : type_(static_cast<uint64_t>(ObFTDictType::DICT_TYPE_INVALID))
  {
  } // default constructor
  ObFTDictInfoKey(const uint64_t type)
      : type_(type)
  {
  }
  int hash(uint64_t &hash_value) const
  {
    int ret = OB_SUCCESS;
    hash_value = hash();
    return ret;
  }

  uint64_t hash() const
  {
    uint64_t hash = 0;
    hash = common::murmurhash(&type_, sizeof(int64_t), hash);
    return hash;
  }

  bool operator==(const ObFTDictInfoKey &other) const
  {
    return type_ == other.type_;
  }

  int compare(const ObFTDictInfoKey &other) const
  {
    int ret = 0;
    if (0 == ret) {
      ret = type_ - other.type_;
    }
    return ret;
  }

private:
  uint64_t type_;
  // name
};

class ObFTCacheRangeContainer;
class ObFTRangeDict;
class ObIFTDict;
struct ObFTDAT;

// An immutable view of one built-in dictionary: the dictionary blocks are
// materialized into this view's own allocator instead of being pinned in the
// dictionary kv cache, so a view neither holds cache handles nor depends on the
// lifetime of the kv cache. A view is built once and then only read.
struct ObFTDictHubView
{
  ObFTDictHubView()
      : alloc_(lib::ObMemAttr("FTDictView")),
        range_dict_(nullptr)
  {
  }
  ObArenaAllocator alloc_;
  ObFTRangeDict *range_dict_;
};

class ObFTDictHub
{
public:
  ObFTDictHub()
      : is_inited_(false),
        dict_map_(),
        rw_dict_lock_(),
        view_slot_cnt_(0),
        view_build_lock_(),
        view_alloc_(lib::ObMemAttr("FTDictViews")),
        view_fallback_cnt_(0)
  {
    for (int64_t i = 0; i < MAX_DICT_VIEWS; ++i) {
      view_slots_[i].view_key_ = 0;
      view_slots_[i].view_ = 0;
    }
  }
  ~ObFTDictHub() {}

  int init();

  int destroy();

  int build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container);

  int load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container);

  // Returns the immutable views of the three built-in IK dictionaries for the
  // given charset and collation, building them on first use. The dicts are owned
  // by the hub and stay valid until the hub is destroyed; they are read only, so
  // the caller must neither modify nor free them.
  //
  // The views are built from the built-in dictionaries, which are immutable
  // process lifetime data: they are loaded once from the bundled dictionary file
  // and no code path ever bumps a dictionary version, so a published view can
  // never become stale while the process lives. That is why there is no timer,
  // no version check and no eviction here: a view is built at most once per key,
  // published once, and freed only by destroy() or invalidate_views(). Once all
  // MAX_DICT_VIEWS slots are taken, callers fall back to building the
  // dictionaries per call (OB_SIZE_OVERFLOW) instead of dropping a view another
  // thread may be reading.
  int get_builtin_ik_dicts(const ObCharsetType charset,
                           const ObCollationType coll_type,
                           ObIFTDict *&main_dict,
                           ObIFTDict *&quan_dict,
                           ObIFTDict *&stop_dict);

  // Hook for a future dictionary reload path: drops every published view so that
  // the next tokenization rebuilds them from the reloaded dictionaries. It must
  // only be called while no tokenization can be in flight: readers take no lock
  // at all, so freeing a view another thread is using would be a use after free.
  // Nothing calls it today because the built-in dictionaries are never reloaded.
  void invalidate_views();

  // Number of tokenizations that fell back to building the dictionaries per call
  // because all view slots were taken.
  int64_t get_view_fallback_cnt() const { return ATOMIC_LOAD(&view_fallback_cnt_); }

private:
  int get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info);

  int put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info);

  static int64_t make_view_key(const ObCharsetType charset,
                               const ObCollationType coll_type,
                               const ObFTDictType type)
  {
    return (static_cast<int64_t>(type) << 48) | (static_cast<int64_t>(charset) << 32)
           | static_cast<int64_t>(coll_type);
  }

  int find_view(const int64_t view_key, ObFTDictHubView *&view) const;

  int build_view(const int64_t view_key,
                 const ObFTDictDesc &desc,
                 ObFTDictHubView *&view);

  int materialize_dict(const ObFTDictDesc &desc, ObFTDictHubView &view);

  void destroy_views();

private:
  static constexpr int64_t MAX_DICT_VIEWS = 8;
  struct ObFTDictViewSlot
  {
    int64_t view_key_;
    int64_t view_; // ObFTDictHubView *, published after the view is fully built
  };

private:
  bool is_inited_;
  // holds info of dict
  hash::ObHashMap<ObFTDictInfoKey, ObFTDictInfo> dict_map_;
  ObBucketLock rw_dict_lock_;
  // published views, read without any lock
  ObFTDictViewSlot view_slots_[MAX_DICT_VIEWS];
  int64_t view_slot_cnt_;
  lib::ObMutex view_build_lock_;
  ObArenaAllocator view_alloc_; // owns the views themselves
  mutable int64_t view_fallback_cnt_;
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_
