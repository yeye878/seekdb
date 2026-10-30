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

#define USING_LOG_PREFIX SQL_ENG

#include "ob_file_scan_utils.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "lib/utility/ob_print_utils.h"
#include "lib/charset/ob_charset.h"
#include "common/timezone/ob_time_convert.h"

namespace oceanbase
{
namespace sql
{

OB_SERIALIZE_MEMBER(ObFileColumn, name_, type_, nullable_, ordinal_);
OB_SERIALIZE_MEMBER(ObFileSchema, columns_, column_names_);
OB_SERIALIZE_MEMBER(ObFileReadOptions, has_header_, format_, delimiter_, quote_, escape_,
                    null_str_, sample_size_);
OB_SERIALIZE_MEMBER(ObFileIdentity, valid_, device_, inode_, size_, mtime_ns_, ctime_ns_);
OB_SERIALIZE_MEMBER(ObFileTableDef, options_, path_, format_, schema_, identity_);

const char *ob_file_column_type_name(const ObFileColumnType type)
{
  const char *name = "NULL";
  switch (type) {
    case OB_FILE_TYPE_NULL: name = "NULL"; break;
    case OB_FILE_TYPE_BOOLEAN: name = "BOOLEAN"; break;
    case OB_FILE_TYPE_BIGINT: name = "BIGINT"; break;
    case OB_FILE_TYPE_DOUBLE: name = "DOUBLE"; break;
    case OB_FILE_TYPE_DATE: name = "DATE"; break;
    case OB_FILE_TYPE_DATETIME: name = "DATETIME"; break;
    case OB_FILE_TYPE_VARCHAR: name = "VARCHAR"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

const char *ob_file_format_name(const ObFileFormat format)
{
  const char *name = "csv";
  switch (format) {
    case OB_FILE_FORMAT_CSV: name = "csv"; break;
    case OB_FILE_FORMAT_JSON: name = "json"; break;
    case OB_FILE_FORMAT_PARQUET: name = "parquet"; break;
    case OB_FILE_FORMAT_GLOB: name = "glob"; break;
    default: name = "csv"; break;
  }
  return name;
}

common::ObObjType ob_file_column_to_obj_type(const ObFileColumnType type)
{
  common::ObObjType obj_type = common::ObVarcharType;
  switch (type) {
    case OB_FILE_TYPE_BOOLEAN: obj_type = common::ObTinyIntType; break;
    case OB_FILE_TYPE_BIGINT: obj_type = common::ObIntType; break;
    case OB_FILE_TYPE_DOUBLE: obj_type = common::ObDoubleType; break;
    case OB_FILE_TYPE_DATE: obj_type = common::ObDateType; break;
    case OB_FILE_TYPE_DATETIME: obj_type = common::ObDateTimeType; break;
    case OB_FILE_TYPE_VARCHAR: obj_type = common::ObVarcharType; break;
    // A column that stayed NULL for every row still needs a concrete type.
    case OB_FILE_TYPE_NULL:
    default: obj_type = common::ObVarcharType; break;
  }
  return obj_type;
}

namespace
{

int copy_file_string(common::ObIAllocator &allocator,
                     const common::ObString &src,
                     common::ObString &dst)
{
  int ret = OB_SUCCESS;
  if (src.empty()) {
    dst.reset();
  } else {
    char *buf = static_cast<char *>(allocator.alloc(src.length() + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      buf[src.length()] = '\0';
      dst.assign_ptr(buf, src.length());
    }
  }
  return ret;
}

void fill_file_identity(const struct stat &st, ObFileIdentity &identity)
{
  identity.valid_ = true;
  identity.device_ = static_cast<uint64_t>(st.st_dev);
  identity.inode_ = static_cast<uint64_t>(st.st_ino);
  identity.size_ = static_cast<int64_t>(st.st_size);
#if defined(__APPLE__)
  identity.mtime_ns_ = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
  identity.ctime_ns_ = static_cast<int64_t>(st.st_ctimespec.tv_sec) * 1000000000 + st.st_ctimespec.tv_nsec;
#elif defined(_WIN32)
  identity.mtime_ns_ = static_cast<int64_t>(st.st_mtime) * 1000000000;
  identity.ctime_ns_ = static_cast<int64_t>(st.st_ctime) * 1000000000;
#else
  identity.mtime_ns_ = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
  identity.ctime_ns_ = static_cast<int64_t>(st.st_ctim.tv_sec) * 1000000000 + st.st_ctim.tv_nsec;
#endif
}

} // anonymous namespace

int ob_file_sql_get_file_identity(const common::ObString &path, ObFileIdentity &identity)
{
  int ret = OB_SUCCESS;
  struct stat st;
  identity = ObFileIdentity();
  if (0 != stat(path.ptr(), &st)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to stat input file", K(ret), K(path), K(errno));
  } else {
    fill_file_identity(st, identity);
  }
  return ret;
}

bool ob_file_sql_same_identity(const ObFileIdentity &lhs, const ObFileIdentity &rhs)
{
  return lhs.valid_ == rhs.valid_ &&
      (!lhs.valid_ || (lhs.device_ == rhs.device_ && lhs.inode_ == rhs.inode_ &&
                       lhs.size_ == rhs.size_ && lhs.mtime_ns_ == rhs.mtime_ns_ &&
                       lhs.ctime_ns_ == rhs.ctime_ns_));
}

int ObFileTableDef::deep_copy(common::ObIAllocator &allocator, const ObFileTableDef &other)
{
  int ret = OB_SUCCESS;
  options_ = other.options_;
  format_ = other.format_;
  identity_ = other.identity_;
  if (OB_FAIL(copy_file_string(allocator, other.options_.format_, options_.format_))) {
  } else if (OB_FAIL(copy_file_string(allocator, other.options_.null_str_, options_.null_str_))) {
  } else if (OB_FAIL(copy_file_string(allocator, other.path_, path_))) {
  } else if (OB_FAIL(schema_.columns_.assign(other.schema_.columns_))) {
  } else if (OB_FAIL(schema_.column_names_.assign(other.schema_.column_names_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < schema_.columns_.count(); ++i) {
    common::ObString name;
    if (OB_FAIL(copy_file_string(allocator, other.schema_.columns_.at(i).name_, name))) {
    } else {
      schema_.columns_.at(i).name_ = name;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < schema_.column_names_.count(); ++i) {
    common::ObString name;
    if (OB_FAIL(copy_file_string(allocator, other.schema_.column_names_.at(i), name))) {
    } else {
      schema_.column_names_.at(i) = name;
    }
  }
  return ret;
}

int ob_file_sql_cell_to_obj(common::ObIAllocator &allocator,
                            const common::ObString &text,
                            const ObFileColumnType type,
                            const common::ObTimeZoneInfo *tz_info,
                            const common::ObCollationType collation,
                            common::ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (OB_FILE_TYPE_BOOLEAN == type) {
    obj.set_tinyint(text.length() == 4 && 0 == strncasecmp(text.ptr(), "true", 4) ? 1 : 0);
  } else if (OB_FILE_TYPE_BIGINT == type) {
    int err = 0;
    errno = 0;
    const int64_t value = common::ObCharset::strntoll(text.ptr(), text.length(), 10, &err);
    if (0 != err) {
      ret = OB_INVALID_NUMERIC;
      LOG_WARN("failed to parse integer file cell", K(ret), K(text), K(err));
    } else {
      obj.set_int(value);
    }
  } else if (OB_FILE_TYPE_DOUBLE == type) {
    int err = 0;
    char *end_ptr = nullptr;
    errno = 0;
    const double value = common::ObCharset::strntod(text.ptr(), text.length(), &end_ptr, &err);
    if (0 != err || end_ptr != text.ptr() + text.length()) {
      ret = OB_INVALID_NUMERIC;
      LOG_WARN("failed to parse double file cell", K(ret), K(text), K(err));
    } else {
      obj.set_double(value);
    }
  } else if (OB_FILE_TYPE_DATE == type) {
    int32_t value = 0;
    if (OB_FAIL(common::ObTimeConverter::str_to_date(text, value))) {
      LOG_WARN("failed to parse date file cell", K(ret), K(text));
    } else {
      obj.set_date(value);
    }
  } else if (OB_FILE_TYPE_DATETIME == type) {
    common::ObString normalized = text;
    if (text.length() > 10 && (text[10] == 'T' || text[10] == 't')) {
      if (OB_FAIL(copy_file_string(allocator, text, normalized))) {
      } else {
        normalized.ptr()[10] = ' ';
      }
    }
    common::ObTimeConvertCtx cvrt_ctx(tz_info, false);
    int64_t value = 0;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(common::ObTimeConverter::str_to_datetime(normalized, cvrt_ctx, value))) {
      LOG_WARN("failed to parse datetime file cell", K(ret), K(text));
    } else {
      obj.set_datetime(value);
      obj.set_scale(0);
    }
  } else {
    common::ObString value;
    if (text.empty()) {
      obj.set_varchar("", 0);
    } else if (OB_FAIL(copy_file_string(allocator, text, value))) {
    } else {
      obj.set_varchar(value.ptr(), value.length());
    }
    if (OB_SUCC(ret)) {
      obj.set_collation_type(collation);
    }
  }
  return ret;
}

namespace
{

bool is_all_digits(const char *ptr, const int64_t len, int64_t &pos)
{
  bool ok = len > 0;
  pos = 0;
  if (ok && (ptr[0] == '-' || ptr[0] == '+')) {
    pos = 1;
    ok = len > 1;
  }
  for (; ok && pos < len; ++pos) {
    ok = ptr[pos] >= '0' && ptr[pos] <= '9';
  }
  return ok;
}

// Parses YYYY-MM-DD exactly; returns true only for a real calendar date.
bool parse_date_strict(const char *ptr, const int64_t len)
{
  bool ok = len == 10 && ptr[4] == '-' && ptr[7] == '-';
  int year = 0;
  int month = 0;
  int day = 0;
  int64_t pos = 0;
  if (ok) {
    int64_t p = 0;
    ok = is_all_digits(ptr, 4, p) && is_all_digits(ptr + 5, 2, p) && is_all_digits(ptr + 8, 2, p);
  }
  if (ok) {
    for (int i = 0; i < 4; ++i) year = year * 10 + (ptr[i] - '0');
    month = (ptr[5] - '0') * 10 + (ptr[6] - '0');
    day = (ptr[8] - '0') * 10 + (ptr[9] - '0');
    ok = year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1;
    if (ok) {
      static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
      const int max_day = month == 2 && leap ? 29 : days_in_month[month - 1];
      ok = day <= max_day;
    }
  }
  UNUSED(pos);
  return ok;
}

// Parses YYYY-MM-DD HH:MM:SS or the ISO 'T' variant.
bool parse_datetime_strict(const char *ptr, const int64_t len)
{
  bool ok = (len == 19 || len == 22) && parse_date_strict(ptr, 10);
  if (ok) {
    ok = (ptr[10] == ' ' || ptr[10] == 'T') &&
          ptr[13] == ':' && ptr[16] == ':';
    if (ok) {
      int hour = (ptr[11] - '0') * 10 + (ptr[12] - '0');
      int minute = (ptr[14] - '0') * 10 + (ptr[15] - '0');
      int second = (ptr[17] - '0') * 10 + (ptr[18] - '0');
      ok = ptr[11] >= '0' && ptr[11] <= '9' && ptr[12] >= '0' && ptr[12] <= '9' &&
           ptr[14] >= '0' && ptr[14] <= '9' && ptr[15] >= '0' && ptr[15] <= '9' &&
           ptr[17] >= '0' && ptr[17] <= '9' && ptr[18] >= '0' && ptr[18] <= '9' &&
           hour <= 23 && minute <= 59 && second <= 59;
      if (ok && len == 22) {
        ok = ptr[19] == '.' &&
             ptr[20] >= '0' && ptr[20] <= '9' && ptr[21] >= '0' && ptr[21] <= '9';
      }
    }
  }
  return ok;
}

bool parse_bool(const char *ptr, const int64_t len)
{
  return (len == 4 && 0 == strncasecmp(ptr, "true", 4)) ||
         (len == 5 && 0 == strncasecmp(ptr, "false", 5));
}

bool parse_double(const char *ptr, const int64_t len)
{
  bool ok = len > 0;
  bool seen_digit = false;
  bool seen_dot = false;
  bool seen_exp = false;
  bool exp_has_digit = true;
  int64_t i = 0;
  if (ok && (ptr[0] == '-' || ptr[0] == '+')) {
    i = 1;
    ok = len > 1;
  }
  for (; ok && i < len; ++i) {
    const char c = ptr[i];
    if (c >= '0' && c <= '9') {
      seen_digit = true;
      if (seen_exp) {
        exp_has_digit = true;
      }
    } else if (c == '.' && !seen_dot && !seen_exp) {
      seen_dot = true;
    } else if ((c == 'e' || c == 'E') && !seen_exp && seen_digit) {
      seen_exp = true;
      exp_has_digit = false;
      if (i + 1 < len && (ptr[i + 1] == '-' || ptr[i + 1] == '+')) {
        ++i;
      }
    } else {
      ok = false;
    }
  }
  return ok && seen_digit && exp_has_digit;
}

// Classifies one raw cell. The empty/NULL string maps to NULL so the caller can
// distinguish "absent" from "empty string".
ObFileColumnType classify_cell(const char *ptr, const int64_t len,
                               const ObFileReadOptions &options)
{
  ObFileColumnType type = OB_FILE_TYPE_VARCHAR;
  if (0 == len) {
    type = OB_FILE_TYPE_NULL;
  } else if (!options.null_str_.empty() &&
             len == options.null_str_.length() &&
             0 == strncmp(ptr, options.null_str_.ptr(), len)) {
    type = OB_FILE_TYPE_NULL;
  } else if (parse_bool(ptr, len)) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else if (parse_datetime_strict(ptr, len)) {
    type = OB_FILE_TYPE_DATETIME;
  } else if (parse_date_strict(ptr, len)) {
    type = OB_FILE_TYPE_DATE;
  } else {
    int64_t digits_pos = 0;
    if (is_all_digits(ptr, len, digits_pos)) {
      int err = 0;
      errno = 0;
      common::ObCharset::strntoll(ptr, len, 10, &err);
      type = 0 == err ? OB_FILE_TYPE_BIGINT : OB_FILE_TYPE_DOUBLE;
      if (0 != err) {
        int double_err = 0;
        char *end_ptr = nullptr;
        errno = 0;
        common::ObCharset::strntod(ptr, len, &end_ptr, &double_err);
        if (0 != double_err || end_ptr != ptr + len) {
          type = OB_FILE_TYPE_VARCHAR;
        }
      }
    } else if (parse_double(ptr, len)) {
      int err = 0;
      char *end_ptr = nullptr;
      errno = 0;
      common::ObCharset::strntod(ptr, len, &end_ptr, &err);
      type = (0 == err && end_ptr == ptr + len) ? OB_FILE_TYPE_DOUBLE : OB_FILE_TYPE_VARCHAR;
    }
  }
  return type;
}

// Widening rule between an inferred column type and a new value type.
ObFileColumnType widen_type(const ObFileColumnType current, const ObFileColumnType incoming)
{
  ObFileColumnType result = current;
  if (OB_FILE_TYPE_NULL == incoming) {
    result = current;
  } else if (OB_FILE_TYPE_NULL == current) {
    result = incoming;
  } else if (current == incoming) {
    result = current;
  } else if ((OB_FILE_TYPE_BIGINT == current && OB_FILE_TYPE_DOUBLE == incoming) ||
             (OB_FILE_TYPE_DOUBLE == current && OB_FILE_TYPE_BIGINT == incoming)) {
    result = OB_FILE_TYPE_DOUBLE;
  } else if ((OB_FILE_TYPE_DATE == current && OB_FILE_TYPE_DATETIME == incoming) ||
             (OB_FILE_TYPE_DATETIME == current && OB_FILE_TYPE_DATE == incoming)) {
    // A date and a timestamp in the same column must both survive, so the column
    // becomes a timestamp.
    result = OB_FILE_TYPE_DATETIME;
  } else {
    // Any other combination has no common scalar type.
    result = OB_FILE_TYPE_VARCHAR;
  }
  return result;
}

// Reports whether a path contains glob metacharacters. The string is scanned by
// length rather than through strpbrk so a non-terminated ObString is safe.
bool has_glob_meta(const common::ObString &path)
{
  for (int32_t i = 0; i < path.length(); ++i) {
    const char c = path[i];
    if (c == '*' || c == '?' || c == '[') {
      return true;
    }
  }
  return false;
}

const char *path_basename(const char *path)
{
  const char *base = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/') {
      base = p + 1;
    }
  }
  return base;
}

// Rejects a resolved path that is not inside `secure_file_priv`. Both sides are
// made absolute first so that ".." cannot escape the allowed directory.
int check_path_contained(common::ObIAllocator &allocator,
                         const common::ObString &path,
                         const common::ObString &secure_file_priv,
                         common::ObString &resolved,
                         bool log_user_error = true)
{
  int ret = OB_SUCCESS;
  char resolved_buf[PATH_MAX];
  if (OB_ISNULL(realpath(path.ptr(), resolved_buf))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("file does not exist or cannot be resolved", K(ret), K(path), K(errno));
  } else if (!secure_file_priv.empty() && 0 != secure_file_priv.case_compare("/")) {
    char priv_buf[PATH_MAX];
    if (OB_ISNULL(realpath(secure_file_priv.ptr(), priv_buf))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("secure_file_priv does not exist", K(ret), K(secure_file_priv));
    } else {
      const int64_t priv_len = static_cast<int64_t>(strlen(priv_buf));
      const bool contained = priv_len == 1 ? priv_buf[0] == '/' :
          0 == strncmp(resolved_buf, priv_buf, priv_len) &&
          (resolved_buf[priv_len] == '/' || resolved_buf[priv_len] == '\0');
      if (!contained) {
        ret = OB_INVALID_ARGUMENT;
        if (log_user_error) {
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path is outside secure_file_priv");
        }
        LOG_WARN("path escapes secure_file_priv", K(ret), K(path), K(secure_file_priv),
                 K(resolved_buf), K(priv_buf), K(contained));
      }
    }
  }
  if (OB_SUCC(ret)) {
    const int64_t len = static_cast<int64_t>(strlen(resolved_buf));
    char *ptr = static_cast<char *>(allocator.alloc(len + 1));
    if (OB_ISNULL(ptr)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(ptr, resolved_buf, len);
      ptr[len] = '\0';
      resolved.assign_ptr(ptr, static_cast<int32_t>(len));
    }
  }
  return ret;
}

} // anonymous namespace

int ob_file_sql_check_access(common::ObIAllocator &allocator,
                             const common::ObString &path,
                             const common::ObString &secure_file_priv,
                             bool enable_file_sql)
{
  int ret = OB_SUCCESS;
  common::ObString resolved;
  struct stat st;
  if (!enable_file_sql) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file table function is disabled, set enable_file_sql=ON first");
  } else if (path.empty() || path.length() >= PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path is empty or too long");
  } else if (strstr(path.ptr(), "://") != nullptr) {
    // Remote and special-scheme paths are explicitly out of scope.
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "only local file paths are supported");
  } else if (OB_FAIL(check_path_contained(allocator, path, secure_file_priv, resolved))) {
  } else if (0 != stat(resolved.ptr(), &st)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to stat file", K(ret), K(resolved), K(errno));
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "path is not a regular file");
  }
  return ret;
}

int ob_file_sql_resolve_paths(common::ObIAllocator &allocator,
                              const common::ObString &pattern,
                              const common::ObString &secure_file_priv,
                              common::ObIArray<common::ObString> &paths)
{
  int ret = OB_SUCCESS;
  paths.reuse();
  if (pattern.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file pattern is empty");
  } else if (strstr(pattern.ptr(), "://") != nullptr) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "only local file paths are supported");
  } else if (!has_glob_meta(pattern)) {
    common::ObString resolved;
    if (OB_FAIL(check_path_contained(allocator, pattern, secure_file_priv, resolved))) {
    } else if (OB_FAIL(ob_file_sql_check_access(allocator, resolved, secure_file_priv, true))) {
    } else if (OB_FAIL(paths.push_back(resolved))) {
    }
  } else {
    const char *raw = pattern.ptr();
    const char *base = path_basename(raw);
    common::ObString dir;
    if (base == raw) {
      dir = common::ObString::make_string(".");
    } else {
      dir.assign_ptr(raw, static_cast<int32_t>(base - raw));
    }
    common::ObString resolved_dir;
    common::ObString dir_input;
    if (OB_FAIL(copy_file_string(allocator, dir, dir_input))) {
    } else if (OB_FAIL(check_path_contained(allocator, dir_input, secure_file_priv, resolved_dir))) {
    } else {
      DIR *d = opendir(resolved_dir.ptr());
      if (OB_ISNULL(d)) {
        ret = OB_FILE_NOT_EXIST;
        LOG_WARN("failed to open directory", K(ret), K(resolved_dir), K(errno));
      } else {
        const bool pattern_has_leading_dot = (base != nullptr && base[0] == '.');
        struct dirent *ent = nullptr;
        while (OB_SUCC(ret) && OB_NOT_NULL(ent = readdir(d))) {
          if (0 == strcmp(ent->d_name, ".") || 0 == strcmp(ent->d_name, "..") ||
              (!pattern_has_leading_dot && ent->d_name[0] == '.') ||
              0 != fnmatch(base, ent->d_name, FNM_PERIOD)) {
            continue;
          }
          const int64_t name_len = static_cast<int64_t>(strlen(ent->d_name));
          const int64_t full_len = resolved_dir.length() + 1 + name_len;
          char *full = static_cast<char *>(allocator.alloc(full_len + 1));
          if (OB_ISNULL(full)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            break;
          }
          MEMCPY(full, resolved_dir.ptr(), resolved_dir.length());
          full[resolved_dir.length()] = '/';
          MEMCPY(full + resolved_dir.length() + 1, ent->d_name, name_len);
          full[full_len] = '\0';
          common::ObString candidate;
          const int check_ret = check_path_contained(allocator, common::ObString(full_len, full),
                                                     secure_file_priv, candidate, false);
          if (OB_ALLOCATE_MEMORY_FAILED == check_ret) {
            ret = check_ret;
            break;
          } else if (OB_SUCCESS != check_ret) {
            continue;
          }
          struct stat st;
          if (0 != stat(candidate.ptr(), &st) || !S_ISREG(st.st_mode)) {
            continue;
          }
          if (OB_FAIL(paths.push_back(candidate))) {
            break;
          }
        }
        closedir(d);
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 1; i < paths.count(); ++i) {
      common::ObString value = paths.at(i);
      int64_t j = i;
      while (j > 0 && paths.at(j - 1).case_compare(value) > 0) {
        paths.at(j) = paths.at(j - 1);
        --j;
      }
      paths.at(j) = value;
    }
  }
  return ret;
}

// Bounded local-file input used by the CSV and JSONL scanners. The buffer is
// allocated once; rows never require the whole file to be resident in memory.
class ObStreamFile
{
public:
  ObStreamFile()
    : fp_(nullptr), buf_(nullptr), pos_(0), size_(0), eof_(false) {}
  ~ObStreamFile() { close(); }

  int init(common::ObIAllocator &allocator, const common::ObString &path)
  {
    int ret = OB_SUCCESS;
    close();
    if (OB_ISNULL(buf_)) {
      buf_ = static_cast<char *>(allocator.alloc(BUFFER_SIZE));
      if (OB_ISNULL(buf_)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    if (OB_SUCC(ret) && OB_ISNULL(fp_ = fopen(path.ptr(), "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open file", K(ret), K(path), K(errno));
    }
    pos_ = size_ = 0;
    eof_ = false;
    return ret;
  }

  int get(char &c)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= size_) {
      if (eof_) {
        ret = OB_ITER_END;
      } else if (OB_ISNULL(fp_)) {
        ret = OB_NOT_INIT;
      } else {
        size_ = fread(buf_, 1, BUFFER_SIZE, fp_);
        pos_ = 0;
        if (0 == size_) {
          eof_ = true;
          if (ferror(fp_)) {
            ret = OB_IO_ERROR;
          } else {
            ret = OB_ITER_END;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      c = buf_[pos_++];
    }
    return ret;
  }

  int peek(char &c)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= size_) {
      char tmp = 0;
      if (OB_FAIL(get(tmp))) {
      } else {
        --pos_;
        c = tmp;
      }
    } else {
      c = buf_[pos_];
    }
    return ret;
  }

  void close()
  {
    if (OB_NOT_NULL(fp_)) {
      fclose(fp_);
      fp_ = nullptr;
    }
    pos_ = size_ = 0;
    eof_ = false;
  }

  static const int64_t BUFFER_SIZE = 64 * 1024;

private:
  FILE *fp_;
  char *buf_;
  int64_t pos_;
  int64_t size_;
  bool eof_;
};

struct ObCsvFieldBuf
{
  ObCsvFieldBuf() : buf_(nullptr), len_(0), cap_(0), quoted_(false), started_(false) {}
  char *buf_;
  int64_t len_;
  int64_t cap_;
  bool quoted_;
  bool started_;

  int append(common::ObIAllocator &allocator, const char c)
  {
    int ret = OB_SUCCESS;
    if (len_ + 1 > cap_) {
      const int64_t new_cap = cap_ == 0 ? 64 : cap_ * 2;
      char *grown = static_cast<char *>(allocator.alloc(new_cap));
      if (OB_ISNULL(grown)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (len_ > 0) {
          MEMCPY(grown, buf_, len_);
        }
        buf_ = grown;
        cap_ = new_cap;
      }
    }
    if (OB_SUCC(ret)) {
      buf_[len_++] = c;
    }
    return ret;
  }

  int finish(common::ObIAllocator &allocator, common::ObString &out)
  {
    int ret = OB_SUCCESS;
    char *ptr = static_cast<char *>(allocator.alloc(len_ + 1));
    if (OB_ISNULL(ptr)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len_ > 0) {
        MEMCPY(ptr, buf_, len_);
      }
      ptr[len_] = '\0';
      out.assign_ptr(ptr, static_cast<int32_t>(len_));
    }
    return ret;
  }
};

class ObCsvRecordReader
{
public:
  ObCsvRecordReader()
    : file_(), record_arena_(common::ObModIds::OB_SQL_EXECUTOR), options_(),
      line_no_(0), raw_record_size_(0) {}
  ~ObCsvRecordReader() { close(); }

  int init(common::ObIAllocator &allocator,
           const common::ObString &path,
           const ObFileReadOptions &options)
  {
    options_ = options;
    line_no_ = 0;
    raw_record_size_ = 0;
    record_arena_.reset();
    return file_.init(allocator, path);
  }

  int next(common::ObIArray<common::ObString> &fields,
           common::ObIArray<bool> &is_null)
  {
    int ret = OB_SUCCESS;
    fields.reuse();
    is_null.reuse();
    record_arena_.reset();
    raw_record_size_ = 0;
    ObCsvFieldBuf field;
    bool in_quotes = false;
    bool after_quote = false;
    bool have_record = false;
    while (OB_SUCC(ret)) {
      char c = 0;
      const int get_ret = file_.get(c);
      if (OB_ITER_END == get_ret) {
        if (in_quotes) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unterminated quoted CSV field");
        } else if (!have_record) {
          ret = OB_ITER_END;
        } else if (OB_FAIL(finish_field(field, fields, is_null))) {
        }
        break;
      } else if (OB_FAIL(get_ret)) {
      } else {
        have_record = true;
        ++raw_record_size_;
        if (OB_UNLIKELY(raw_record_size_ > MAX_RECORD_SIZE)) {
          ret = OB_SIZE_OVERFLOW;
          LOG_USER_ERROR(OB_SIZE_OVERFLOW);
        } else if (c == '\n') {
          ++line_no_;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (in_quotes) {
        if (c == options_.escape_ && options_.escape_ != options_.quote_) {
          char escaped = 0;
          if (OB_FAIL(file_.get(escaped))) {
            ret = OB_ITER_END == ret ? OB_INVALID_ARGUMENT : ret;
          } else {
            ++raw_record_size_;
            if (OB_FAIL(field.append(record_arena_, escaped))) {
            }
          }
        } else if (c == options_.quote_) {
          char next = 0;
          const int peek_ret = file_.peek(next);
          if (OB_SUCC(peek_ret) && next == options_.quote_) {
            char doubled = 0;
            if (OB_FAIL(file_.get(doubled))) {
            } else {
              ++raw_record_size_;
              if (OB_FAIL(field.append(record_arena_, options_.quote_))) {
              }
            }
          } else {
            in_quotes = false;
            after_quote = true;
          }
        } else if (OB_FAIL(field.append(record_arena_, c))) {
        }
      } else if (after_quote && c != options_.delimiter_ && c != '\n' && c != '\r') {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unexpected character after closing CSV quote");
      } else if (c == options_.quote_ && !field.started_) {
        in_quotes = true;
        field.started_ = true;
        field.quoted_ = true;
      } else if (c == options_.escape_ && options_.escape_ != options_.quote_) {
        char escaped = 0;
        if (OB_FAIL(file_.get(escaped))) {
          ret = OB_ITER_END == ret ? OB_INVALID_ARGUMENT : ret;
        } else {
          ++raw_record_size_;
          field.started_ = true;
          if (OB_FAIL(field.append(record_arena_, escaped))) {
          }
        }
      } else if (c == options_.delimiter_) {
        if (OB_FAIL(finish_field(field, fields, is_null))) {
        } else {
          field = ObCsvFieldBuf();
          after_quote = false;
        }
      } else if (c == '\n' || c == '\r') {
        if (c == '\r') {
          char next = 0;
          if (OB_SUCC(file_.peek(next)) && next == '\n') {
            char lf = 0;
            if (OB_FAIL(file_.get(lf))) {
            } else {
              ++raw_record_size_;
              ++line_no_;
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(finish_field(field, fields, is_null))) {
        }
        break;
      } else {
        field.started_ = true;
        if (OB_FAIL(field.append(record_arena_, c))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to parse CSV record", K(ret), K_(line_no));
    }
    return ret;
  }

  int64_t line_no() const { return line_no_; }

  void close()
  {
    file_.close();
    record_arena_.reset();
  }

  static const int64_t MAX_RECORD_SIZE = 16 * 1024 * 1024;
  static const int64_t MAX_COLUMNS = 1024;

private:
  int finish_field(ObCsvFieldBuf &field,
                   common::ObIArray<common::ObString> &fields,
                   common::ObIArray<bool> &is_null)
  {
    int ret = OB_SUCCESS;
    common::ObString out;
    if (OB_UNLIKELY(fields.count() >= MAX_COLUMNS)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_USER_ERROR(OB_SIZE_OVERFLOW);
    } else if (OB_FAIL(field.finish(record_arena_, out))) {
    } else {
      const bool explicit_null = !options_.null_str_.empty() &&
          out.length() == options_.null_str_.length() &&
          0 == strncmp(out.ptr(), options_.null_str_.ptr(), out.length());
      const bool null_cell = explicit_null || (!field.quoted_ && out.empty());
      if (OB_FAIL(fields.push_back(out))) {
      } else if (OB_FAIL(is_null.push_back(null_cell))) {
      }
    }
    return ret;
  }

  ObStreamFile file_;
  common::ObArenaAllocator record_arena_;
  ObFileReadOptions options_;
  int64_t line_no_;
  int64_t raw_record_size_;
};

struct ObFileCsvScanner::Impl
{
  Impl() : reader_(), schema_(nullptr), options_(), header_skipped_(false) {}
  ObCsvRecordReader reader_;
  const ObFileSchema *schema_;
  ObFileReadOptions options_;
  bool header_skipped_;
};

ObFileCsvScanner::ObFileCsvScanner() : impl_(nullptr) {}

ObFileCsvScanner::~ObFileCsvScanner()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->~Impl();
    impl_ = nullptr;
  }
}

void ObFileCsvScanner::release()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->reader_.close();
    impl_->header_skipped_ = false;
  }
}

int ObFileCsvScanner::init(common::ObIAllocator &allocator,
                           const common::ObString &path,
                           const ObFileReadOptions &options,
                           const ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_)) {
    Impl *impl = static_cast<Impl *>(allocator.alloc(sizeof(Impl)));
    if (OB_ISNULL(impl)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      impl_ = new (impl) Impl();
    }
  } else {
    impl_->reader_.close();
  }
  if (OB_SUCC(ret)) {
    impl_->schema_ = &schema;
    impl_->options_ = options;
    impl_->header_skipped_ = false;
    if (OB_FAIL(impl_->reader_.init(allocator, path, options))) {
    }
  }
  return ret;
}

int ObFileCsvScanner::next_row(common::ObIArray<common::ObString> &fields,
                               common::ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_) || OB_ISNULL(impl_->schema_)) {
    ret = OB_NOT_INIT;
  } else if (!impl_->header_skipped_) {
    impl_->header_skipped_ = true;
    if (impl_->options_.has_header_) {
      common::ObSEArray<common::ObString, 24> header;
      common::ObSEArray<bool, 24> header_null;
      if (OB_FAIL(impl_->reader_.next(header, header_null))) {
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(impl_->reader_.next(fields, is_null))) {
  } else if (OB_SUCC(ret) && 0 == fields.count() && 0 == is_null.count() &&
             impl_->schema_->columns_.count() > 0) {
    ret = OB_ITER_END;
  } else if (OB_SUCC(ret) && OB_UNLIKELY(fields.count() != impl_->schema_->columns_.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "csv row has a different field count than the header");
  }
  return ret;
}

int ob_file_sql_infer_csv(common::ObIAllocator &allocator,
                          const common::ObString &path,
                          const ObFileReadOptions &options,
                          ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  ObCsvRecordReader reader;
  common::ObSEArray<common::ObString, 16> fields;
  common::ObSEArray<bool, 16> is_null;
  common::ObSEArray<ObFileColumnType, 16> types;
  bool first_row = true;
  schema.columns_.reuse();
  schema.column_names_.reuse();
  if (OB_UNLIKELY(options.sample_size_ != -1)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "only sample_size = -1 is supported for csv");
  } else if (OB_FAIL(reader.init(allocator, path, options))) {
  }
  while (OB_SUCC(ret)) {
    const int read_ret = reader.next(fields, is_null);
    if (OB_ITER_END == read_ret) {
      break;
    } else if (OB_FAIL(read_ret)) {
    } else if (first_row && options.has_header_) {
      first_row = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        ObFileColumn col;
        common::ObString base = fields.at(i);
        common::ObString candidate = base;
        int64_t suffix = 2;
        bool conflict = true;
        while (OB_SUCC(ret) && conflict) {
          conflict = false;
          for (int64_t j = 0; !conflict && j < schema.columns_.count(); ++j) {
            conflict = 0 == schema.columns_.at(j).name_.case_compare(candidate);
          }
          if (conflict) {
            char suffix_buf[64];
            const int64_t suffix_len = snprintf(suffix_buf, sizeof(suffix_buf), "__%ld",
                                                static_cast<long>(suffix++));
            char *merged = static_cast<char *>(allocator.alloc(base.length() + suffix_len + 1));
            if (OB_ISNULL(merged)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              MEMCPY(merged, base.ptr(), base.length());
              MEMCPY(merged + base.length(), suffix_buf, suffix_len);
              merged[base.length() + suffix_len] = '\0';
              candidate.assign_ptr(merged, static_cast<int32_t>(base.length() + suffix_len));
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(copy_file_string(allocator, candidate, col.name_))) {
          } else {
            col.ordinal_ = i;
            col.type_ = OB_FILE_TYPE_NULL;
            col.nullable_ = false;
            if (OB_FAIL(schema.columns_.push_back(col))) {
            } else if (OB_FAIL(types.push_back(OB_FILE_TYPE_NULL))) {
            }
          }
        }
      }
    } else {
      if (first_row) {
        first_row = false;
        for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
          char name_buf[32];
          const int64_t name_len = snprintf(name_buf, sizeof(name_buf), "column%ld",
                                            static_cast<long>(i + 1));
          ObFileColumn col;
          common::ObString name;
          if (OB_FAIL(copy_file_string(allocator,
                  common::ObString(static_cast<int32_t>(name_len), name_buf), name))) {
          } else {
            col.name_ = name;
            col.ordinal_ = i;
            col.type_ = OB_FILE_TYPE_NULL;
            col.nullable_ = false;
            if (OB_FAIL(schema.columns_.push_back(col))) {
            } else if (OB_FAIL(types.push_back(OB_FILE_TYPE_NULL))) {
            }
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_UNLIKELY(fields.count() != schema.columns_.count())) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "csv row has a different field count than the header");
        LOG_WARN("csv field count mismatch", K(ret), K(fields.count()), K(schema.columns_.count()));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
          if (is_null.at(i)) {
            schema.columns_.at(i).nullable_ = true;
          } else {
            const ObFileColumnType cell = fields.at(i).empty()
                ? OB_FILE_TYPE_VARCHAR : classify_cell(fields.at(i).ptr(), fields.at(i).length(), options);
            const ObFileColumnType widened = widen_type(types.at(i), cell);
            types.at(i) = widened;
            schema.columns_.at(i).type_ = widened;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && schema.columns_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "csv file has no columns");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < schema.columns_.count(); ++i) {
    if (OB_FAIL(schema.column_names_.push_back(schema.columns_.at(i).name_))) {
    }
  }
  reader.close();
  return ret;
}

// ---------------------------------------------------------------------------
// JSONL
// ---------------------------------------------------------------------------

namespace
{

// Splits the content into lines and hands each non-empty line to `fn`.
int iterate_jsonl_lines(const common::ObString &content,
                        int (*fn)(void *, const char *, int64_t, int64_t),
                        void *ctx)
{
  int ret = OB_SUCCESS;
  const char *p = content.ptr();
  const char *end = content.ptr() + content.length();
  while (OB_SUCC(ret) && p < end) {
    const char *line_start = p;
    while (p < end && *p != '\n') {
      ++p;
    }
    int64_t len = p - line_start;
    if (len > 0 && line_start[len - 1] == '\r') {
      --len;
    }
    if (p < end) {
      ++p;
    }
    if (len > 0 && OB_FAIL(fn(ctx, line_start, len, 0))) {
      // error already reported by fn
    }
  }
  return ret;
}

// Maps a JSON scalar node to the inferred file type.
ObFileColumnType json_node_type(const common::ObJsonNode *node)
{
  ObFileColumnType type = OB_FILE_TYPE_VARCHAR;
  if (OB_ISNULL(node) || node->json_type() == common::ObJsonNodeType::J_NULL) {
    type = OB_FILE_TYPE_NULL;
  } else if (node->json_type() == common::ObJsonNodeType::J_BOOLEAN) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else if (node->json_type() == common::ObJsonNodeType::J_INT) {
    type = OB_FILE_TYPE_BIGINT;
  } else if (node->json_type() == common::ObJsonNodeType::J_UINT) {
    type = static_cast<const common::ObJsonUint *>(node)->value() <= INT64_MAX
           ? OB_FILE_TYPE_BIGINT : OB_FILE_TYPE_DOUBLE;
  } else if (node->json_type() == common::ObJsonNodeType::J_DOUBLE ||
             node->json_type() == common::ObJsonNodeType::J_DECIMAL) {
    type = OB_FILE_TYPE_DOUBLE;
  } else if (node->json_type() == common::ObJsonNodeType::J_STRING) {
    const common::ObString &str = static_cast<const common::ObJsonString *>(node)->get_str();
    type = classify_cell(str.ptr(), str.length(), ObFileReadOptions());
    if (OB_FILE_TYPE_NULL == type) {
      type = OB_FILE_TYPE_VARCHAR;
    }
  } else {
    // Objects and arrays are not representable as scalar columns.
    type = OB_FILE_TYPE_VARCHAR;
  }
  return type;
}

struct JsonlSchemaCtx
{
  common::ObIAllocator *allocator_;
  ObFileSchema *schema_;
};

int jsonl_schema_line(void *ctx, const char *line, const int64_t len, const int64_t line_no)
{
  int ret = OB_SUCCESS;
  JsonlSchemaCtx *c = static_cast<JsonlSchemaCtx *>(ctx);
  common::ObArenaAllocator tmp(common::ObModIds::OB_JSON_PARSER);
  common::ObIJsonBase *j_base = nullptr;
  common::ObJsonObject *obj = nullptr;
  common::ObString line_str(static_cast<int32_t>(len), line);
  if (OB_FAIL(common::ObJsonBaseFactory::get_json_base(&tmp, line_str,
              common::ObJsonInType::JSON_TREE, common::ObJsonInType::JSON_TREE, j_base))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid json line");
    LOG_WARN("failed to parse jsonl line", K(ret), K(line_no));
  } else if (OB_ISNULL(j_base) || j_base->json_type() != common::ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "json line is not a json object");
  } else if (OB_FALSE_IT(obj = static_cast<common::ObJsonObject *>(j_base))) {
  } else {
    // Union of keys: an existing column is widened, a new key is appended.
    for (uint64_t idx = 0; OB_SUCC(ret) && idx < obj->element_count(); ++idx) {
      common::ObString key;
      common::ObJsonNode *value = nullptr;
      if (OB_FAIL(obj->get_value_by_idx(idx, key, value))) {
        LOG_WARN("failed to read json object entry", K(ret), K(idx));
        break;
      } else if (OB_NOT_NULL(value) &&
                 (value->json_type() == common::ObJsonNodeType::J_OBJECT ||
                  value->json_type() == common::ObJsonNodeType::J_ARRAY)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "nested json values are not supported in jsonl");
        break;
      }
      int64_t existing = -1;
      for (int64_t i = 0; -1 == existing && i < c->schema_->columns_.count(); ++i) {
        if (0 == c->schema_->columns_.at(i).name_.case_compare(key)) {
          existing = i;
        }
      }
      const ObFileColumnType incoming = json_node_type(value);
      if (existing >= 0) {
        ObFileColumn &col = c->schema_->columns_.at(existing);
        col.type_ = widen_type(col.type_, incoming);
        if (OB_FILE_TYPE_NULL == incoming) {
          col.nullable_ = true;
        }
      } else {
        ObFileColumn col;
        char *name = static_cast<char *>(c->allocator_->alloc(key.length() + 1));
        if (OB_ISNULL(name)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          if (key.length() > 0) {
            MEMCPY(name, key.ptr(), key.length());
          }
          name[key.length()] = '\0';
          col.name_.assign_ptr(name, key.length());
          col.type_ = incoming;
          col.nullable_ = true;
          col.ordinal_ = c->schema_->columns_.count();
          if (OB_FAIL(c->schema_->columns_.push_back(col))) {
          }
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

class ObJsonLineReader
{
public:
  ObJsonLineReader()
    : file_(), line_arena_(common::ObModIds::OB_SQL_EXECUTOR), line_no_(0) {}
  ~ObJsonLineReader() { close(); }

  int init(common::ObIAllocator &allocator, const common::ObString &path)
  {
    line_no_ = 0;
    line_arena_.reset();
    return file_.init(allocator, path);
  }

  int next(common::ObString &line)
  {
    int ret = OB_SUCCESS;
    line.reset();
    while (OB_SUCC(ret)) {
      line_arena_.reset();
      ObCsvFieldBuf buf;
      bool have_data = false;
      bool finished = false;
      while (OB_SUCC(ret) && !finished) {
        char c = 0;
        const int get_ret = file_.get(c);
        if (OB_ITER_END == get_ret) {
          if (!have_data) {
            ret = OB_ITER_END;
          }
          break;
        } else if (OB_FAIL(get_ret)) {
        } else {
          have_data = true;
          if (OB_UNLIKELY(buf.len_ >= MAX_LINE_SIZE)) {
            ret = OB_SIZE_OVERFLOW;
            LOG_USER_ERROR(OB_SIZE_OVERFLOW);
          } else if (c == '\n') {
            finished = true;
          } else if (OB_FAIL(buf.append(line_arena_, c))) {
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else {
        ++line_no_;
        if (buf.len_ > 0 && buf.buf_[buf.len_ - 1] == '\r') {
          --buf.len_;
        }
        if (0 == buf.len_) {
          // Empty physical lines are ignored without being exposed as records.
          continue;
        }
        if (OB_FAIL(buf.finish(line_arena_, line))) {
        }
        break;
      }
    }
    return ret;
  }

  int64_t line_no() const { return line_no_; }

  void close()
  {
    file_.close();
    line_arena_.reset();
  }

  static const int64_t MAX_LINE_SIZE = 16 * 1024 * 1024;

private:
  ObStreamFile file_;
  common::ObArenaAllocator line_arena_;
  int64_t line_no_;
};

struct ObFileJsonScanner::Impl
{
  Impl()
    : allocator_(nullptr), reader_(), schema_(nullptr),
      row_arena_(common::ObModIds::OB_SQL_EXECUTOR) {}
  common::ObIAllocator *allocator_;
  ObJsonLineReader reader_;
  const ObFileSchema *schema_;
  common::ObArenaAllocator row_arena_;
};

ObFileJsonScanner::ObFileJsonScanner() : impl_(nullptr) {}

ObFileJsonScanner::~ObFileJsonScanner()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->~Impl();
    impl_ = nullptr;
  }
}

void ObFileJsonScanner::release()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->reader_.close();
    impl_->row_arena_.reset();
  }
}

int ObFileJsonScanner::init(common::ObIAllocator &allocator,
                            const common::ObString &path,
                            const ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_)) {
    Impl *impl = static_cast<Impl *>(allocator.alloc(sizeof(Impl)));
    if (OB_ISNULL(impl)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      impl_ = new (impl) Impl();
    }
  } else {
    impl_->reader_.close();
  }
  if (OB_SUCC(ret)) {
    impl_->allocator_ = &allocator;
    impl_->schema_ = &schema;
    impl_->row_arena_.reset();
    if (OB_FAIL(impl_->reader_.init(allocator, path))) {
    }
  }
  return ret;
}

int json_scalar_to_text(common::ObJsonNode *node,
                        common::ObArenaAllocator &tmp,
                        common::ObString &cell)
{
  int ret = OB_SUCCESS;
  char buf[64];
  int64_t len = 0;
  switch (node->json_type()) {
    case common::ObJsonNodeType::J_INT: {
      int64_t v = 0;
      if (OB_FAIL(node->to_int(v, false, false))) {
      } else if (OB_UNLIKELY(0 > (len = snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v))))) {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case common::ObJsonNodeType::J_UINT: {
      const uint64_t v = static_cast<common::ObJsonUint *>(node)->value();
      if (OB_UNLIKELY(0 > (len = snprintf(buf, sizeof(buf), "%lu",
                                          static_cast<unsigned long>(v))))) {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case common::ObJsonNodeType::J_DOUBLE:
    case common::ObJsonNodeType::J_DECIMAL: {
      double v = 0;
      if (OB_FAIL(node->to_double(v))) {
      } else if (OB_UNLIKELY(0 > (len = snprintf(buf, sizeof(buf), "%g", v)))) {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case common::ObJsonNodeType::J_BOOLEAN: {
      const bool v = static_cast<common::ObJsonBoolean *>(node)->value();
      len = v ? 4 : 5;
      MEMCPY(buf, v ? "true" : "false", len + 1);
      break;
    }
    default: {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
  }
  if (OB_SUCC(ret)) {
    char *copy = static_cast<char *>(tmp.alloc(len));
    if (OB_ISNULL(copy) && len > 0) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len > 0) {
        MEMCPY(copy, buf, len);
      }
      cell.assign_ptr(copy, static_cast<int32_t>(len));
    }
  }
  return ret;
}

int ObFileJsonScanner::next_row(common::ObIArray<common::ObString> &fields,
                                common::ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  is_null.reuse();
  if (OB_ISNULL(impl_) || OB_ISNULL(impl_->schema_)) {
    ret = OB_NOT_INIT;
  } else {
    impl_->row_arena_.reset();
    common::ObString line;
    if (OB_FAIL(impl_->reader_.next(line))) {
    } else {
      common::ObArenaAllocator tmp(common::ObModIds::OB_JSON_PARSER);
      common::ObIJsonBase *j_base = nullptr;
      common::ObJsonObject *obj = nullptr;
      if (OB_FAIL(common::ObJsonBaseFactory::get_json_base(&tmp, line,
                  common::ObJsonInType::JSON_TREE, common::ObJsonInType::JSON_TREE, j_base))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid json line");
        LOG_WARN("failed to parse jsonl line", K(ret), "line", impl_->reader_.line_no());
      } else if (OB_ISNULL(j_base) || j_base->json_type() != common::ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "json line is not a json object");
      } else if (OB_FALSE_IT(obj = static_cast<common::ObJsonObject *>(j_base))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < impl_->schema_->columns_.count(); ++i) {
          const ObFileColumn &col = impl_->schema_->columns_.at(i);
          common::ObJsonNode *value = obj->get_value(col.name_);
          common::ObString cell;
          common::ObString copied;
          bool is_null_cell = false;
          if (OB_ISNULL(value) || value->json_type() == common::ObJsonNodeType::J_NULL) {
            is_null_cell = true;
          } else if (value->json_type() == common::ObJsonNodeType::J_OBJECT ||
                     value->json_type() == common::ObJsonNodeType::J_ARRAY) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "nested json values are not supported in jsonl");
          } else if (value->json_type() == common::ObJsonNodeType::J_STRING) {
            cell = static_cast<common::ObJsonString *>(value)->get_str();
          } else if (OB_FAIL(json_scalar_to_text(value, tmp, cell))) {
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(copy_file_string(impl_->row_arena_, cell, copied))) {
          } else if (OB_FAIL(fields.push_back(copied))) {
          } else if (OB_FAIL(is_null.push_back(is_null_cell))) {
          }
        }
      }
    }
  }
  return ret;
}

int ob_file_sql_infer_json(common::ObIAllocator &allocator,
                           const common::ObString &path,
                           ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  ObJsonLineReader reader;
  common::ObString line;
  JsonlSchemaCtx ctx;
  schema.columns_.reuse();
  schema.column_names_.reuse();
  ctx.allocator_ = &allocator;
  ctx.schema_ = &schema;
  if (OB_FAIL(reader.init(allocator, path))) {
  }
  while (OB_SUCC(ret)) {
    const int read_ret = reader.next(line);
    if (OB_ITER_END == read_ret) {
      break;
    } else if (OB_FAIL(read_ret)) {
    } else if (OB_FAIL(jsonl_schema_line(&ctx, line.ptr(), line.length(), reader.line_no()))) {
    }
  }
  if (OB_SUCC(ret) && schema.columns_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "json file has no columns");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < schema.columns_.count(); ++i) {
    if (OB_FAIL(schema.column_names_.push_back(schema.columns_.at(i).name_))) {
    }
  }
  reader.close();
  return ret;
}

int ob_file_sql_infer_parquet(common::ObIAllocator &allocator,
                              const common::ObString &path,
                              ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  schema.columns_.reuse();
  schema.column_names_.reuse();
  // The Parquet footer schema is owned by the scanner; reuse it so inference and
  // scanning cannot disagree about the columns.
  ObFileParquetScanner scanner;
  ObFileSchema tmp_schema;
  if (OB_FAIL(scanner.init(allocator, path, tmp_schema))) {
  } else if (OB_FAIL(schema.columns_.assign(tmp_schema.columns_))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < schema.columns_.count(); ++i) {
      if (OB_FAIL(schema.column_names_.push_back(schema.columns_.at(i).name_))) {
      }
    }
  }
  scanner.release();
  return ret;
}

// ---------------------------------------------------------------------------
// Parquet
//
// Compact thrift reader for the subset the fixtures use: footer schema, PLAIN
// and RLE_DICTIONARY page encodings, UNCOMPRESSED/SNAPPY codecs, DataPageV1/V2
// and scalar logical types. Rows are decoded one row group at a time, so
// scanning stays streaming.
// ---------------------------------------------------------------------------

namespace
{

struct ParquetCursor
{
  ParquetCursor() : p_(nullptr), end_(nullptr) {}
  ParquetCursor(const char *p, const char *end) : p_(p), end_(end) {}

  int64_t remaining() const { return end_ - p_; }

  int read_byte(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(p_ >= end_)) {
      ret = OB_INVALID_DATA;
    } else {
      v = static_cast<uint8_t>(*p_++);
    }
    return ret;
  }

  int read_uvarint(int64_t &v)
  {
    int ret = OB_SUCCESS;
    uint64_t raw = 0;
    uint8_t b = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done && shift < 70) {
      if (OB_FAIL(read_byte(b))) {
      } else {
        raw |= (static_cast<uint64_t>(b & 0x7F) << shift);
        shift += 7;
        done = (b & 0x80) == 0;
      }
    }
    if (OB_SUCC(ret)) {
      v = static_cast<int64_t>(raw);
    }
    return ret;
  }

  int read_zigzag(int64_t &v)
  {
    int ret = OB_SUCCESS;
    int64_t raw = 0;
    if (OB_FAIL(read_uvarint(raw))) {
    } else {
      v = (raw >> 1) ^ -(raw & 1);
    }
    return ret;
  }

  int read_fixed32(uint32_t &v)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(p_ + 4 > end_)) {
      ret = OB_INVALID_DATA;
    } else {
      v = static_cast<uint32_t>(static_cast<uint8_t>(p_[0])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(p_[1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(p_[2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(p_[3])) << 24);
      p_ += 4;
    }
    return ret;
  }

  // Binary fields carry an unsigned varint length, not a zigzag one.
  int read_bytes(common::ObString &v, common::ObIAllocator &allocator)
  {
    int ret = OB_SUCCESS;
    int64_t len = 0;
    if (OB_FAIL(read_uvarint(len))) {
    } else if (OB_UNLIKELY(len < 0 || p_ + len > end_)) {
      ret = OB_INVALID_DATA;
    } else {
      char *buf = static_cast<char *>(allocator.alloc(len + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (len > 0) {
          MEMCPY(buf, p_, len);
        }
        buf[len] = '\0';
        v.assign_ptr(buf, static_cast<int32_t>(len));
        p_ += len;
      }
    }
    return ret;
  }

  const char *p_;
  const char *end_;
};

enum ParquetPhysicalType
{
  PQ_BOOLEAN = 0, PQ_INT32 = 1, PQ_INT64 = 2, PQ_INT96 = 3, PQ_FLOAT = 4,
  PQ_DOUBLE = 5, PQ_BYTE_ARRAY = 6, PQ_FIXED_LEN_BYTE_ARRAY = 7
};

enum ParquetCodec
{
  PQ_UNCOMPRESSED = 0, PQ_SNAPPY = 1, PQ_GZIP = 2, PQ_ZSTD = 6
};

enum ParquetRepetition
{
  PQ_REQUIRED = 0, PQ_OPTIONAL = 1, PQ_REPEATED = 2
};

// Compact protocol type ids (parquet uses thrift compact encoding). The two
// boolean types are their own field types and carry no payload.
const int16_t THRIFT_STOP = 0;
const int16_t THRIFT_TRUE = 1;
const int16_t THRIFT_FALSE = 2;
const int16_t THRIFT_BYTE = 3;
const int16_t THRIFT_I16 = 4;
const int16_t THRIFT_I32 = 5;
const int16_t THRIFT_I64 = 6;
const int16_t THRIFT_DOUBLE = 7;
const int16_t THRIFT_BINARY = 8;
const int16_t THRIFT_LIST = 9;
const int16_t THRIFT_SET = 10;
const int16_t THRIFT_STRUCT = 12;

// Reads one field header. `last_id` is the id of the previous field of the same
// struct and is advanced to this field's id, which compact encoding encodes as a
// delta.
int thrift_read_field(ParquetCursor &c, int16_t &last_id, int16_t &type, int16_t &id)
{
  int ret = OB_SUCCESS;
  uint8_t b = 0;
  if (OB_FAIL(c.read_byte(b))) {
  } else if (THRIFT_STOP == b) {
    type = THRIFT_STOP;
    id = 0;
  } else {
    type = b & 0x0F;
    const int16_t delta = (b & 0xF0) >> 4;
    if (0 == delta) {
      int64_t zig = 0;
      if (OB_FAIL(c.read_zigzag(zig))) {
      } else {
        id = static_cast<int16_t>(zig);
      }
    } else {
      id = static_cast<int16_t>(last_id + delta);
    }
    last_id = id;
  }
  return ret;
}

int thrift_skip(ParquetCursor &c, const int16_t type, common::ObIAllocator &allocator);

// Lists carry the element count in the high nibble of the header byte; a count
// of 15 means the real count follows as an unsigned varint.
int thrift_read_list_header(ParquetCursor &c, int16_t &elem_type, int64_t &count)
{
  int ret = OB_SUCCESS;
  uint8_t b = 0;
  if (OB_FAIL(c.read_byte(b))) {
  } else {
    count = (b & 0xF0) >> 4;
    elem_type = b & 0x0F;
    if (15 == count) {
      ret = c.read_uvarint(count);
    }
  }
  return ret;
}

int thrift_skip(ParquetCursor &c, const int16_t type, common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case THRIFT_FALSE:
    case THRIFT_TRUE: {
      // The value rides in the type nibble; no payload follows.
      break;
    }
    case THRIFT_BYTE: { uint8_t v = 0; ret = c.read_byte(v); break; }
    case THRIFT_I16:
    case THRIFT_I32:
    case THRIFT_I64: { int64_t v = 0; ret = c.read_zigzag(v); break; }
    case THRIFT_DOUBLE: {
      if (OB_UNLIKELY(c.remaining() < 8)) {
        ret = OB_INVALID_DATA;
      } else {
        c = ParquetCursor(c.p_ + 8, c.end_);
      }
      break;
    }
    case THRIFT_BINARY: { common::ObString s; ret = c.read_bytes(s, allocator); break; }
    case THRIFT_LIST:
    case THRIFT_SET: {
      int16_t elem_type = 0;
      int64_t count = 0;
      if (OB_FAIL(thrift_read_list_header(c, elem_type, count))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        ret = thrift_skip(c, elem_type, allocator);
      }
      break;
    }
    case THRIFT_STRUCT: {
      bool done = false;
      int16_t last_id = 0;
      while (OB_SUCC(ret) && !done) {
        int16_t ftype = 0;
        int16_t fid = 0;
        if (OB_FAIL(thrift_read_field(c, last_id, ftype, fid))) {
        } else if (THRIFT_STOP == ftype) {
          done = true;
        } else {
          ret = thrift_skip(c, ftype, allocator);
        }
      }
      break;
    }
    default:
      ret = OB_INVALID_DATA;
      break;
  }
  return ret;
}

// One leaf column of the footer schema.
struct ParquetSchemaLeaf
{
  ParquetSchemaLeaf()
    : physical_(PQ_BYTE_ARRAY), converted_(-1), repetition_(PQ_REQUIRED), type_length_(0) {}
  common::ObString name_;
  int32_t physical_;
  int32_t converted_;
  int32_t repetition_;
  int32_t type_length_;
};

// Maps a Parquet physical/logical type pair onto an inferred file type.
ObFileColumnType parquet_logical_type(const int32_t physical, const int32_t converted)
{
  ObFileColumnType type = OB_FILE_TYPE_VARCHAR;
  switch (physical) {
    case PQ_BOOLEAN: type = OB_FILE_TYPE_BOOLEAN; break;
    case PQ_INT32: {
      // DATE and TIMESTAMP millis are stored as INT32 with a converted type.
      if (6 == converted) {
        type = OB_FILE_TYPE_DATE;
      } else {
        type = OB_FILE_TYPE_BIGINT;
      }
      break;
    }
    case PQ_INT64: type = (9 == converted) ? OB_FILE_TYPE_DATETIME : OB_FILE_TYPE_BIGINT; break;
    case PQ_FLOAT:
    case PQ_DOUBLE: type = OB_FILE_TYPE_DOUBLE; break;
    default: type = OB_FILE_TYPE_VARCHAR; break;
  }
  return type;
}

// One column chunk of one row group.
struct ParquetColumnMeta
{
  ParquetColumnMeta()
    : physical_(PQ_BYTE_ARRAY), converted_(-1), codec_(PQ_UNCOMPRESSED),
      num_values_(0), total_compressed_(0), total_uncompressed_(0),
      data_page_offset_(0), dictionary_page_offset_(-1), repetition_(PQ_REQUIRED),
      type_length_(0), inferred_(OB_FILE_TYPE_VARCHAR) {}
  common::ObString name_;
  int32_t physical_;
  int32_t converted_;
  int32_t codec_;
  int64_t num_values_;
  int64_t total_compressed_;
  int64_t total_uncompressed_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  int32_t repetition_;
  int32_t type_length_;
  ObFileColumnType inferred_;
};

// Decoded cells of one column chunk.
struct ParquetChunkData
{
  ParquetChunkData() : values_(), is_null_() {}
  common::ObSEArray<common::ObString, 64> values_;
  common::ObSEArray<bool, 64> is_null_;
};

// Raw snappy block decoder: a varint length followed by literal/copy tags.
int snappy_uncompress(const char *in, const int64_t in_len,
                      const int64_t expected_len,
                      char *out, const int64_t out_cap)
{
  int ret = OB_SUCCESS;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(in);
  const uint8_t *end = p + in_len;
  int64_t written = 0;
  uint64_t declared = 0;
  int shift = 0;
  bool len_done = false;
  while (OB_SUCC(ret) && p < end && !len_done && shift < 35) {
    declared |= (static_cast<uint64_t>(*p & 0x7F) << shift);
    shift += 7;
    len_done = (*p & 0x80) == 0;
    ++p;
  }
  if (OB_UNLIKELY(!len_done || declared != static_cast<uint64_t>(expected_len) ||
                  expected_len > out_cap)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("snappy length header mismatch", K(ret), K(declared), K(expected_len), K(out_cap));
  }
  while (OB_SUCC(ret) && p < end && written < expected_len) {
    const uint8_t tag = *p++;
    const uint8_t type = tag & 0x03;
    if (0 == type) {
      int64_t len = (tag >> 2) + 1;
      if ((tag >> 2) >= 60) {
        const int extra = (tag >> 2) - 59;
        len = 0;
        for (int i = 0; i < extra && p < end; ++i) {
          len |= (static_cast<int64_t>(*p++) << (8 * i));
        }
        ++len;
      }
      if (OB_UNLIKELY(p + len > end || written + len > expected_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("snappy literal overruns", K(ret), K(len));
      } else {
        MEMCPY(out + written, p, len);
        written += len;
        p += len;
      }
    } else {
      int64_t len = 0;
      int64_t offset = 0;
      if (1 == type) {
        if (OB_UNLIKELY(p >= end)) {
          ret = OB_INVALID_DATA;
        } else {
          len = 4 + ((tag >> 2) & 0x07);
          offset = ((static_cast<int64_t>(tag & 0xE0) << 3) | *p++);
        }
      } else if (2 == type) {
        if (OB_UNLIKELY(p + 1 >= end)) {
          ret = OB_INVALID_DATA;
        } else {
          len = 1 + (tag >> 2);
          offset = static_cast<uint8_t>(p[0]) | (static_cast<int64_t>(p[1]) << 8);
          p += 2;
        }
      } else {
        if (OB_UNLIKELY(p + 3 >= end)) {
          ret = OB_INVALID_DATA;
        } else {
          len = 1 + (tag >> 2);
          offset = static_cast<uint8_t>(p[0]) |
                   (static_cast<int64_t>(static_cast<uint8_t>(p[1])) << 8) |
                   (static_cast<int64_t>(static_cast<uint8_t>(p[2])) << 16) |
                   (static_cast<int64_t>(static_cast<uint8_t>(p[3])) << 24);
          p += 4;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_UNLIKELY(offset <= 0 || offset > written || written + len > expected_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("snappy copy invalid", K(ret), K(offset), K(len), K(written));
      } else {
        for (int64_t i = 0; i < len; ++i) {
          out[written + i] = out[written - offset + i];
        }
        written += len;
      }
    }
  }
  if (OB_SUCC(ret) && written != expected_len) {
    ret = OB_INVALID_DATA;
    LOG_WARN("snappy decoded size mismatch", K(ret), K(written), K(expected_len));
  }
  return ret;
}

int parquet_parse_schema_element(ParquetCursor &c, common::ObIAllocator &allocator,
                                 ParquetSchemaLeaf &leaf, int32_t &num_children)
{
  int ret = OB_SUCCESS;
  num_children = 0;
  bool done = false;
  int16_t last_id = 0;
  while (OB_SUCC(ret) && !done) {
    int16_t ftype = 0;
    int16_t fid = 0;
    if (OB_FAIL(thrift_read_field(c, last_id, ftype, fid))) {
    } else if (THRIFT_STOP == ftype) {
      done = true;
    } else if (1 == fid && THRIFT_I32 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        leaf.physical_ = static_cast<int32_t>(v);
      }
    } else if (2 == fid && THRIFT_I32 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        leaf.type_length_ = static_cast<int32_t>(v);
      }
    } else if (3 == fid && THRIFT_I32 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        leaf.repetition_ = static_cast<int32_t>(v);
      }
    } else if (4 == fid && THRIFT_BINARY == ftype) {
      if (OB_FAIL(c.read_bytes(leaf.name_, allocator))) {
      }
    } else if (5 == fid && THRIFT_I32 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        num_children = static_cast<int32_t>(v);
      }
    } else if (6 == fid && THRIFT_I32 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        leaf.converted_ = static_cast<int32_t>(v);
      }
    } else if (OB_FAIL(thrift_skip(c, ftype, allocator))) {
    }
  }
  return ret;
}

// PageHeader of either data page version.
struct ParquetPageHeader
{
  ParquetPageHeader()
    : type_(-1), uncompressed_(0), compressed_(0),
      num_values_(0), encoding_(-1), num_nulls_(0),
      def_len_(0), rep_len_(0), is_compressed_(true), dict_num_values_(0) {}
  int32_t type_;       // 0 = DATA_PAGE, 2 = DICTIONARY_PAGE, 3 = DATA_PAGE_V2
  int64_t uncompressed_;
  int64_t compressed_;
  // V1
  int64_t num_values_;
  int32_t encoding_;
  // V2
  int64_t num_nulls_;
  int64_t def_len_;
  int64_t rep_len_;
  bool is_compressed_;
  // dictionary page
  int64_t dict_num_values_;
};

int parquet_read_page_header(ParquetCursor &c, common::ObIAllocator &allocator,
                             ParquetPageHeader &info)
{
  int ret = OB_SUCCESS;
  bool done = false;
  int16_t last_id = 0;
  while (OB_SUCC(ret) && !done) {
    int16_t ftype = 0;
    int16_t fid = 0;
    if (OB_FAIL(thrift_read_field(c, last_id, ftype, fid))) {
    } else if (THRIFT_STOP == ftype) {
      done = true;
    } else if (1 == fid && THRIFT_I32 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        info.type_ = static_cast<int32_t>(v);
      }
    } else if (2 == fid && THRIFT_I32 == ftype) {
      if (OB_FAIL(c.read_zigzag(info.uncompressed_))) {
      }
    } else if (3 == fid && THRIFT_I32 == ftype) {
      if (OB_FAIL(c.read_zigzag(info.compressed_))) {
      }
    } else if (5 == fid && THRIFT_STRUCT == ftype) {
      // DataPageHeader (V1): 1 num_values, 2 encoding, 3 def enc, 4 rep enc.
      bool inner_done = false;
      int16_t inner_last = 0;
      while (OB_SUCC(ret) && !inner_done) {
        int16_t itype = 0;
        int16_t iid = 0;
        if (OB_FAIL(thrift_read_field(c, inner_last, itype, iid))) {
        } else if (THRIFT_STOP == itype) {
          inner_done = true;
        } else if (1 == iid && THRIFT_I32 == itype) {
          if (OB_FAIL(c.read_zigzag(info.num_values_))) {
          }
        } else if (2 == iid && THRIFT_I32 == itype) {
          int64_t v = 0;
          if (OB_FAIL(c.read_zigzag(v))) {
          } else {
            info.encoding_ = static_cast<int32_t>(v);
          }
        } else if (OB_FAIL(thrift_skip(c, itype, allocator))) {
        }
      }
    } else if (7 == fid && THRIFT_STRUCT == ftype) {
      // DictionaryPageHeader: 1 num_values, 2 encoding.
      bool inner_done = false;
      int16_t inner_last = 0;
      while (OB_SUCC(ret) && !inner_done) {
        int16_t itype = 0;
        int16_t iid = 0;
        if (OB_FAIL(thrift_read_field(c, inner_last, itype, iid))) {
        } else if (THRIFT_STOP == itype) {
          inner_done = true;
        } else if (1 == iid && THRIFT_I32 == itype) {
          if (OB_FAIL(c.read_zigzag(info.dict_num_values_))) {
          }
        } else if (OB_FAIL(thrift_skip(c, itype, allocator))) {
        }
      }
    } else if (8 == fid && THRIFT_STRUCT == ftype) {
      // DataPageHeaderV2: 1 num_values, 2 num_nulls, 3 num_rows, 4 encoding,
      // 5 def levels length, 6 rep levels length, 7 is_compressed.
      bool inner_done = false;
      int16_t inner_last = 0;
      while (OB_SUCC(ret) && !inner_done) {
        int16_t itype = 0;
        int16_t iid = 0;
        if (OB_FAIL(thrift_read_field(c, inner_last, itype, iid))) {
        } else if (THRIFT_STOP == itype) {
          inner_done = true;
        } else if (1 == iid && THRIFT_I32 == itype) {
          if (OB_FAIL(c.read_zigzag(info.num_values_))) {
          }
        } else if (2 == iid && THRIFT_I32 == itype) {
          if (OB_FAIL(c.read_zigzag(info.num_nulls_))) {
          }
        } else if (4 == iid && THRIFT_I32 == itype) {
          int64_t v = 0;
          if (OB_FAIL(c.read_zigzag(v))) {
          } else {
            info.encoding_ = static_cast<int32_t>(v);
          }
        } else if (5 == iid && THRIFT_I32 == itype) {
          if (OB_FAIL(c.read_zigzag(info.def_len_))) {
          }
        } else if (6 == iid && THRIFT_I32 == itype) {
          if (OB_FAIL(c.read_zigzag(info.rep_len_))) {
          }
        } else if (7 == iid && (THRIFT_TRUE == itype || THRIFT_FALSE == itype)) {
          info.is_compressed_ = (THRIFT_TRUE == itype);
        } else if (OB_FAIL(thrift_skip(c, itype, allocator))) {
        }
      }
    } else if (OB_FAIL(thrift_skip(c, ftype, allocator))) {
    }
  }
  return ret;
}

int64_t parquet_fixed_width(const int32_t physical, const int32_t type_length)
{
  switch (physical) {
    case PQ_BOOLEAN: return 0;  // PLAIN booleans are bit-packed, not byte-aligned.
    case PQ_INT32:
    case PQ_FLOAT: return 4;
    case PQ_INT64:
    case PQ_DOUBLE: return 8;
    case PQ_INT96: return 12;
    case PQ_FIXED_LEN_BYTE_ARRAY: return type_length;
    default: return 0;  // BYTE_ARRAY is variable width
  }
}

// Renders one PLAIN value as text; the materialized table keeps every cell as
// text so the type conversion in the resolver stays in one place.
int parquet_plain_value(const char *&cursor, const char *end,
                        const int32_t physical, const int32_t type_length,
                        common::ObIAllocator &allocator,
                        common::ObString &cell)
{
  int ret = OB_SUCCESS;
  char buf[64];
  int64_t len = 0;
  switch (physical) {
    case PQ_BOOLEAN: {
      if (OB_UNLIKELY(cursor >= end)) {
        ret = OB_INVALID_DATA;
      } else {
        len = snprintf(buf, sizeof(buf), "%s", *cursor ? "true" : "false");
        ++cursor;
      }
      break;
    }
    case PQ_INT32: {
      uint32_t raw = 0;
      if (OB_UNLIKELY(cursor + 4 > end)) {
        ret = OB_INVALID_DATA;
      } else {
        int32_t v = static_cast<int32_t>(
            static_cast<uint32_t>(static_cast<uint8_t>(cursor[0])) |
            (static_cast<uint32_t>(static_cast<uint8_t>(cursor[1])) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(cursor[2])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(cursor[3])) << 24));
        cursor += 4;
        len = snprintf(buf, sizeof(buf), "%d", v);
      }
      break;
    }
    case PQ_INT64: {
      if (OB_UNLIKELY(cursor + 8 > end)) {
        ret = OB_INVALID_DATA;
      } else {
        int64_t v = 0;
        for (int i = 7; i >= 0; --i) {
          v = (v << 8) | static_cast<uint8_t>(cursor[i]);
        }
        cursor += 8;
        len = snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
      }
      break;
    }
    case PQ_FLOAT:
    case PQ_DOUBLE: {
      const int64_t width = PQ_FLOAT == physical ? 4 : 8;
      if (OB_UNLIKELY(cursor + width > end)) {
        ret = OB_INVALID_DATA;
      } else {
        double v = 0;
        if (4 == width) {
          uint32_t bits = 0;
          for (int i = 3; i >= 0; --i) {
            bits = (bits << 8) | static_cast<uint8_t>(cursor[i]);
          }
          float f = 0;
          MEMCPY(&f, &bits, sizeof(f));
          v = f;
        } else {
          MEMCPY(&v, cursor, sizeof(v));
        }
        cursor += width;
        len = snprintf(buf, sizeof(buf), "%g", v);
      }
      break;
    }
    case PQ_BYTE_ARRAY: {
      uint32_t raw_len = 0;
      if (OB_UNLIKELY(cursor + 4 > end)) {
        ret = OB_INVALID_DATA;
      } else {
        raw_len = static_cast<uint32_t>(static_cast<uint8_t>(cursor[0])) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(cursor[1])) << 8) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(cursor[2])) << 16) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(cursor[3])) << 24);
        cursor += 4;
        if (OB_UNLIKELY(cursor + raw_len > end)) {
          ret = OB_INVALID_DATA;
        } else {
          char *copy = static_cast<char *>(allocator.alloc(raw_len));
          if (OB_ISNULL(copy) && raw_len > 0) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            if (raw_len > 0) {
              MEMCPY(copy, cursor, raw_len);
            }
            cell.assign_ptr(copy, static_cast<int32_t>(raw_len));
            cursor += raw_len;
          }
        }
      }
      break;
    }
    case PQ_FIXED_LEN_BYTE_ARRAY: {
      if (OB_UNLIKELY(cursor + type_length > end)) {
        ret = OB_INVALID_DATA;
      } else {
        char *copy = static_cast<char *>(allocator.alloc(type_length));
        if (OB_ISNULL(copy) && type_length > 0) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          if (type_length > 0) {
            MEMCPY(copy, cursor, type_length);
          }
          cell.assign_ptr(copy, type_length);
          cursor += type_length;
        }
      }
      break;
    }
    default: {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet physical type");
      break;
    }
  }
  if (OB_SUCC(ret) && cell.empty() && len > 0) {
    char *copy = static_cast<char *>(allocator.alloc(len));
    if (OB_ISNULL(copy)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(copy, buf, len);
      cell.assign_ptr(copy, static_cast<int32_t>(len));
    }
  }
  return ret;
}

// Decodes the RLE / bit-packed hybrid used by definition levels and dictionary
// indices. `bit_width` is the width of one value in bits.
int parquet_rle_decode(ParquetCursor &c, const int32_t bit_width, const int64_t count,
                       common::ObIArray<int64_t> &out)
{
  int ret = OB_SUCCESS;
  const int32_t value_bytes = (bit_width + 7) / 8;
  int64_t decoded = 0;
  while (OB_SUCC(ret) && decoded < count) {
    int64_t header = 0;
    if (OB_FAIL(c.read_uvarint(header))) {
    } else if (header & 1) {
      // bit-packed run: (header >> 1) groups of 8 values
      const int64_t groups = header >> 1;
      const int64_t run = groups * 8;
      const int64_t byte_cnt = groups * bit_width;
      if (OB_UNLIKELY(c.remaining() < byte_cnt)) {
        ret = OB_INVALID_DATA;
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < run && decoded < count; ++i) {
          uint64_t v = 0;
          for (int32_t b = 0; b < bit_width; ++b) {
            const int64_t bit_pos = i * bit_width + b;
            if (0 != (static_cast<uint8_t>(c.p_[bit_pos / 8]) & (1U << (bit_pos % 8)))) {
              v |= (static_cast<uint64_t>(1) << b);
            }
          }
          if (OB_FAIL(out.push_back(static_cast<int64_t>(v)))) {
          } else {
            ++decoded;
          }
        }
        c = ParquetCursor(c.p_ + byte_cnt, c.end_);
      }
    } else {
      // RLE run: one value repeated
      const int64_t run = header >> 1;
      int64_t v = 0;
      for (int32_t i = 0; OB_SUCC(ret) && i < value_bytes; ++i) {
        uint8_t b = 0;
        if (OB_FAIL(c.read_byte(b))) {
        } else {
          v |= static_cast<int64_t>(b) << (8 * i);
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < run && decoded < count; ++i) {
        if (OB_FAIL(out.push_back(v))) {
        } else {
          ++decoded;
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

class ObRandomAccessFile
{
public:
  ObRandomAccessFile() : fd_(-1), size_(0) {}
  ~ObRandomAccessFile() { close(); }

  int init(const common::ObString &path)
  {
    int ret = OB_SUCCESS;
    close();
    struct stat st;
    fd_ = open(path.ptr(), O_RDONLY);
    if (fd_ < 0) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open parquet file", K(ret), K(path), K(errno));
    } else if (0 != fstat(fd_, &st)) {
      ret = OB_IO_ERROR;
    } else if (!S_ISREG(st.st_mode)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "path is not a regular file");
    } else {
      size_ = st.st_size;
    }
    if (OB_FAIL(ret)) {
      close();
    }
    return ret;
  }

  int read_at(const int64_t offset,
              const int64_t len,
              common::ObIAllocator &allocator,
              common::ObString &out)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(offset < 0 || len < 0 || offset > size_ || len > size_ - offset ||
                    len > INT32_MAX - 1)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("invalid file read range", K(ret), K(offset), K(len), K_(size));
    } else {
      char *buf = static_cast<char *>(allocator.alloc(len + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t done = 0;
        while (OB_SUCC(ret) && done < len) {
          const ssize_t n = pread(fd_, buf + done, len - done, offset + done);
          if (n > 0) {
            done += n;
          } else if (0 == n) {
            ret = OB_INVALID_DATA;
          } else if (EINTR != errno) {
            ret = OB_IO_ERROR;
          }
        }
        if (OB_SUCC(ret)) {
          buf[len] = '\0';
          out.assign_ptr(buf, static_cast<int32_t>(len));
        }
      }
    }
    return ret;
  }

  int64_t size() const { return size_; }

  void close()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    size_ = 0;
  }

private:
  int fd_;
  int64_t size_;
};

struct ObFileParquetScanner::Impl
{
  Impl()
    : allocator_(nullptr), file_(), schema_(nullptr), row_groups_(), num_row_groups_(0),
      row_group_(-1), row_index_(0), rows_in_group_(0), chunk_data_(),
      projection_(), decode_arena_(common::ObModIds::OB_SQL_EXECUTOR) {}
  common::ObIAllocator *allocator_;
  ObRandomAccessFile file_;
  const ObFileSchema *schema_;
  common::ObSEArray<common::ObSEArray<ParquetColumnMeta, 16>, 8> row_groups_;
  int64_t num_row_groups_;
  int64_t row_group_;
  int64_t row_index_;
  int64_t rows_in_group_;
  common::ObSEArray<ParquetChunkData, 16> chunk_data_;
  common::ObSEArray<int8_t, 16> projection_;
  common::ObArenaAllocator decode_arena_;

  int load_row_group();
  int decode_chunk(const ParquetColumnMeta &meta, ParquetChunkData &data);
};

ObFileParquetScanner::ObFileParquetScanner() : impl_(nullptr) {}
ObFileParquetScanner::~ObFileParquetScanner()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->~Impl();
    impl_ = nullptr;
  }
}

void ObFileParquetScanner::release()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->file_.close();
    impl_->decode_arena_.reset();
    impl_->chunk_data_.reset();
    impl_->projection_.reset();
  }
}

namespace
{

// Parses the footer: schema leaves plus every row group's column chunks.
int parquet_parse_footer(common::ObIAllocator &allocator,
                         const common::ObString &content,
                         common::ObIArray<ParquetSchemaLeaf> &leaves,
                         common::ObSEArray<common::ObSEArray<ParquetColumnMeta, 16>, 8> &row_groups,
                         int64_t &num_row_groups)
{
  int ret = OB_SUCCESS;
  num_row_groups = 0;
  const int64_t size = content.length();
  if (OB_UNLIKELY(size < 12 || 0 != strncmp(content.ptr(), "PAR1", 4) ||
                 0 != strncmp(content.ptr() + size - 4, "PAR1", 4))) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file is not a valid parquet file");
    LOG_WARN("bad parquet magic", K(ret), K(size));
  } else {
    uint32_t footer_len = 0;
    const char *len_ptr = content.ptr() + size - 8;
    footer_len = static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[0])) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[1])) << 8) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[2])) << 16) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[3])) << 24);
    if (OB_UNLIKELY(static_cast<int64_t>(footer_len) > size - 12)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("parquet footer length is out of range", K(ret), K(footer_len), K(size));
    } else {
      ParquetCursor c(content.ptr() + size - 8 - footer_len, content.ptr() + size - 8);
      bool done = false;
      int16_t last_id = 0;
      while (OB_SUCC(ret) && !done) {
        int16_t ftype = 0;
        int16_t fid = 0;
        if (OB_FAIL(thrift_read_field(c, last_id, ftype, fid))) {
        } else if (THRIFT_STOP == ftype) {
          done = true;
        } else if (2 == fid && THRIFT_LIST == ftype) {
          // schema: list of SchemaElement
          int16_t elem_type = 0;
          int64_t count = 0;
          if (OB_FAIL(thrift_read_list_header(c, elem_type, count))) {
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
            ParquetSchemaLeaf leaf;
            int32_t num_children = 0;
            if (OB_FAIL(parquet_parse_schema_element(c, allocator, leaf, num_children))) {
            } else if (0 == num_children) {
              // only leaves carry data
              if (OB_FAIL(leaves.push_back(leaf))) {
              }
            }
          }
        } else if (4 == fid && THRIFT_LIST == ftype) {
          // row_groups: list of RowGroup
          int16_t elem_type = 0;
          int64_t count = 0;
          if (OB_FAIL(thrift_read_list_header(c, elem_type, count))) {
          }
          num_row_groups = count;
          for (int64_t g = 0; OB_SUCC(ret) && g < count; ++g) {
            common::ObSEArray<ParquetColumnMeta, 16> chunk_meta;
            bool rg_done = false;
            int16_t rg_last = 0;
            while (OB_SUCC(ret) && !rg_done) {
              int16_t rtype = 0;
              int16_t rid = 0;
              if (OB_FAIL(thrift_read_field(c, rg_last, rtype, rid))) {
              } else if (THRIFT_STOP == rtype) {
                rg_done = true;
              } else if (1 == rid && THRIFT_LIST == rtype) {
                // columns: list of ColumnChunk
                int16_t celem = 0;
                int64_t ccount = 0;
                if (OB_FAIL(thrift_read_list_header(c, celem, ccount))) {
                }
                for (int64_t k = 0; OB_SUCC(ret) && k < ccount; ++k) {
                  ParquetColumnMeta meta;
                  bool chunk_done = false;
                  int16_t chunk_last = 0;
                  while (OB_SUCC(ret) && !chunk_done) {
                    int16_t ctype = 0;
                    int16_t cid = 0;
                    if (OB_FAIL(thrift_read_field(c, chunk_last, ctype, cid))) {
                    } else if (THRIFT_STOP == ctype) {
                      chunk_done = true;
                    } else if (3 == cid && THRIFT_STRUCT == ctype) {
                      // ColumnMetaData
                      bool md_done = false;
                      int16_t md_last = 0;
                      while (OB_SUCC(ret) && !md_done) {
                        int16_t mtype = 0;
                        int16_t mid = 0;
                        if (OB_FAIL(thrift_read_field(c, md_last, mtype, mid))) {
                        } else if (THRIFT_STOP == mtype) {
                          md_done = true;
                        } else if (1 == mid && THRIFT_I32 == mtype) {
                          int64_t v_physical = 0;
                          if (OB_FAIL(c.read_zigzag(v_physical))) {
                          } else {
                            meta.physical_ = static_cast<int32_t>(v_physical);
                          }
                        } else if (3 == mid && THRIFT_LIST == mtype) {
                          int16_t pelem = 0;
                          int64_t pcount = 0;
                          if (OB_FAIL(thrift_read_list_header(c, pelem, pcount))) {
                          }
                          for (int64_t e = 0; OB_SUCC(ret) && e < pcount; ++e) {
                            common::ObString part;
                            if (OB_FAIL(c.read_bytes(part, allocator))) {
                            } else if (e == pcount - 1) {
                              meta.name_ = part;
                            }
                          }
                        } else if (4 == mid && THRIFT_I32 == mtype) {
                          int64_t v_codec = 0;
                          if (OB_FAIL(c.read_zigzag(v_codec))) {
                          } else {
                            meta.codec_ = static_cast<int32_t>(v_codec);
                          }
                        } else if (5 == mid && THRIFT_I64 == mtype) {
                          // num_values is an i64 in the format, so it arrives as
                          // the compact i64 type rather than i32.
                          if (OB_FAIL(c.read_zigzag(meta.num_values_))) {
                          }
                        } else if (6 == mid && THRIFT_I64 == mtype) {
                          if (OB_FAIL(c.read_zigzag(meta.total_uncompressed_))) {
                          }
                        } else if (7 == mid && THRIFT_I64 == mtype) {
                          if (OB_FAIL(c.read_zigzag(meta.total_compressed_))) {
                          }
                        } else if ((5 == mid || 6 == mid || 7 == mid) && THRIFT_I32 == mtype) {
                          // Tolerate writers that narrow these fields to i32.
                          int64_t v = 0;
                          if (OB_FAIL(c.read_zigzag(v))) {
                          } else if (5 == mid) {
                            meta.num_values_ = v;
                          } else if (6 == mid) {
                            meta.total_uncompressed_ = v;
                          } else {
                            meta.total_compressed_ = v;
                          }
                        } else if (9 == mid && THRIFT_I64 == mtype) {
                          if (OB_FAIL(c.read_zigzag(meta.data_page_offset_))) { // int64 member
                          }
                        } else if (11 == mid && THRIFT_I64 == mtype) {
                          if (OB_FAIL(c.read_zigzag(meta.dictionary_page_offset_))) { // int64 member
                          }
                        } else if (OB_FAIL(thrift_skip(c, mtype, allocator))) {
                        }
                      }
                    } else if (OB_FAIL(thrift_skip(c, ctype, allocator))) {
                    }
                  }
                  if (OB_SUCC(ret)) {
                    // carry the schema's repetition/type info onto the chunk
                    for (int64_t n = 0; OB_SUCC(ret) && n < leaves.count(); ++n) {
                      if (0 == leaves.at(n).name_.case_compare(meta.name_)) {
                        meta.converted_ = leaves.at(n).converted_;
                        meta.repetition_ = leaves.at(n).repetition_;
                        meta.type_length_ = leaves.at(n).type_length_;
                        if (PQ_BYTE_ARRAY == meta.physical_ && -1 != leaves.at(n).physical_) {
                          meta.physical_ = leaves.at(n).physical_;
                        }
                        meta.inferred_ = parquet_logical_type(meta.physical_, meta.converted_);
                      }
                    }
                    if (OB_FAIL(chunk_meta.push_back(meta))) {
                    }
                  }
                }
              } else if (OB_FAIL(thrift_skip(c, rtype, allocator))) {
              }
            }
            if (OB_SUCC(ret) && OB_FAIL(row_groups.push_back(chunk_meta))) {
            }
          }
        } else if (OB_FAIL(thrift_skip(c, ftype, allocator))) {
        }
      }
      if (OB_SUCC(ret) && leaves.empty()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet footer has no leaf columns", K(ret));
      }
    }
  }
  return ret;
}

} // anonymous namespace

int ObFileParquetScanner::Impl::decode_chunk(const ParquetColumnMeta &meta,
                                             ParquetChunkData &data)
{
  int ret = OB_SUCCESS;
  int64_t chunk_start = meta.data_page_offset_;
  if (meta.dictionary_page_offset_ > 0 && meta.dictionary_page_offset_ < chunk_start) {
    chunk_start = meta.dictionary_page_offset_;
  }
  common::ObString chunk;
  if (OB_UNLIKELY(meta.total_compressed_ <= 0)) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(file_.read_at(chunk_start, meta.total_compressed_, decode_arena_, chunk))) {
  }
  const char *base = chunk.ptr();
  const int64_t size = chunk.length();
  int64_t page_off = 0;
  if (OB_UNLIKELY(page_off < 0 || page_off >= size)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("parquet page offset out of range", K(ret), K(page_off), K(size));
  }
  common::ObSEArray<common::ObString, 64> dictionary;
  bool have_values = (0 == meta.num_values_);
  while (OB_SUCC(ret) && !have_values) {
    ParquetCursor hc(base + page_off, base + size);
    ParquetPageHeader info;
    if (OB_FAIL(parquet_read_page_header(hc, decode_arena_, info))) {
    } else {
      const char *region = hc.p_;
      const int64_t region_len = info.compressed_;
      if (OB_UNLIKELY(region + region_len > base + size)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet page region overruns the file", K(ret), K(region_len));
      } else if (2 == info.type_) {
        // Dictionary page: PLAIN values, optionally snappy compressed.
        ParquetCursor dc(region, region + region_len);
        if (info.is_compressed_ && PQ_SNAPPY == meta.codec_) {
          char *out = static_cast<char *>(decode_arena_.alloc(info.uncompressed_));
          if (OB_ISNULL(out)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(snappy_uncompress(region, region_len,
                                               info.uncompressed_, out, info.uncompressed_))) {
          } else {
            dc = ParquetCursor(out, out + info.uncompressed_);
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < info.dict_num_values_; ++i) {
          common::ObString cell;
          if (OB_FAIL(parquet_plain_value(dc.p_, dc.end_, meta.physical_,
                                          meta.type_length_, decode_arena_, cell))) {
          } else if (OB_FAIL(dictionary.push_back(cell))) {
          }
        }
      } else if (0 == info.type_ || 3 == info.type_) {
        const bool is_v2 = 3 == info.type_;
        const int64_t page_start = data.values_.count();
        const int64_t max_def = PQ_REPEATED == meta.repetition_ ? 2
                                : (PQ_OPTIONAL == meta.repetition_ ? 1 : 0);
        // A DataPageV2 keeps the repetition and definition levels in front of
        // the compressed payload, so only the tail is handed to the codec. A
        // DataPageV1 compresses the whole page including its level data.
        const int64_t level_prefix = is_v2 ? info.rep_len_ + info.def_len_ : 0;
        common::ObString content(static_cast<int32_t>(region_len), region);
        char *scratch = nullptr;
        if (info.is_compressed_ && PQ_SNAPPY == meta.codec_) {
          const char *cpayload = region + level_prefix;
          const int64_t clen = region_len - level_prefix;
          const int64_t raw_values = info.uncompressed_ - level_prefix;
          if (OB_UNLIKELY(clen < 0 || raw_values < 0)) {
            ret = OB_INVALID_DATA;
            LOG_WARN("parquet page sizes are inconsistent", K(ret), K(region_len),
                     K(level_prefix), K(info.uncompressed_));
          } else {
            scratch = static_cast<char *>(decode_arena_.alloc(info.uncompressed_));
            if (OB_ISNULL(scratch)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              if (level_prefix > 0) {
                MEMCPY(scratch, region, level_prefix);
              }
              if (OB_FAIL(snappy_uncompress(cpayload, clen, raw_values,
                                            scratch + level_prefix, raw_values))) {
              } else {
                content.assign_ptr(scratch, static_cast<int32_t>(info.uncompressed_));
              }
            }
          }
        }
        const char *values_begin = content.ptr();
        if (OB_FAIL(ret)) {
        } else if (max_def > 0) {
          // Definition levels: V2 stores their length in the page header, V1
          // prefixes the hybrid data with a 4 byte length.
          const char *level_begin = content.ptr() + (is_v2 ? info.rep_len_ : 0);
          int64_t level_bytes = 0;
          ParquetCursor lc(level_begin, content.ptr() + content.length());
          if (is_v2) {
            level_bytes = info.def_len_;
          } else {
            uint32_t len = 0;
            if (OB_FAIL(lc.read_fixed32(len))) {
            } else {
              level_bytes = len;
            }
          }
          if (OB_SUCC(ret)) {
            const char *level_data = level_begin;
            if (!is_v2) {
              level_data = lc.p_;
            }
            values_begin = level_data + level_bytes;
            ParquetCursor dc3(level_data, level_data + level_bytes);
            common::ObSEArray<int64_t, 64> levels;
            if (OB_FAIL(parquet_rle_decode(dc3, max_def, info.num_values_, levels))) {
            } else if (OB_UNLIKELY(levels.count() != info.num_values_)) {
              ret = OB_INVALID_DATA;
              LOG_WARN("definition level count mismatch", K(ret), K(levels.count()),
                       K(info.num_values_));
            } else {
              for (int64_t i = 0; OB_SUCC(ret) && i < levels.count(); ++i) {
                const bool null_cell = levels.at(i) < max_def;
                if (OB_FAIL(data.is_null_.push_back(null_cell))) {
                }
              }
            }
          }
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < info.num_values_; ++i) {
            if (OB_FAIL(data.is_null_.push_back(false))) {
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else {
          // The value area only holds the non-null entries, so its size follows
          // the definition levels rather than the page's own null counter: a
          // V1 page header does not carry one at all.
          int64_t non_null = 0;
          for (int64_t i = page_start; OB_SUCC(ret) && i < data.is_null_.count(); ++i) {
            if (!data.is_null_.at(i)) {
              ++non_null;
            }
          }
          ParquetCursor vc(values_begin, content.ptr() + content.length());
          if (0 == info.encoding_) {
            // PLAIN. Fixed-width values are laid out contiguously from the start
            // of the value area; any trailing padding belongs to the page.
            const int64_t width = parquet_fixed_width(meta.physical_, meta.type_length_);
            const char *vp = vc.p_;
            const char *vend = vc.end_;
            if (width > 0 && non_null * width <= vend - vp) {
              vend = vp + non_null * width;
            }
            if (PQ_BOOLEAN == meta.physical_) {
              int64_t bool_value_idx = 0;
              for (int64_t i = 0; OB_SUCC(ret) && i < info.num_values_; ++i) {
                if (data.is_null_.at(page_start + i)) {
                  if (OB_FAIL(data.values_.push_back(common::ObString()))) {
                  }
                } else {
                  const int64_t non_null_idx = bool_value_idx++;
                  if (OB_UNLIKELY(non_null_idx / 8 >= vend - vp)) {
                    ret = OB_INVALID_DATA;
                  } else {
                    const bool v = 0 != (static_cast<uint8_t>(vp[non_null_idx / 8]) &
                                         (1U << (non_null_idx % 8)));
                    if (OB_FAIL(data.values_.push_back(common::ObString::make_string(v ? "true" : "false")))) {
                    }
                  }
                }
              }
              vp += (non_null + 7) / 8;
            } else {
              for (int64_t i = 0; OB_SUCC(ret) && i < info.num_values_; ++i) {
                if (data.is_null_.at(page_start + i)) {
                  common::ObString empty;
                  if (OB_FAIL(data.values_.push_back(empty))) {
                  }
                } else {
                  common::ObString cell;
                  if (OB_FAIL(parquet_plain_value(vp, vend, meta.physical_, meta.type_length_,
                                                  decode_arena_, cell))) {
                  } else if (OB_FAIL(data.values_.push_back(cell))) {
                  }
                }
              }
            }
            have_values = data.values_.count() >= meta.num_values_;
          } else if (8 == info.encoding_ || 2 == info.encoding_) {
            // RLE_DICTIONARY: one byte of bit width, then the hybrid encoding.
            uint8_t bit_width = 0;
            if (OB_FAIL(vc.read_byte(bit_width))) {
            } else if (OB_UNLIKELY(0 == bit_width && !dictionary.empty())) {
              for (int64_t i = 0; OB_SUCC(ret) && i < info.num_values_; ++i) {
                common::ObString cell = data.is_null_.at(page_start + i)
                                        ? common::ObString() : dictionary.at(0);
                if (OB_FAIL(data.values_.push_back(cell))) {
                }
              }
              have_values = data.values_.count() >= meta.num_values_;
            } else {
              common::ObSEArray<int64_t, 64> indices;
              if (OB_FAIL(parquet_rle_decode(vc, bit_width, non_null, indices))) {
              } else if (OB_UNLIKELY(indices.count() != non_null)) {
                ret = OB_INVALID_DATA;
              } else {
                int64_t value_idx = 0;
                for (int64_t i = 0; OB_SUCC(ret) && i < info.num_values_; ++i) {
                  common::ObString cell;
                  if (!data.is_null_.at(page_start + i)) {
                    const int64_t dict_idx = indices.at(value_idx++);
                    if (OB_UNLIKELY(dict_idx < 0 || dict_idx >= dictionary.count())) {
                      ret = OB_INVALID_DATA;
                      LOG_WARN("dictionary index out of range", K(ret), K(dict_idx));
                    } else {
                      cell = dictionary.at(dict_idx);
                    }
                  }
                  if (OB_SUCC(ret) && OB_FAIL(data.values_.push_back(cell))) {
                  }
                }
                have_values = data.values_.count() >= meta.num_values_;
              }
            }
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet page encoding");
          }
        }
      } else {
        // Neither data nor dictionary: skip the page and keep looking.
      }
      if (OB_SUCC(ret) && !have_values) {
        page_off = (region - base) + region_len;
        if (OB_UNLIKELY(page_off >= size)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("parquet data page not found", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObFileParquetScanner::Impl::load_row_group()
{
  int ret = OB_SUCCESS;
  decode_arena_.reset();
  chunk_data_.reset();
  rows_in_group_ = 0;
  row_index_ = 0;
  if (OB_UNLIKELY(row_group_ >= row_groups_.count())) {
    ret = OB_ITER_END;
  } else {
    common::ObSEArray<ParquetColumnMeta, 16> &chunks = row_groups_.at(row_group_);
    if (OB_UNLIKELY(projection_.count() != chunks.count())) {
      ret = OB_INVALID_DATA;
      LOG_WARN("parquet projection does not match row group", K(ret),
               K(projection_.count()), K(chunks.count()));
    } else {
      rows_in_group_ = chunks.empty() ? 0 : chunks.at(0).num_values_;
      for (int64_t i = 0; OB_SUCC(ret) && i < chunks.count(); ++i) {
        if (!projection_.at(i)) {
          continue;
        }
        ParquetChunkData data;
        if (OB_FAIL(decode_chunk(chunks.at(i), data))) {
        } else if (OB_FAIL(chunk_data_.push_back(data))) {
        }
      }
    }
  }
  return ret;
}

int ObFileParquetScanner::init(common::ObIAllocator &allocator,
                               const common::ObString &path,
                               const ObFileSchema &schema,
                               const common::ObIArray<int8_t> *projection)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_)) {
    Impl *impl = static_cast<Impl *>(allocator.alloc(sizeof(Impl)));
    if (OB_ISNULL(impl)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      impl_ = new (impl) Impl();
    }
  } else {
    impl_->file_.close();
    impl_->row_groups_.reset();
    impl_->chunk_data_.reset();
  }
  if (OB_SUCC(ret)) {
    impl_->allocator_ = &allocator;
    impl_->schema_ = &schema;
    impl_->row_group_ = -1;
    impl_->row_index_ = 0;
    impl_->rows_in_group_ = 0;
    impl_->projection_.reuse();
    impl_->decode_arena_.reset();
    if (OB_FAIL(impl_->file_.init(path))) {
    }
  }
  if (OB_SUCC(ret)) {
    common::ObString tail;
    common::ObString footer;
    common::ObString content;
    const int64_t file_size = impl_->file_.size();
    if (OB_UNLIKELY(file_size < 12)) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(impl_->file_.read_at(file_size - 8, 8, impl_->decode_arena_, tail))) {
    } else {
      const uint8_t *p = reinterpret_cast<const uint8_t *>(tail.ptr());
      const uint32_t footer_len = static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
      if (OB_UNLIKELY(footer_len > static_cast<uint64_t>(file_size - 12) ||
                      footer_len > 64 * 1024 * 1024)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet footer is too large or invalid", K(ret), K(footer_len), K(file_size));
      } else if (OB_FAIL(impl_->file_.read_at(file_size - 8 - footer_len, footer_len,
                                               impl_->decode_arena_, footer))) {
      } else {
        char *buf = static_cast<char *>(impl_->decode_arena_.alloc(footer_len + 12));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, "PAR1", 4);
          if (footer_len > 0) {
            MEMCPY(buf + 4, footer.ptr(), footer_len);
          }
          MEMCPY(buf + 4 + footer_len, tail.ptr(), 8);
          content.assign_ptr(buf, static_cast<int32_t>(footer_len + 12));
        }
      }
    }
    common::ObSEArray<ParquetSchemaLeaf, 16> leaves;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(parquet_parse_footer(allocator, content, leaves,
                                            impl_->row_groups_, impl_->num_row_groups_))) {
    } else if (schema.columns_.empty()) {
      ObFileSchema *mutable_schema = const_cast<ObFileSchema *>(&schema);
      if (OB_UNLIKELY(impl_->row_groups_.empty())) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet file has no row groups", K(ret));
      } else {
        common::ObSEArray<ParquetColumnMeta, 16> &chunks = impl_->row_groups_.at(0);
        for (int64_t i = 0; OB_SUCC(ret) && i < chunks.count(); ++i) {
          ObFileColumn col;
          col.name_ = chunks.at(i).name_;
          col.type_ = chunks.at(i).inferred_;
          col.nullable_ = PQ_REQUIRED != chunks.at(i).repetition_;
          col.ordinal_ = i;
          if (OB_FAIL(mutable_schema->columns_.push_back(col))) {
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    const int64_t column_count = schema.columns_.count();
    if (OB_ISNULL(projection) || projection->empty()) {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
        if (OB_FAIL(impl_->projection_.push_back(1))) {
        }
      }
    } else if (OB_UNLIKELY(projection->count() != column_count)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("parquet projection size differs from schema", K(ret),
               K(projection->count()), K(column_count));
    } else if (OB_FAIL(append(impl_->projection_, *projection))) {
    }
  }
  if (OB_FAIL(ret)) {
    impl_->file_.close();
  }
  return ret;
}

int ObFileParquetScanner::next_row(common::ObIArray<common::ObString> &fields,
                                   common::ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  is_null.reuse();
  if (OB_ISNULL(impl_)) {
    ret = OB_NOT_INIT;
  } else {
    if (impl_->row_index_ >= impl_->rows_in_group_) {
      // The current group is exhausted. The counter starts one before the first
      // group so the initial call loads group 0 instead of skipping it.
      ++impl_->row_group_;
      if (OB_FAIL(impl_->load_row_group())) {
        if (OB_ITER_END != ret) {
          LOG_WARN("failed to load parquet row group", K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_UNLIKELY(impl_->row_index_ >= impl_->rows_in_group_)) {
        ret = OB_ITER_END;
      } else {
        const int64_t row = impl_->row_index_++;
        int64_t decoded_idx = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < impl_->projection_.count(); ++i) {
          if (!impl_->projection_.at(i)) {
            if (OB_FAIL(fields.push_back(common::ObString()))) {
            } else if (OB_FAIL(is_null.push_back(true))) {
            }
          } else if (OB_UNLIKELY(decoded_idx >= impl_->chunk_data_.count())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("missing projected parquet column", K(ret), K(i), K(decoded_idx));
          } else {
            ParquetChunkData &data = impl_->chunk_data_.at(decoded_idx++);
            if (OB_UNLIKELY(row >= data.values_.count() || row >= data.is_null_.count())) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("parquet row index out of range", K(ret), K(row));
            } else if (OB_FAIL(fields.push_back(data.values_.at(row)))) {
            } else if (OB_FAIL(is_null.push_back(data.is_null_.at(row)))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ob_file_sql_detect_format(const common::ObString &path, common::ObString &format)
{
  int ret = OB_SUCCESS;
  const char *base = path_basename(path.ptr());
  const char *dot = strrchr(base, '.');
  if (OB_ISNULL(dot)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "cannot detect file format without an extension");
  } else if (0 == strcasecmp(dot, ".csv")) {
    format = ObString::make_string("csv");
  } else if (0 == strcasecmp(dot, ".jsonl") || 0 == strcasecmp(dot, ".ndjson")) {
    format = ObString::make_string("jsonl");
  } else if (0 == strcasecmp(dot, ".json")) {
    format = ObString::make_string("json");
  } else if (0 == strcasecmp(dot, ".parquet")) {
    format = ObString::make_string("parquet");
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unsupported file extension");
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
