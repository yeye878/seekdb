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

#ifndef OCEANBASE_SQL_SESSION_OB_FTS_TOKENIZE_CACHE_H_
#define OCEANBASE_SQL_SESSION_OB_FTS_TOKENIZE_CACHE_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/charset/ob_charset.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace sql
{

class ObFTTokenizeCache final
{
public:
  static constexpr int64_t MAX_ENTRIES = 4;

  explicit ObFTTokenizeCache(common::ObIAllocator &allocator)
      : allocator_(allocator), next_entry_(0), lock_(common::ObLatchIds::DEFAULT_SPIN_LOCK)
  {}

  ~ObFTTokenizeCache()
  {
    for (int64_t i = 0; i < MAX_ENTRIES; ++i) {
      entries_[i].reset(allocator_);
    }
  }

  bool get(const common::ObString &fulltext,
           const common::ObString &parser_name,
           const common::ObString &properties,
           const common::ObCollationType collation_type,
           const int64_t output_mode,
           common::ObIAllocator &allocator,
           common::ObString &result)
  {
    bool hit = false;
    common::ObSpinLockGuard guard(lock_);
    for (int64_t i = 0; !hit && i < MAX_ENTRIES; ++i) {
      const Entry &entry = entries_[i];
      if (entry.collation_type_ == collation_type
          && entry.output_mode_ == output_mode
          && string_equal(entry.fulltext_, fulltext)
          && string_equal(entry.parser_name_, parser_name)
          && string_equal(entry.properties_, properties)) {
        result.reset();
        if (!entry.result_.empty()) {
          char *buf = static_cast<char *>(allocator.alloc(entry.result_.length()));
          if (OB_NOT_NULL(buf)) {
            MEMCPY(buf, entry.result_.ptr(), entry.result_.length());
            result.assign_ptr(buf, entry.result_.length());
            hit = true;
          }
        } else {
          hit = true;
        }
      }
    }
    return hit;
  }

  int put(const common::ObString &fulltext,
          const common::ObString &parser_name,
          const common::ObString &properties,
          const common::ObCollationType collation_type,
          const int64_t output_mode,
          const common::ObString &result)
  {
    int ret = common::OB_SUCCESS;
    common::ObSpinLockGuard guard(lock_);
    Entry &entry = entries_[next_entry_];
    entry.reset(allocator_);
    if (OB_FAIL(copy_string(allocator_, fulltext, entry.fulltext_))
        || OB_FAIL(copy_string(allocator_, parser_name, entry.parser_name_))
        || OB_FAIL(copy_string(allocator_, properties, entry.properties_))
        || OB_FAIL(copy_string(allocator_, result, entry.result_))) {
      entry.reset(allocator_);
    } else {
      entry.collation_type_ = collation_type;
      entry.output_mode_ = output_mode;
      next_entry_ = (next_entry_ + 1) % MAX_ENTRIES;
    }
    return ret;
  }

private:
  struct Entry
  {
    Entry()
        : collation_type_(common::CS_TYPE_INVALID), output_mode_(0)
    {}

    void reset(common::ObIAllocator &allocator)
    {
      release(allocator, fulltext_);
      release(allocator, parser_name_);
      release(allocator, properties_);
      release(allocator, result_);
      collation_type_ = common::CS_TYPE_INVALID;
      output_mode_ = 0;
    }

    static void release(common::ObIAllocator &allocator, common::ObString &str)
    {
      if (!str.empty()) {
        allocator.free(str.ptr());
      }
      str.reset();
    }

    common::ObString fulltext_;
    common::ObString parser_name_;
    common::ObString properties_;
    common::ObCollationType collation_type_;
    int64_t output_mode_;
    common::ObString result_;
  };

  static bool string_equal(const common::ObString &left, const common::ObString &right)
  {
    return left.length() == right.length()
        && (0 == left.length()
            || 0 == MEMCMP(left.ptr(), right.ptr(), left.length()));
  }

  static int copy_string(common::ObIAllocator &allocator,
                         const common::ObString &source,
                         common::ObString &target)
  {
    int ret = common::OB_SUCCESS;
    target.reset();
    if (!source.empty()) {
      char *buf = static_cast<char *>(allocator.alloc(source.length()));
      if (OB_ISNULL(buf)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, source.ptr(), source.length());
        target.assign_ptr(buf, source.length());
      }
    }
    return ret;
  }

private:
  common::ObIAllocator &allocator_;
  Entry entries_[MAX_ENTRIES];
  int64_t next_entry_;
  mutable common::ObSpinLock lock_;

  DISALLOW_COPY_AND_ASSIGN(ObFTTokenizeCache);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_SESSION_OB_FTS_TOKENIZE_CACHE_H_
