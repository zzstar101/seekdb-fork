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

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/container/ob_se_array.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
  destroy_views();
  return ret;
}

void ObFTDictHub::destroy_views()
{
  int64_t slot_cnt = ATOMIC_LOAD(&view_slot_cnt_);
  for (int64_t i = 0; i < slot_cnt && i < MAX_DICT_VIEWS; ++i) {
    ObFTDictHubView *view = reinterpret_cast<ObFTDictHubView *>(view_slots_[i].view_);
    if (OB_NOT_NULL(view)) {
      if (OB_NOT_NULL(view->range_dict_)) {
        view->range_dict_->~ObFTRangeDict();
        view->range_dict_ = nullptr;
      }
      view->~ObFTDictHubView();
    }
    view_slots_[i].view_key_ = 0;
    view_slots_[i].view_ = 0;
  }
  ATOMIC_STORE(&view_slot_cnt_, 0);
  view_alloc_.reset();
}

void ObFTDictHub::invalidate_views()
{
  // Readers hold no lock, so this is only safe while no tokenization is in
  // flight. The built-in dictionaries are immutable and never reloaded today,
  // which is why nothing calls this yet.
  lib::ObMutexGuard guard(view_build_lock_);
  destroy_views();
}

int ObFTDictHub::find_view(const int64_t view_key, ObFTDictHubView *&view) const
{
  int ret = OB_ENTRY_NOT_EXIST;
  const int64_t slot_cnt = ATOMIC_LOAD(&view_slot_cnt_);
  for (int64_t i = 0; i < slot_cnt && i < MAX_DICT_VIEWS; ++i) {
    if (view_key == ATOMIC_LOAD(&view_slots_[i].view_key_)) {
      view = reinterpret_cast<ObFTDictHubView *>(ATOMIC_LOAD(&view_slots_[i].view_));
      ret = OB_SUCCESS;
      break;
    }
  }
  return ret;
}

int ObFTDictHub::get_builtin_ik_dicts(const ObCharsetType charset,
                                      const ObCollationType coll_type,
                                      ObIFTDict *&main_dict,
                                      ObIFTDict *&quan_dict,
                                      ObIFTDict *&stop_dict)
{
  int ret = OB_SUCCESS;
  main_dict = nullptr;
  quan_dict = nullptr;
  stop_dict = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("dict hub is not inited", K(ret));
  }
  const ObFTDictType types[] = {
      ObFTDictType::DICT_IK_MAIN, ObFTDictType::DICT_IK_QUAN, ObFTDictType::DICT_IK_STOP};
  const char *names[] = {"main_dict", "quan_dict", "stopword"};
  ObIFTDict **dicts[] = {&main_dict, &quan_dict, &stop_dict};
  for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
    const int64_t view_key = make_view_key(charset, coll_type, types[i]);
    ObFTDictHubView *view = nullptr;
    if (OB_SUCC(find_view(view_key, view))) {
    } else if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("failed to find dict view", K(ret), K(view_key));
    } else {
      ObFTDictDesc desc(names[i], types[i], CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
      if (OB_FAIL(build_view(view_key, desc, view))) {
        LOG_WARN("failed to build dict view", K(ret), K(view_key));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(view) || OB_ISNULL(view->range_dict_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null dict view", K(ret), KP(view));
      } else {
        *dicts[i] = view->range_dict_;
      }
    }
  }
  if (OB_SUCC(ret) && (OB_ISNULL(main_dict) || OB_ISNULL(quan_dict) || OB_ISNULL(stop_dict))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null dict", K(ret), KP(main_dict), KP(quan_dict), KP(stop_dict));
  }
  return ret;
}

int ObFTDictHub::build_view(const int64_t view_key,
                            const ObFTDictDesc &desc,
                            ObFTDictHubView *&view)
{
  int ret = OB_SUCCESS;
  view = nullptr;
  lib::ObMutexGuard guard(view_build_lock_);
  // another thread may have built it while we waited for the lock
  if (OB_SUCC(find_view(view_key, view))) {
  } else if (OB_ENTRY_NOT_EXIST != ret) {
    LOG_WARN("failed to find dict view", K(ret), K(view_key));
  } else if (FALSE_IT(ret = OB_SUCCESS)) {
  } else {
    const int64_t slot_cnt = ATOMIC_LOAD(&view_slot_cnt_);
    ObFTDictHubView *new_view = nullptr;
    if (OB_UNLIKELY(slot_cnt >= MAX_DICT_VIEWS)) {
      // Never evict a published view: readers use it without a lock, so freeing
      // it here could pull the dictionaries out from under a concurrent
      // tokenization. The caller falls back to building them per call instead.
      const int64_t fallback_cnt = ATOMIC_AAF(&view_fallback_cnt_, 1);
      ret = OB_SIZE_OVERFLOW;
      if (1 == fallback_cnt) {
        LOG_INFO("no free dictionary view slot, build dicts per call", K(slot_cnt), K(view_key));
      }
    } else if (OB_ISNULL(new_view = OB_NEWx(ObFTDictHubView, &view_alloc_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate dict view", K(ret));
    } else if (OB_FAIL(materialize_dict(desc, *new_view))) {
      LOG_WARN("failed to materialize dict", K(ret), K(desc.type_));
    } else {
      // publish: the view pointer is visible before the key that matches it
      ATOMIC_STORE(&view_slots_[slot_cnt].view_,
                   reinterpret_cast<int64_t>(new_view));
      ATOMIC_STORE(&view_slots_[slot_cnt].view_key_, view_key);
      ATOMIC_STORE(&view_slot_cnt_, slot_cnt + 1);
      view = new_view;
    }
  }
  return ret;
}

int ObFTDictHub::materialize_dict(const ObFTDictDesc &desc, ObFTDictHubView &view)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_alloc(lib::ObMemAttr("FTDictViewTmp"));
  ObFTCacheRangeContainer tmp_container(tmp_alloc);
  if (OB_FAIL(load_cache(desc, tmp_container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      // the dictionary was not cached yet, build it once
      if (OB_FAIL(build_cache(desc, tmp_container))) {
        LOG_WARN("failed to build cache for dict", K(ret), K(desc.type_));
      }
    } else {
      LOG_WARN("failed to load cache for dict", K(ret), K(desc.type_));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(view.range_dict_ = OB_NEWx(ObFTRangeDict,
                                                  &view.alloc_,
                                                  view.alloc_,
                                                  nullptr,
                                                  desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate range dict", K(ret));
  } else {
    common::ObSEArray<ObFTDAT *, 8> dat_blocks;
    const ObList<ObFTCacheRangeHandle *, ObIAllocator> &handles = tmp_container.get_handles();
    for (ObList<ObFTCacheRangeHandle *, ObIAllocator>::const_iterator iter = handles.begin();
         OB_SUCC(ret) && iter != handles.end();
         ++iter) {
      ObFTCacheRangeHandle *handle = *iter;
      ObFTDAT *src_dat = nullptr;
      char *buf = nullptr;
      int64_t block_size = 0;
      if (OB_ISNULL(handle) || OB_ISNULL(handle->value_)
          || OB_ISNULL(src_dat = handle->value_->dat_block_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null dict cache handle", K(ret), KP(handle));
      } else if (OB_UNLIKELY((block_size = src_dat->mem_block_size_) <
                             static_cast<int64_t>(sizeof(ObFTDAT)))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected dict block size", K(ret), K(block_size));
      } else if (OB_ISNULL(buf = static_cast<char *>(
                               view.alloc_.alloc_aligned(block_size, sizeof(int64_t))))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate dict block", K(ret), K(block_size));
      } else if (FALSE_IT(MEMCPY(buf, src_dat, block_size))) {
      } else if (OB_FAIL(dat_blocks.push_back(reinterpret_cast<ObFTDAT *>(buf)))) {
        LOG_WARN("failed to push back dict block", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(view.range_dict_->build_dict_from_dat_blocks(dat_blocks))) {
      LOG_WARN("failed to build range dict from dict blocks", K(ret), K(desc.type_));
    }
  }
  return ret;
}

int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  int32_t range_count = 0;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      // read the record in place: copying the whole ObFTDictInfo out of the map
      // would touch its 2KB name buffer on every call.
      const ObFTDictInfo *info = dict_map_.get(key);
      if (OB_ISNULL(info)) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
        range_count = info->range_count_;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, range_count, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
