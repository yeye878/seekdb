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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_file_table_resolver_helper.h"
#include <string.h>

namespace oceanbase
{
namespace sql
{

namespace
{

int copy_cell(common::ObIAllocator &allocator,
              const common::ObString &src,
              common::ObString &dst)
{
  int ret = OB_SUCCESS;
  if (src.empty()) {
    dst.reset();
  } else {
    char *buf = static_cast<char *>(allocator.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign_ptr(buf, src.length());
    }
  }
  return ret;
}

int fill_columns(common::ObIAllocator &allocator,
                 const ObFileSchema &schema,
                 ObFileTableRows &rows)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < schema.columns_.count(); ++i) {
    const ObFileColumn &src = schema.columns_.at(i);
    ObFileColumn col;
    col.type_ = src.type_;
    col.nullable_ = src.nullable_;
    col.ordinal_ = src.ordinal_;
    if (OB_FAIL(copy_cell(allocator, src.name_, col.name_))) {
    } else if (OB_FAIL(rows.columns_.push_back(col))) {
    } else if (OB_FAIL(rows.column_types_.push_back(ob_file_column_to_obj_type(src.type_)))) {
    }
  }
  return ret;
}

int load_glob_schema(common::ObIAllocator &allocator,
                     const common::ObString &pattern,
                     const common::ObString &secure_file_priv,
                     ObFileTableRows &rows)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<common::ObString, 16> paths;
  if (OB_FAIL(ob_file_sql_resolve_paths(allocator, pattern, secure_file_priv, paths))) {
  } else {
    ObFileColumn name_col;
    name_col.name_ = common::ObString::make_string("file");
    name_col.type_ = OB_FILE_TYPE_VARCHAR;
    name_col.nullable_ = false;
    name_col.ordinal_ = 0;
    if (OB_FAIL(rows.columns_.push_back(name_col))) {
    } else if (OB_FAIL(rows.column_types_.push_back(common::ObVarcharType))) {
    }
  }
  return ret;
}

int load_file_schema(common::ObIAllocator &allocator,
                     const ObFileReadOptions &options,
                     const common::ObString &path,
                     const common::ObString &secure_file_priv,
                     const ObFileFormat format,
                     ObFileTableRows &rows)
{
  int ret = OB_SUCCESS;
  ObFileSchema schema;
  common::ObSEArray<common::ObString, 4> paths;
  if (OB_FAIL(ob_file_sql_resolve_paths(allocator, path, secure_file_priv, paths))) {
  } else if (OB_UNLIKELY(1 != paths.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expected exactly one file", K(ret), K(paths.count()));
  } else if (OB_FAIL(ob_file_sql_get_file_identity(paths.at(0), rows.identity_))) {
  } else if (OB_FILE_FORMAT_CSV == format) {
    if (OB_FAIL(ob_file_sql_infer_csv(allocator, paths.at(0), options, schema))) {
    } else if (OB_FAIL(fill_columns(allocator, schema, rows))) {
    }
  } else if (OB_FILE_FORMAT_JSON == format) {
    if (OB_FAIL(ob_file_sql_infer_json(allocator, paths.at(0), schema))) {
    } else if (OB_FAIL(fill_columns(allocator, schema, rows))) {
    }
  } else if (OB_FILE_FORMAT_PARQUET == format) {
    if (OB_FAIL(ob_file_sql_infer_parquet(allocator, paths.at(0), schema))) {
    } else if (OB_FAIL(fill_columns(allocator, schema, rows))) {
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected file format", K(ret), K(format));
  }
  if (OB_SUCC(ret)) {
    ObFileIdentity after;
    if (OB_FAIL(ob_file_sql_get_file_identity(paths.at(0), after))) {
    } else if (!ob_file_sql_same_identity(rows.identity_, after)) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "input file changed during schema inference");
    } else {
      rows.identity_ = after;
    }
  }
  return ret;
}

} // anonymous namespace

// Resolves only metadata. Ordinary queries never append file cells to `rows`;
// the small cell array is used later solely for DESCRIBE's six metadata columns.
int ob_file_table_load_rows(common::ObIAllocator &allocator,
                            const ObFileReadOptions &options,
                            const common::ObString &path,
                            const common::ObString &secure_file_priv,
                            const bool enable_file_sql,
                            const bool is_glob,
                            const ObFileFormat format,
                            ObFileTableRows &rows)
{
  int ret = OB_SUCCESS;
  rows.columns_.reuse();
  rows.column_types_.reuse();
  rows.cells_.reuse();
  rows.is_null_.reuse();
  if (!enable_file_sql) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "file table function is disabled, set enable_file_sql=ON first");
  } else if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file table function requires a file path");
  } else if (is_glob) {
    if (OB_FAIL(load_glob_schema(allocator, path, secure_file_priv, rows))) {
    }
  } else if (OB_FAIL(load_file_schema(allocator, options, path, secure_file_priv, format, rows))) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
