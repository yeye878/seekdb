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

#include "sql/engine/basic/ob_values_table_access_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_obj_cast_runtime.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
OB_SERIALIZE_MEMBER_INHERIT(ObValuesTableAccessSpec,
                            ObOpSpec,
                            access_type_,
                            column_exprs_,
                            value_exprs_,
                            start_param_idx_,
                            end_param_idx_,
                            obj_params_,
                            file_projection_,
                            has_file_table_,
                            file_table_def_);

ObValuesTableAccessOp::ObValuesTableAccessOp(ObExecContext &exec_ctx,
                                             const ObOpSpec &spec,
                                             ObOpInput *input)
  : ObOperator(exec_ctx, spec, input), datum_caster_(), cm_(CM_NONE),
    row_idx_(0), row_cnt_(0), csv_scanner_(nullptr), json_scanner_(nullptr),
    parquet_scanner_(nullptr), glob_paths_(), glob_index_(0), file_path_(),
    file_row_arena_(common::ObModIds::OB_SQL_EXECUTOR), in_batch_(false) {}

int ObValuesTableAccessOp::inner_open()
{
  int ret = OB_SUCCESS;
  row_idx_ = 0;
  if (MY_SPEC.has_file_table_) {
    ret = open_file_scan();
    return ret;
  }
  const int32_t result_flag = 0;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (OB_ISNULL(plan_ctx) || OB_ISNULL(ctx_.get_sql_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(MY_SPEC.column_exprs_.empty())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(datum_caster_.init(eval_ctx_.exec_ctx_))) {
  } else if (OB_FAIL(ObSQLUtils::get_default_cast_mode(false, 0, GET_MY_SESSION(ctx_), cm_))) {
  } else {
    cm_ = cm_ | CM_COLUMN_CONVERT;
    if (OB_SUCC(ret)) {
      if (ObValuesTableDef::FOLD_ACCESS_EXPR == MY_SPEC.access_type_) {
        if (OB_UNLIKELY(MY_SPEC.start_param_idx_ < 0 ||
                        MY_SPEC.start_param_idx_ > MY_SPEC.end_param_idx_ ||
                        MY_SPEC.end_param_idx_ >= plan_ctx->get_param_store().count())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const ObObj &first_column_node = plan_ctx->get_param_store().at(MY_SPEC.start_param_idx_);
          if (OB_FAIL(first_column_node.get_real_param_count(row_cnt_))) {
          }
        }
      } else if (ObValuesTableDef::ACCESS_PARAM == MY_SPEC.access_type_) {
        int64_t column_cnt = MY_SPEC.column_exprs_.count();
        row_cnt_ = (MY_SPEC.end_param_idx_ - MY_SPEC.start_param_idx_ + 1) / column_cnt;
      } else if (ObValuesTableDef::ACCESS_OBJ == MY_SPEC.access_type_) {
        int64_t column_cnt = MY_SPEC.column_exprs_.count();
        row_cnt_ = MY_SPEC.obj_params_.count() / column_cnt;
      } else {
        int64_t column_cnt = MY_SPEC.column_exprs_.count();
        row_cnt_ = MY_SPEC.value_exprs_.count() / column_cnt;
      }
    }
  }
  return ret;
}

int ObValuesTableAccessOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (MY_SPEC.has_file_table_) {
    release_file_scanners();
    if (OB_FAIL(open_file_scan())) {
    }
  } else {
    row_idx_ = 0;
  }
  return ret;
}

int ObValuesTableAccessOp::switch_iterator()
{
  int ret = OB_SUCCESS;
  const int switch_ret = ObOperator::inner_switch_iterator();
  if (OB_SUCCESS != switch_ret && OB_ITER_END != switch_ret) {
    ret = switch_ret;
  } else if (MY_SPEC.has_file_table_) {
    release_file_scanners();
    if (OB_FAIL(open_file_scan())) {
    }
  } else {
    row_idx_ = 0;
  }
  return ret;
}

int ObValuesTableAccessOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (MY_SPEC.has_file_table_) {
    clear_evaluated_flag();
    if (OB_FAIL(try_check_status())) {
    } else if (OB_FAIL(get_next_file_row())) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to scan file row", K(ret));
      }
    }
    return ret;
  }
  if (OB_FAIL(try_check_status())) {
  } else {
    do {
      clear_evaluated_flag();
      if (OB_FAIL(calc_next_row())) {
        if(OB_ITER_END != ret) {
        }
      } else {
        LOG_DEBUG("output row", "row", ROWEXPR2STR(eval_ctx_, MY_SPEC.output_));
      }
      break;
    } while (OB_SUCC(ret));
  }
  return ret;
}

int ObValuesTableAccessOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  int64_t row_count = 0;
  bool reached_end = false;
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
  guard.set_batch_size(max_row_cnt);
  if (OB_FAIL(try_check_status())) {
  } else {
    if (MY_SPEC.has_file_table_) {
      in_batch_ = true;
    }
    while (OB_SUCC(ret) && row_count < max_row_cnt) {
      guard.set_batch_idx(row_count);
      if (MY_SPEC.has_file_table_) {
        ret = get_next_file_row();
      } else {
        clear_evaluated_flag();
        ret = calc_next_row();
      }
      if (OB_FAIL(ret)) {
        if (OB_ITER_END == ret) {
          reached_end = true;
          ret = OB_SUCCESS;
        }
        break;
      }
      ++row_count;
    }
  }
  in_batch_ = false;
  brs_.size_ = row_count;
  brs_.end_ = reached_end;
  return ret;
}

void ObValuesTableAccessOp::update_src_meta(const ObObjMeta &src_obj_meta,
                                            const ObAccuracy &src_obj_acc,
                                            ObDatumMeta &src_meta)
{
  src_meta.type_ = src_obj_meta.get_type();
  src_meta.cs_type_ = src_obj_meta.get_collation_type();
  src_meta.scale_ = src_obj_acc.get_scale();
  src_meta.precision_ = src_obj_acc.get_precision();
}

int ObValuesTableAccessOp::get_real_src_obj_type(const int64_t row_idx,
                                                 ObExpr &src_expr,
                                                 ObDatumMeta &src_meta,
                                                 ObObjMeta &src_obj_meta)
{
  int ret = OB_SUCCESS;
  if (T_QUESTIONMARK == src_expr.type_) {
    int64_t param_idx = src_expr.extra_;
    ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
    const ObSqlArrayObj *array_obj = NULL;
    if (OB_UNLIKELY(param_idx < 0 || param_idx >= plan_ctx->get_param_store().count())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (plan_ctx->get_param_store().at(param_idx).is_ext_sql_array()) {
      // If the parameter is is_ext_sql_array
      if (OB_ISNULL(array_obj = reinterpret_cast<const ObSqlArrayObj*>(
                                            plan_ctx->get_param_store().at(param_idx).get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (array_obj->count_ <= row_idx) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        src_obj_meta = array_obj->data_[row_idx].meta_;
        const ObAccuracy &src_obj_acc = array_obj->data_[row_idx].get_accuracy();
        update_src_meta(src_obj_meta, src_obj_acc, src_meta);
      }
    } else if (src_expr.frame_idx_ < spec_.plan_->get_expr_frame_info().const_frame_.count() +
                                     spec_.plan_->get_expr_frame_info().param_frame_.count()) {
      src_obj_meta = plan_ctx->get_param_store().at(param_idx).meta_;
      const ObAccuracy &src_obj_acc = plan_ctx->get_param_store().at(param_idx).get_accuracy();
      update_src_meta(src_obj_meta, src_obj_acc, src_meta);
    }
  }
  return ret;
}

int ObValuesTableAccessOp::calc_datum_from_expr(const int64_t col_idx,
                                                const int64_t row_idx,
                                                ObExpr *src_expr,
                                                ObExpr *dst_expr)
{
  int ret = OB_SUCCESS;
  ObDatumMeta src_meta = src_expr->datum_meta_;
  ObObjMeta src_obj_meta = src_expr->obj_meta_;
  ObDatum *datum = NULL;
  bool need_adjust_decimal_int = false;
  if (src_expr == dst_expr) {
    /* actually should not happened */
  } else if (T_QUESTIONMARK == src_expr->type_ &&
             OB_FAIL(get_real_src_obj_type(row_idx, *src_expr, src_meta, src_obj_meta))) {
    LOG_WARN("failed to get real obj type");
  } else {
    need_adjust_decimal_int = src_meta.type_ == ObDecimalIntType &&
                              dst_expr->datum_meta_.type_ == ObDecimalIntType &&
                              ObDatumCast::need_scale_decimalint(src_meta.scale_,
                                                                 src_meta.precision_,
                                                                 dst_expr->datum_meta_.scale_,
                                                                 dst_expr->datum_meta_.precision_);
  }
  if (OB_FAIL(ret)) {
  } else if (src_meta.type_ == dst_expr->datum_meta_.type_ &&
             src_meta.cs_type_ == dst_expr->datum_meta_.cs_type_ &&
             !need_adjust_decimal_int) {
    // when type and collation_type is same. src has the same type to dst;
    if (OB_FAIL(src_expr->eval(eval_ctx_, datum))) {
    } else {
      dst_expr->locate_datum_for_write(eval_ctx_) = *datum;
      dst_expr->set_evaluated_projected(eval_ctx_);
    }
  } else {
    if (OB_FAIL(ObCharset::check_valid_implicit_convert(src_meta.cs_type_,
                                                        dst_expr->datum_meta_.cs_type_))) {
    } else {
      ObExpr real_src_expr = *src_expr;
      real_src_expr.datum_meta_ = src_meta;
      real_src_expr.obj_meta_ = src_obj_meta;
      real_src_expr.obj_datum_map_ = ObDatum::get_obj_datum_map_type(src_meta.type_);
      if (OB_FAIL(datum_caster_.to_type(dst_expr->datum_meta_, real_src_expr, cm_, datum))) {
      } else {
        ObDatum &dst_datum = dst_expr->locate_datum_for_write(eval_ctx_);
        if (ObObjDatumMapType::OBJ_DATUM_STRING == dst_expr->obj_datum_map_) {
          ObExprStrResAlloc res_alloc(*dst_expr, eval_ctx_);
          if (OB_FAIL(dst_datum.deep_copy(*datum, res_alloc))) {
          }
        } else {
          ObDataBuffer res_alloc(const_cast<char*>(dst_datum.ptr_), dst_expr->res_buf_len_);
          if (OB_FAIL(dst_datum.deep_copy(*datum, res_alloc))) {
          }
        }
        dst_expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

int ObValuesTableAccessOp::calc_datum_from_param(const ObObj &src_obj, ObExpr *dst_expr)
{
  int ret = OB_SUCCESS;
  ObDatum &dst_datum = dst_expr->locate_datum_for_write(eval_ctx_);
  const ObObjType &src_type = src_obj.get_type();
  const ObObjType &dst_type =  dst_expr->obj_meta_.get_type();
  const ObObjParam src_obj_param = static_cast<const ObObjParam>(src_obj);
  bool need_adjust_decimal_int = ob_is_decimal_int_tc(src_type)
                                 && ob_is_decimal_int_tc(dst_expr->datum_meta_.type_)
                                 && ObDatumCast::need_scale_decimalint(src_obj_param.get_scale(),
                                                                       src_obj_param.get_precision(),
                                                                       dst_expr->datum_meta_.scale_,
                                                                       dst_expr->datum_meta_.precision_);
  if (ob_is_decimal_int_tc(src_type) && (ob_is_number_tc(dst_type) || need_adjust_decimal_int)) {
    ObObj dst_obj;
    const ObDataTypeCastParams dtc_params = ObBasicSessionInfo::create_dtc_params(GET_MY_SESSION(ctx_));
    ObAccuracy dst_accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY2[0][dst_type];
    if (ob_is_decimal_int_tc(dst_type)) {
      dst_accuracy.set_scale(dst_expr->datum_meta_.scale_);
      dst_accuracy.set_precision(dst_expr->datum_meta_.precision_);
    }
    ObCastCtx cast_ctx(&eval_ctx_.exec_ctx_.get_allocator(), &dtc_params, cm_, dst_expr->obj_meta_.get_collation_type(), &dst_accuracy);
    ObSqlObjCastRuntime cast_runtime(&eval_ctx_.exec_ctx_);
    cast_ctx.runtime_ = &cast_runtime;
    if (OB_FAIL(ObObjCaster::to_type(dst_type, dst_expr->obj_meta_.get_collation_type(), cast_ctx,
                                     src_obj, dst_obj))) {
    } else if (OB_FAIL(dst_datum.from_obj(dst_obj))) {
    } else {
      dst_expr->set_evaluated_projected(eval_ctx_);
    }
  } else if (OB_FAIL(dst_datum.from_obj(src_obj))) {
  } else {
    dst_expr->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

OB_INLINE int ObValuesTableAccessOp::calc_next_row()
{
  int ret = OB_SUCCESS;
  int64_t col_num = MY_SPEC.column_exprs_.count();
  int64_t col_idx = 0;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (row_idx_ == row_cnt_) {
    ret = OB_ITER_END;
  } else if (ObValuesTableDef::FOLD_ACCESS_EXPR == MY_SPEC.access_type_ &&
             OB_FAIL(plan_ctx->replace_batch_param_datum(row_idx_,
                                          MY_SPEC.start_param_idx_,
                                          MY_SPEC.end_param_idx_ - MY_SPEC.start_param_idx_ + 1))) {
  } else {
    while (OB_SUCC(ret) && col_idx < col_num) {
      ObExpr *col_expr = MY_SPEC.column_exprs_.at(col_idx);
      switch (MY_SPEC.access_type_)
      {
        case ObValuesTableDef::ACCESS_EXPR : {
          int64_t idx = row_idx_ * col_num + col_idx;
          ObExpr *src_expr = MY_SPEC.value_exprs_.at(idx);
          if (OB_FAIL(calc_datum_from_expr(col_idx, row_idx_, src_expr, col_expr))) {
          }
          break;
        }
        case ObValuesTableDef::FOLD_ACCESS_EXPR : {
          ObExpr *src_expr = MY_SPEC.value_exprs_.at(col_idx);
          if (OB_FAIL(calc_datum_from_expr(col_idx, row_idx_, src_expr, col_expr))) {
          }
          break;
        }
        case ObValuesTableDef::ACCESS_PARAM: {
          int64_t param_idx = MY_SPEC.start_param_idx_ + col_idx + row_idx_ * col_num;
          const ObObjParam &param = plan_ctx->get_param_store().at(param_idx);
          if (OB_FAIL(calc_datum_from_param(param, col_expr))) {
          }
          break;
        }
        case ObValuesTableDef::ACCESS_OBJ: {
          int64_t idx = row_idx_ * col_num + col_idx;
          const ObObj &param = MY_SPEC.obj_params_.at(idx);
          if (OB_FAIL(calc_datum_from_param(param, col_expr))) {
          }
          break;
        }
      };
      col_idx++;
    } // while end

    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.output_.count(); i++) {
      ObExpr *output = MY_SPEC.output_.at(i);
      if (i < col_num && output == MY_SPEC.column_exprs_.at(i)) {
        /* need do nothing */
      } else {
        ObDatum *dst_datum = NULL;
        MY_SPEC.output_.at(i)->eval(eval_ctx_, dst_datum);
      }
    }
    if (OB_SUCC(ret)) {
      ++row_idx_;
    }
  }
  return ret;
}

int ObValuesTableAccessOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  common::ObString secure_file_priv;
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  const ObFileTableDef &def = MY_SPEC.file_table_def_;
  file_row_arena_.reset();
  row_idx_ = 0;
  in_batch_ = false;
  glob_index_ = 0;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_file_table_enabled(enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "file table function is disabled, set enable_file_sql=ON first");
  } else if (OB_FAIL(session->get_secure_file_priv(secure_file_priv))) {
  } else if (OB_FAIL(ObSQLUtils::get_default_cast_mode(false, 0, session, cm_))) {
  } else {
    cm_ |= CM_COLUMN_CONVERT;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FILE_FORMAT_GLOB == def.format_) {
    if (OB_FAIL(ob_file_sql_resolve_paths(ctx_.get_allocator(), def.path_,
                                          secure_file_priv, glob_paths_))) {
    }
  } else {
    common::ObSEArray<common::ObString, 4> paths;
    if (OB_FAIL(ob_file_sql_resolve_paths(ctx_.get_allocator(), def.path_,
                                          secure_file_priv, paths))) {
    } else if (OB_UNLIKELY(1 != paths.count())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file function requires exactly one input file");
    } else {
      file_path_ = paths.at(0);
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(check_file_identity())) {
    } else if (OB_FILE_FORMAT_CSV == def.format_) {
      if (OB_ISNULL(csv_scanner_)) {
        void *buf = ctx_.get_allocator().alloc(sizeof(ObFileCsvScanner));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          csv_scanner_ = new (buf) ObFileCsvScanner();
        }
      } else {
        csv_scanner_->release();
      }
      if (OB_SUCC(ret) && OB_FAIL(csv_scanner_->init(ctx_.get_allocator(), file_path_,
                                                     def.options_, def.schema_))) {
      }
    } else if (OB_FILE_FORMAT_JSON == def.format_) {
      if (OB_ISNULL(json_scanner_)) {
        void *buf = ctx_.get_allocator().alloc(sizeof(ObFileJsonScanner));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          json_scanner_ = new (buf) ObFileJsonScanner();
        }
      } else {
        json_scanner_->release();
      }
      if (OB_SUCC(ret) && OB_FAIL(json_scanner_->init(ctx_.get_allocator(), file_path_,
                                                      def.schema_))) {
      }
    } else if (OB_FILE_FORMAT_PARQUET == def.format_) {
      if (OB_ISNULL(parquet_scanner_)) {
        void *buf = ctx_.get_allocator().alloc(sizeof(ObFileParquetScanner));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          parquet_scanner_ = new (buf) ObFileParquetScanner();
        }
      } else {
        parquet_scanner_->release();
      }
      if (OB_SUCC(ret) && OB_FAIL(parquet_scanner_->init(ctx_.get_allocator(), file_path_,
                                                         def.schema_,
                                                         &MY_SPEC.file_projection_))) {
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected file format", K(ret), K(def.format_));
    }
  }
  if (OB_FAIL(ret)) {
    release_file_scanners();
  }
  return ret;
}

int ObValuesTableAccessOp::check_file_identity()
{
  int ret = OB_SUCCESS;
  const ObFileTableDef &def = MY_SPEC.file_table_def_;
  ObFileIdentity current;
  if (OB_FILE_FORMAT_GLOB == def.format_) {
  } else if (OB_UNLIKELY(!def.identity_.valid_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table has no inferred file identity", K(ret), K(def.path_));
  } else if (OB_FAIL(ob_file_sql_get_file_identity(file_path_, current))) {
  } else if (!ob_file_sql_same_identity(def.identity_, current)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "input file changed after schema inference");
    LOG_WARN("input file identity changed", K(ret), K(file_path_),
             K(def.identity_.device_), K(current.device_), K(def.identity_.inode_),
             K(current.inode_), K(def.identity_.size_), K(current.size_),
             K(def.identity_.mtime_ns_), K(current.mtime_ns_));
  }
  return ret;
}

int ObValuesTableAccessOp::get_next_file_row()
{
  int ret = OB_SUCCESS;
  common::ObSEArray<common::ObString, 24> fields;
  common::ObSEArray<bool, 24> is_null;
  const ObFileFormat format = MY_SPEC.file_table_def_.format_;
  if (OB_FILE_FORMAT_GLOB == format) {
    if (glob_index_ >= glob_paths_.count()) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(fields.push_back(glob_paths_.at(glob_index_++)))) {
    } else if (OB_FAIL(is_null.push_back(false))) {
    }
  } else if (OB_FILE_FORMAT_CSV == format) {
    if (OB_ISNULL(csv_scanner_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(csv_scanner_->next_row(fields, is_null))) {
    }
  } else if (OB_FILE_FORMAT_JSON == format) {
    if (OB_ISNULL(json_scanner_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(json_scanner_->next_row(fields, is_null))) {
    }
  } else if (OB_FILE_FORMAT_PARQUET == format) {
    if (OB_ISNULL(parquet_scanner_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(parquet_scanner_->next_row(fields, is_null))) {
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  if (OB_SUCC(ret) && 0 == fields.count() && 0 == is_null.count() &&
      0 < MY_SPEC.file_table_def_.schema_.columns_.count()) {
    ret = OB_ITER_END;
  }
  if (OB_FAIL(ret)) {
    if (OB_ITER_END == ret) {
      if (OB_FILE_FORMAT_GLOB != format) {
        const int identity_ret = check_file_identity();
        if (OB_SUCCESS != identity_ret) {
          ret = identity_ret;
          LOG_WARN("failed to verify file identity at EOF", K(ret), K(row_idx_), K(file_path_));
        }
      }
    } else {
      LOG_WARN("failed to read file row", K(ret), K(row_idx_), K(format), K(file_path_));
    }
  } else {
    // Keep the previous batch's evaluated state intact when the next read reaches EOF.
    clear_evaluated_flag();
    file_row_arena_.reset();
    if (OB_FAIL(emit_file_row(fields, is_null))) {
    } else {
      ++row_idx_;
    }
  }
  return ret;
}

int ObValuesTableAccessOp::emit_file_row(const common::ObIArray<common::ObString> &fields,
                                         const common::ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  const ObFileSchema &schema = MY_SPEC.file_table_def_.schema_;
  const int64_t column_cnt = MY_SPEC.column_exprs_.count();
  if (OB_UNLIKELY(fields.count() != column_cnt || is_null.count() != column_cnt ||
                  schema.columns_.count() != column_cnt)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("file schema and physical columns differ", K(ret), K(fields.count()),
             K(is_null.count()), K(column_cnt), K(schema.columns_.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_cnt; ++i) {
    ObObjParam obj;
    ObExpr *expr = MY_SPEC.column_exprs_.at(i);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (is_null.at(i)) {
      obj.set_null();
    } else if (OB_FAIL(ob_file_sql_cell_to_obj(file_row_arena_, fields.at(i),
                                               schema.columns_.at(i).type_,
                                               GET_MY_SESSION(ctx_)->get_timezone_info(),
                                               expr->obj_meta_.get_collation_type(), obj))) {
    }
    if (OB_SUCC(ret)) {
      obj.set_param_meta();
      if (OB_FAIL(calc_datum_from_param(obj, expr))) {
      } else if (in_batch_ && !obj.is_null() && ob_is_string_type(obj.get_type())) {
        if (OB_FAIL(expr->deep_copy_self_datum(eval_ctx_))) {
        }
      }
    }
  }
  return ret;
}

void ObValuesTableAccessOp::release_file_scanners()
{
  if (OB_NOT_NULL(csv_scanner_)) {
    csv_scanner_->release();
  }
  if (OB_NOT_NULL(json_scanner_)) {
    json_scanner_->release();
  }
  if (OB_NOT_NULL(parquet_scanner_)) {
    parquet_scanner_->release();
  }
  glob_paths_.reuse();
  glob_index_ = 0;
  file_path_.reset();
  file_row_arena_.reset();
}

void ObValuesTableAccessOp::destroy_file_scanners()
{
  if (OB_NOT_NULL(csv_scanner_)) {
    csv_scanner_->~ObFileCsvScanner();
    csv_scanner_ = nullptr;
  }
  if (OB_NOT_NULL(json_scanner_)) {
    json_scanner_->~ObFileJsonScanner();
    json_scanner_ = nullptr;
  }
  if (OB_NOT_NULL(parquet_scanner_)) {
    parquet_scanner_->~ObFileParquetScanner();
    parquet_scanner_ = nullptr;
  }
  glob_paths_.reset();
  glob_index_ = 0;
  file_path_.reset();
  file_row_arena_.reset();
}

void ObValuesTableAccessOp::destroy()
{
  destroy_file_scanners();
  ObOperator::destroy();
}

int ObValuesTableAccessOp::inner_close()
{
  int ret = OB_SUCCESS;
  row_idx_ = 0;
  if (MY_SPEC.has_file_table_) {
    if (OB_FILE_FORMAT_GLOB != MY_SPEC.file_table_def_.format_) {
      ret = check_file_identity();
    }
    release_file_scanners();
    return ret;
  }
  if (OB_FAIL(datum_caster_.destroy())) {
  }
  return ret;
}
} // end namespace sql
} // end namespace oceanbase
