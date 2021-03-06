/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file square_sum-inl.h
 * \brief This is a temporary solution for fusing operators
 * square and sum together as a composite op for row sparse tensors.
 * The purpose for fusing square and sum for row sparse tensors
 * is that the gradient of the fused operator depends on the input
 * ndarray and thus its gradient is a row-sparse ndarray too.
 * This fused op will become deprecated after the functionality
 * of fusing operators is finished in the future.
 */

#ifndef MXNET_OPERATOR_TENSOR_SQUARE_SUM_INL_H_
#define MXNET_OPERATOR_TENSOR_SQUARE_SUM_INL_H_

#include <vector>
#include <algorithm>
#include <utility>
#include "../mxnet_op.h"
#include "./broadcast_reduce_op.h"
#include "./init_op.h"

namespace mxnet {
namespace op {

inline bool SquareSumForwardInferStorageType(const nnvm::NodeAttrs& attrs,
                                             const Context& ctx,
                                             std::vector<int>* in_attrs,
                                             std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);
  const ReduceAxesParam& param = nnvm::get<ReduceAxesParam>(attrs.parsed);
  if (in_attrs->at(0) == kRowSparseStorage) {  // current impl
    if (param.axis[0] == 1 && param.keepdims) {  // sum per row and keep dims
      STORAGE_TYPE_ASSIGN_CHECK(*out_attrs, 0, kRowSparseStorage);
    } else {
      STORAGE_TYPE_ASSIGN_CHECK(*out_attrs, 0, kDefaultStorage);
    }
  } else {  // fallback
    type_assign(&((*in_attrs)[0]), kDefaultStorage);
    type_assign(&((*out_attrs)[0]), kDefaultStorage);
  }
  return true;
}

inline bool SquareSumBackwardInferStorageType(const nnvm::NodeAttrs& attrs,
                                              const Context& ctx,
                                              std::vector<int>* in_attrs,
                                              std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  if (in_attrs->at(0) == kDefaultStorage || in_attrs->at(0) == kRowSparseStorage) {
    STORAGE_TYPE_ASSIGN_CHECK(*in_attrs, 1, kRowSparseStorage);
    STORAGE_TYPE_ASSIGN_CHECK(*out_attrs, 0, kRowSparseStorage);
  } else {  // fallback
    type_assign(&((*in_attrs)[0]), kDefaultStorage);
    type_assign(&((*in_attrs)[1]), kDefaultStorage);
    type_assign(&((*out_attrs)[0]), kDefaultStorage);
  }
  return true;
}

/*!
 * \brief square sum of a rsp
 * if axis = -1, same as mx.nd.sum(tensor*tensor)
 * if axis = 0, same as mx.nd.sum(tensor*tensor, axis=0)
 * if axis = 1, same as mx.nd.sum(tensor*tensor, axis=1)
 * where tensor*tensor is elemwise multiplication of two ndarrays.
 */
template<int req, int axis, bool keepdim>
struct SquareSumRspKernel;

/*!
 * \brief square sum of a rsp on axis=0 without keeping the dim
 */
template<int req>
struct SquareSumRspKernel<req, 0, false> {
  /*!
   * \param j the element index in out_data and column id of in_data
   */
  template<typename DType>
  MSHADOW_XINLINE static void Map(int j, DType* out_data, const DType* in_data,
                                  const int64_t nnr, const int64_t num_cols) {
    DType sum = 0;
    for (int64_t i = 0; i < nnr; ++i) {
      const DType val = in_data[i*num_cols+j];
      sum += val * val;
    }
    KERNEL_ASSIGN(out_data[j], req, sum);
  }
};

/*!
 * \brief square sum of a rsp on axis=1 without keeping the dim
 */
template<int req>
struct SquareSumRspKernel<req, 1, false> {
  /*!
   * \param i the i-th non-zero row of in_data
   */
  template<typename IType, typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, const IType* in_row_idx,
                                  const DType* in_data, const int64_t num_cols) {
    DType sum = 0;
    const int64_t offset = i * num_cols;
    for (int64_t j = 0; j < num_cols; ++j) {
      const DType val = in_data[offset+j];
      sum += val * val;
    }
    KERNEL_ASSIGN(out_data[in_row_idx[i]], req, sum);
  }
};

/*!
 * \brief square sum of a rsp on axis=1 keeping the dim
 */
template<int req>
struct SquareSumRspKernel<req, 1, true> {
  /*!
   * \param i the i-th non-zero row of in_data
   */
  template<typename IType, typename DType>
  MSHADOW_XINLINE static void Map(int i, IType* out_row_idx, DType* out_data,
                                  const IType* in_row_idx, const DType* in_data,
                                  const int64_t num_cols) {
    DType sum = 0;
    out_row_idx[i] = in_row_idx[i];
    const int64_t offset = i * num_cols;
    for (int64_t j = 0; j < num_cols; ++j) {
      const DType val = in_data[offset+j];
      sum += val * val;
    }
    KERNEL_ASSIGN(out_data[i], req, sum);
  }
};

template<int req, int axis, int ograd_stype = kDefaultStorage>
struct SquareSumRspGradKernel;

template<int req>
struct SquareSumRspGradKernel<req, 0> {
  /*!
   * \param i element index in in_grad and in_data
   * \param in_grad_row_idx row_idx of the gradient of the op's input
   * \param in_grad gradient of the op's input
   * \param out_grad gradient of the op's output
   * \param in_row_idx row idx of the op's input
   * \param in_data op's input
   */
  template<typename IType, typename DType>
  MSHADOW_XINLINE static void Map(int i, IType* in_grad_row_idx, DType* in_grad,
                                  const DType* out_grad, const IType* in_row_idx,
                                  const DType* in_data, const int64_t num_cols) {
    const int64_t row = i / num_cols;
    in_grad_row_idx[row] = in_row_idx[row];
    KERNEL_ASSIGN(in_grad[i], req, 2*in_data[i]*out_grad[i%num_cols]);
  }
};

template<int req>
struct SquareSumRspGradKernel<req, 1> {
  /*!
   * \param i element index in in_grad and in_data
   * \param in_grad_row_idx row_idx of the gradient of the op's input
   * \param in_grad gradient of the op's input
   * \param out_grad gradient of the op's output
   * \param in_row_idx row idx of the op's input
   * \param in_data op's input
   */
  template<typename IType, typename DType>
  MSHADOW_XINLINE static void Map(int i, IType* in_grad_row_idx, DType* in_grad,
                                  const DType* out_grad, const IType* in_row_idx,
                                  const DType* in_data, const int64_t num_cols) {
    const int64_t row = i / num_cols;
    in_grad_row_idx[row] = in_row_idx[row];
    KERNEL_ASSIGN(in_grad[i], req, 2*in_data[i]*out_grad[in_row_idx[row]]);
  }
};

/*!
 * Note: This kernel assumes that the ograd and in_data
 * are all rsp and have equal row_idx array, or
 * in_data is a full rsp.
 */
template<int req>
struct SquareSumRspGradKernel<req, 1, kRowSparseStorage> {
  /*!
   * \param i index of igrad.data()
   * \param in_grad_row_idx row_idx of the gradient of the op's input
   * \param in_grad gradient of the op's input
   * \param out_grad_row_idx row_idx of the gradient of the op's output
   * \param out_grad gradient of the op's output
   * \param in_data op's input
   */
  template<typename IType, typename DType>
  MSHADOW_XINLINE static void Map(int i, IType* in_grad_row_idx, DType* in_grad,
                                  const IType* out_grad_row_idx, const DType* out_grad,
                                  const DType* in_data, const int64_t num_cols) {
    const int64_t row = i / num_cols;
    in_grad_row_idx[row] = out_grad_row_idx[row];
    KERNEL_ASSIGN(in_grad[i], req, 2*in_data[i]*out_grad[row]);
  }
};

template<typename xpu>
void SquareSumRspImpl(const nnvm::NodeAttrs& attrs,
                      mshadow::Stream<xpu>* s,
                      const NDArray& input,
                      const OpReqType req,
                      NDArray* output) {
  if (req == kNullOp) return;
  const ReduceAxesParam& param = nnvm::get<ReduceAxesParam>(attrs.parsed);
  CHECK_EQ(param.axis.ndim(), 1U) << "_square_sum(row_sparse_matrix) only supports axis=0 or 1";
  CHECK(param.axis[0] == 0 || param.axis[0] == 1)
    << "_square_sum(row_sparse_matrix) only supports axis=0 or 1";
  CHECK_EQ(input.storage_type(), kRowSparseStorage)
    << "_square_sum op only supports row-sparse matrix as input";
  int64_t out_data_size = 0;
  if (param.axis[0] == 0) {  // axis = 0
    CHECK_EQ(output->storage_type(), kDefaultStorage);
    out_data_size = input.storage_shape()[1];
  } else if (param.keepdims) {  // axis = 1, keepdims = true
    CHECK_EQ(output->storage_type(), kRowSparseStorage);
    out_data_size = input.storage_shape()[0];
  } else {  // axis = 1, keepdims = false
    CHECK_EQ(output->storage_type(), kDefaultStorage);
    out_data_size = input.shape()[0];
  }
  CHECK_NE(req, kWriteInplace);

  using namespace mxnet_op;
  if (!input.storage_initialized()) {
    if (req == kWriteTo) {
      if (output->storage_type() == kDefaultStorage) {
        MSHADOW_TYPE_SWITCH(output->data().type_flag_, DType, {
          Kernel<set_zero, xpu>::Launch(s, out_data_size, output->data().dptr<DType>());
        })
      } else if (output->storage_type() == kRowSparseStorage) {
        FillZerosRspImpl<xpu>(s, output);
      } else {
        LOG(FATAL) << "SquareSumRspImpl only supports row-sparse/dense output storage type";
      }
    }
    return;
  }

  if (output->storage_type() == kRowSparseStorage) {
    output->CheckAndAlloc({input.aux_shape(rowsparse::kIdx)});
  }
  const TBlob& out_data = output->data();
  const int64_t nnr = input.storage_shape()[0];
  const int64_t num_cols = input.storage_shape()[1];
  const TBlob& in_data = input.data();
  if (0 == param.axis[0]) {  // axis = 0, output is dense
    MSHADOW_TYPE_SWITCH(out_data.type_flag_, DType, {
      MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
        Kernel<SquareSumRspKernel<req_type, 0, false>, xpu>::Launch(s, num_cols,
            out_data.dptr<DType>(), input.data().dptr<DType>(), nnr, num_cols);
      })
    })
  } else {  // axis = 1
    const TBlob in_row_idx = input.aux_data(rowsparse::kIdx);
    if (param.keepdims) {  // output is rsp
      const TBlob out_row_idx = output->aux_data(rowsparse::kIdx);
      MSHADOW_TYPE_SWITCH(out_data.type_flag_, DType, {
        MSHADOW_IDX_TYPE_SWITCH(in_row_idx.type_flag_, IType, {
          MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
            Kernel<SquareSumRspKernel<req_type, 1, true>, xpu>::Launch(s, nnr,
                out_row_idx.dptr<IType>(), out_data.dptr<DType>(), in_row_idx.dptr<IType>(),
                in_data.dptr<DType>(), num_cols);
          })
        })
      })
    } else {  // output is dense
      if (req == kWriteTo) {
        MSHADOW_TYPE_SWITCH(out_data.type_flag_, DType, {
          Kernel<set_zero, xpu>::Launch(s, out_data_size, out_data.dptr<DType>());
        })
      }
      MSHADOW_TYPE_SWITCH(out_data.type_flag_, DType, {
        MSHADOW_IDX_TYPE_SWITCH(in_row_idx.type_flag_, IType, {
          MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
            Kernel<SquareSumRspKernel<req_type, 1, false>, xpu>::Launch(s, nnr,
                out_data.dptr<DType>(), in_row_idx.dptr<IType>(), in_data.dptr<DType>(), num_cols);
          })
        })
      })
    }
  }
}

template<typename xpu>
void SquareSumRspGradImpl(const nnvm::NodeAttrs& attrs,
                          mshadow::Stream<xpu>* s,
                          const NDArray& ograd,
                          const NDArray& input,
                          const OpReqType req,
                          NDArray* igrad) {
  if (req == kNullOp) return;
  const ReduceAxesParam& param = nnvm::get<ReduceAxesParam>(attrs.parsed);
  CHECK_EQ(param.axis.ndim(), 1U) << "_square_sum(row_sparse_matrix) only supports axis=0/1";
  CHECK(param.axis[0] == 0 || param.axis[0] == 1)
    << "_square_sum(row_sparse_matrix) only supports axis=0 or 1";
  CHECK(ograd.storage_type() == kDefaultStorage || ograd.storage_type() == kRowSparseStorage);
  CHECK_EQ(input.storage_type(), kRowSparseStorage);
  CHECK_EQ(igrad->storage_type(), kRowSparseStorage);
  CHECK_EQ(req, kWriteTo);
  if (!input.storage_initialized()) {
    FillZerosRspImpl<xpu>(s, igrad);
    return;
  }

  using namespace mxnet_op;
  // TODO(junwu) change the input of CheckAndAlloc
  // if we want to support differen row idx arrays
  // for ograd and input when they are both row-sparse ndarrays
  igrad->CheckAndAlloc({input.aux_shape(rowsparse::kIdx)});
  const int64_t num_cols = input.storage_shape()[1];
  const TBlob& igrad_data = igrad->data();
  const TBlob igrad_row_idx = igrad->aux_data(rowsparse::kIdx);
  const TBlob& ograd_data = ograd.data();
  const TBlob& in_data = input.data();
  const TBlob in_row_idx = input.aux_data(rowsparse::kIdx);
  if (ograd.storage_type() == kDefaultStorage) {
    if (0 == param.axis[0]) {  // forward is sum per column
      MSHADOW_TYPE_SWITCH(igrad_data.type_flag_, DType, {
        MSHADOW_IDX_TYPE_SWITCH(igrad_row_idx.type_flag_, IType, {
          MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
            Kernel<SquareSumRspGradKernel<req_type, 0, kDefaultStorage>, xpu>::Launch(
                s, igrad_data.Size(), igrad_row_idx.dptr<IType>(),
                igrad_data.dptr<DType>(), ograd_data.dptr<DType>(),
                in_row_idx.dptr<IType>(), in_data.dptr<DType>(), num_cols);
          })
        })
      })
    } else {  // forward is sum per row
      MSHADOW_TYPE_SWITCH(igrad_data.type_flag_, DType, {
        MSHADOW_IDX_TYPE_SWITCH(igrad_row_idx.type_flag_, IType, {
          MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
            Kernel<SquareSumRspGradKernel<req_type, 1, kDefaultStorage>, xpu>::Launch(
                s, igrad_data.Size(), igrad_row_idx.dptr<IType>(),
                igrad_data.dptr<DType>(), ograd_data.dptr<DType>(),
                in_row_idx.dptr<IType>(), in_data.dptr<DType>(), num_cols);
          })
        })
      })
    }
  } else if (ograd.storage_type() == kRowSparseStorage) {
    CHECK_EQ(1, param.axis[0]) << "SquareSumRspGradImpl only supports axis = 1"
                                   " when ograd_stype = kRowSparseStorage";
    CHECK_EQ(ograd.shape().ndim(), 2U);
    const TBlob ograd_row_idx = ograd.aux_data(rowsparse::kIdx);
    CHECK(ograd_row_idx.Size() == in_row_idx.Size() || in_row_idx.Size() == in_data.shape_[0]);
    MSHADOW_IDX_TYPE_SWITCH(igrad_row_idx.type_flag_, IType, {
      if (std::is_same<xpu, cpu>::value) {
        const IType* first1 = ograd_row_idx.dptr<IType>();
        const IType* last1 = first1 + ograd_row_idx.Size();
        const IType* first2 = in_row_idx.dptr<IType>();
        // when ograd_row_idx and in_row_idx have the same size and input is not a full rsp
        // ograd_row_idx and in_row_idx are expected to have the same elements
        if (ograd_row_idx.Size() == in_row_idx.Size() && in_row_idx.Size() != in_data.shape_[0]) {
          CHECK(std::equal(first1, last1, first2)) << "SquareSumRspGradImpl only supports"
                                                      " equal ograd_row_idx and input_row_idx"
                                                      " when ograd and input are both"
                                                      " row-sparse";
        }
      } else {
        LOG(FATAL) << "SquareSumRspGradImpl has not implemented GPU version when"
                      " ograd and input are both row-sparse";
      }
      MSHADOW_TYPE_SWITCH(igrad_data.type_flag_, DType, {
        MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
          Kernel<SquareSumRspGradKernel<req_type, 1, kRowSparseStorage>, xpu>::Launch(
              s, igrad_data.Size(), igrad_row_idx.dptr<IType>(),
              igrad_data.dptr<DType>(), ograd_row_idx.dptr<IType>(),
              ograd_data.dptr<DType>(), in_data.dptr<DType>(), num_cols);
        })
      })
    })
  } else {
    LOG(FATAL) << "SquareSumRspGradImpl only supports ograd_stype"
               << " = kDefaultStorage/kRowSparseStorage";
  }
}

template<typename xpu>
void SquareSumOpForwardEx(const nnvm::NodeAttrs& attrs,
                          const OpContext& ctx,
                          const std::vector<NDArray>& inputs,
                          const std::vector<OpReqType>& req,
                          const std::vector<NDArray>& outputs) {
  CHECK_EQ(inputs.size(), 1U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(req.size(), 1U);
  mshadow::Stream<xpu>* s = ctx.get_stream<xpu>();
  const NDArrayStorageType istype = inputs[0].storage_type();
  if (istype == kRowSparseStorage) {
    CHECK_EQ(inputs[0].shape().ndim(), 2U) << "_square_sum op only supports"
                                              " 2D ndarray as input";
    NDArray output = outputs[0];
    SquareSumRspImpl(attrs, s, inputs[0], req[0], &output);
  } else {
    LOG(FATAL) << "_square_sum op only supports row-sparse ndarray"
                  " as input, while input stype = "
               << istype;
  }
}

template<typename xpu>
void SquareSumOpBackwardEx(const nnvm::NodeAttrs& attrs,
                           const OpContext& ctx,
                           const std::vector<NDArray>& inputs,
                           const std::vector<OpReqType>& req,
                           const std::vector<NDArray>& outputs) {
  CHECK_EQ(inputs.size(), 2U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(req.size(), 1U);
  mshadow::Stream<xpu>* s = ctx.get_stream<xpu>();
  const NDArrayStorageType ograd_stype = inputs[0].storage_type();
  const NDArrayStorageType input_stype = inputs[1].storage_type();
  if (input_stype == kRowSparseStorage
      && (ograd_stype == kDefaultStorage || ograd_stype == kRowSparseStorage)) {
    CHECK_EQ(inputs[1].shape().ndim(), 2U) << "_square_sum op only supports"
                                              " 2D ndarray as input";
    NDArray output = outputs[0];
    SquareSumRspGradImpl(attrs, s, inputs[0], inputs[1], req[0], &output);
  } else {
    LOG(FATAL) << "_square_sum op backward only supports dense ndarray as ograd,"
                  " row-sparse ndarray as input and row-sparse ndarray as igrad,"
                  " while ograd_stype = " << ograd_stype
               << " input_stype = " << input_stype;
  }
}

}  // namespace op
}  // namespace mxnet

#endif  // MXNET_OPERATOR_TENSOR_SQUARE_SUM_INL_H_
