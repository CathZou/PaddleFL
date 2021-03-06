// Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <memory>
#include <algorithm>

#include "paddle/fluid/platform/enforce.h"
#include "../common/prng.h"
#include "core/common/paddle_tensor.h"

namespace privc {

template<typename T, size_t N>
FixedPointTensor<T, N>::FixedPointTensor(TensorAdapter<T>* share_tensor) {
    _share = share_tensor;
}

template<typename T, size_t N>
TensorAdapter<T>* FixedPointTensor<T, N>::mutable_share() {
    return _share;
}

template<typename T, size_t N>
const TensorAdapter<T>* FixedPointTensor<T, N>::share() const {
    return _share;
}

// reveal fixedpointtensor to one party
template<typename T, size_t N>
void FixedPointTensor<T, N>::reveal_to_one(size_t party,
                                           TensorAdapter<T>* ret) const {

    if (party == privc::party()) {
        auto buffer = tensor_factory()->template create<T>(ret->shape());
        privc_ctx()->network()->template recv(next_party(), *buffer);

        share()->add(buffer.get(), ret);
        ret->scaling_factor() = N;
    } else {
        privc_ctx()->network()->template send(party, *share());
    }
}

// reveal fixedpointtensor to all parties
template<typename T, size_t N>
void FixedPointTensor<T, N>::reveal(TensorAdapter<T>* ret) const {
    for (size_t i = 0; i < 2; ++i) {
        reveal_to_one(i, ret);
    }
}

template<typename T, size_t N>
const std::vector<size_t> FixedPointTensor<T, N>::shape() const {
    return _share->shape();
}

//convert TensorAdapter to shares
template<typename T, size_t N>
void FixedPointTensor<T, N>::share(const TensorAdapter<T>* input,
                                    TensorAdapter<T>* output_shares[2],
                                    block seed) {

    if (common::equals(seed, common::g_zero_block)) {
        seed = common::block_from_dev_urandom();
    }
    //set seed of prng[2]
    privc_ctx()->set_random_seed(seed, 2);

    privc_ctx()->template gen_random_private(*output_shares[0]);

    input->sub(output_shares[0], output_shares[1]);
    for (int i = 0; i < 2; ++i) {
        output_shares[i]->scaling_factor() = input->scaling_factor();
    }
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::add(const FixedPointTensor<T, N>* rhs,
                                FixedPointTensor<T, N>* ret) const {
    _share->add(rhs->_share, ret->_share);
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::add(const TensorAdapter<T>* rhs,
                                FixedPointTensor<T, N>* ret) const {
    if (party() == 0) {
        _share->add(rhs, ret->_share);
    } else {
        _share->copy(ret->_share);
    }
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::sub(const FixedPointTensor<T, N>* rhs,
                                FixedPointTensor<T, N>* ret) const {
    _share->sub(rhs->_share, ret->_share);
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::sub(const TensorAdapter<T>* rhs,
                                FixedPointTensor<T, N>* ret) const {
    if (party() == 0) {
        _share->sub(rhs, ret->_share);
    } else {
        _share->copy(ret->_share);
    }
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::negative(FixedPointTensor<T, N>* ret) const {
    _share->negative(ret->_share);
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::exp(FixedPointTensor<T, N>* ret,
                                 size_t iter) const {
    // exp approximate: exp(x) = \lim_{n->inf} (1+x/n)^n
    // where n = 2^ite
    auto pow_iter = tensor_factory()->template create<T>(this->shape());
    common::assign_to_tensor(pow_iter.get(), (T) (pow(2, N - iter)));
    pow_iter->scaling_factor() = N;

    auto tensor_one = tensor_factory()->template create<T>(this->shape());
    common::assign_to_tensor(tensor_one.get(), (T) 1 << N);
    tensor_one->scaling_factor() = N;

    this->mul(pow_iter.get(), ret);

    ret->add(tensor_one.get(), ret);

    for (int i = 0; i < iter; ++i) {
        ret->mul(ret, ret);
    }
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::mul(const FixedPointTensor<T, N>* rhs,
                                 FixedPointTensor<T, N>* ret) const {
    auto shape128 = shape();
    shape128.insert(shape128.begin(), 2);
    auto trip_shape = shape();
    trip_shape.insert(trip_shape.begin(), 3);

    auto triplet = tensor_factory()->template create<T>(trip_shape);
    tripletor()->get_triplet(triplet.get());

    // parser triplet
    std::vector<std::shared_ptr<TensorAdapter<T>>> triplets;
    for (int i = 0; i < 3; ++i) {
        triplets.emplace_back(tensor_factory()->template create<T>(shape()));
        triplet->slice(i, i + 1, triplets[i].get());
        triplets[i]->reshape(shape());
    }

    // calc <e> and <f>
    auto share_e = tensor_factory()
                      ->template create<T>(shape128);
    auto share_f = tensor_factory()
                      ->template create<T>(shape128);
    this->share()->sub128(triplets[0].get(), share_e.get(), false, false);
    rhs->share()->sub128(triplets[1].get(), share_f.get(), false, false);

    // reconstruct  e, f
    auto shape_e_f = shape128;
    shape_e_f.insert(shape_e_f.begin(), 2);
    auto share_e_f = tensor_factory()
                      ->template create<T>(std::vector<size_t>(shape_e_f));
    auto remote_share_e_f = tensor_factory()
                      ->template create<T>(std::vector<size_t>(shape_e_f));
    std::copy(share_e->data(), share_e->data() + share_e->numel(),
              share_e_f->data());
    std::copy(share_f->data(), share_f->data() + share_f->numel(),
              share_e_f->data() + share_e->numel());
    if (party() == 0) {
      net()->template send(next_party(), *share_e_f);
      net()->template recv(next_party(), *remote_share_e_f);
    } else {
      net()->template recv(next_party(), *remote_share_e_f);
      net()->template send(next_party(), *share_e_f);
    }
    auto& e_and_f = share_e_f;
    share_e_f->add128(remote_share_e_f.get(), e_and_f.get(), true, true);

    auto e = tensor_factory()
                ->template create<T>(shape128);
    auto f = tensor_factory()
                ->template create<T>(shape128);
    
    
    e_and_f->slice(0, 1, e.get());
    e_and_f->slice(1, 2, f.get());
    e->reshape(shape128);
    f->reshape(shape128);

    // calc z = f<a> + e<b> + <c> or z = ef + f<a> + e<b> + <c>
    auto z = tensor_factory()
                ->template create<T>(shape());

    f->scaling_factor() = N;
    f->mul128_with_truncate(triplets[0].get(), z.get(), true, false);
    auto eb = tensor_factory()
                ->template create<T>(shape());

    e->scaling_factor() = N;
    e->mul128_with_truncate(triplets[1].get(), eb.get(), true, false);
    z->add(eb.get(), z.get());
    z->add(triplets[2].get(), z.get());
    if (party() == 0) {
        auto ef = tensor_factory()
                ->template create<T>(shape());
        e->mul128_with_truncate(f.get(), ef.get(), true, true);
        z->add(ef.get(), z.get());
    }
    z->copy(ret->mutable_share());
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::mul(const TensorAdapter<T>* rhs,
                                 FixedPointTensor<T, N>* ret) const {
    fixed64_tensor_mult<N>(share(), rhs, ret->mutable_share());
}

template< typename T, size_t N>
void FixedPointTensor<T, N>::div(const TensorAdapter<T>* rhs,
                                 FixedPointTensor<T, N>* ret) const {

    auto temp = tensor_factory()->template create<T>(this->shape());

    double scale = std::pow(2, N);
    auto inverse = [scale](T d) -> T {
                    return 1.0 * scale / d * scale; };
    std::transform(rhs->data(), rhs->data() + rhs->numel(),
                                temp->data(), inverse);

    this->mul(temp.get(), ret);
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::sum(FixedPointTensor* ret) const {
    PADDLE_ENFORCE_EQ(ret->numel(), 1, "output size should be 1.");
    T sum = (T) 0;
    for (int i = 0; i < numel(); ++i) {
        sum += *(share()->data() + i);
    }
    *(ret->mutable_share()->data()) = sum;
}

template<typename T, size_t N>
void FixedPointTensor<T, N>::mat_mul(const FixedPointTensor<T, N>* rhs,
                                 FixedPointTensor<T, N>* ret) const {
    // A dot B, assume A.shape = [a, b], B.shape = [b, c]
    // expand A and B to shape [a, c, b], and element-wise cal A * B
    // then reduce result shape to [a, c]
    size_t a = ret->shape()[0];
    size_t b = shape()[1];
    size_t c = ret->shape()[1];
    std::vector<size_t> expand_shape({a, c, b});
    std::vector<size_t> expand128_shape({2, a, c, b});
    PADDLE_ENFORCE_EQ(a, shape()[0], "invalid result shape for mat mul");
    PADDLE_ENFORCE_EQ(c, rhs->shape()[1], "invalid result shape for mat mul");
    PADDLE_ENFORCE_EQ(shape()[1], rhs->shape()[0], "invalid input shape for mat mul");

    // equal to shape = [3, a, c, b] triplet
    std::vector<size_t> triplet_shape{3, a, c, b};
    auto triplet = tensor_factory()->template create<T>(triplet_shape);
    tripletor()->get_triplet(triplet.get());

    // parser triplet
    std::vector<std::shared_ptr<TensorAdapter<T>>> triplets;
    for (int i = 0; i < 3; ++i) {
        triplets.emplace_back(tensor_factory()->template create<T>(expand_shape));
        triplet->slice(i, i + 1, triplets[i].get());
        triplets[i]->reshape(expand_shape);
    }

    // expand lhs
    auto lhs_tile = tensor_factory()
                      ->template create<T>(std::vector<size_t>({c, a, b}));
    for (int i = 0; i < c; ++i) {
        std::copy(share()->data(), share()->data() + numel(),
                  lhs_tile->data() + i * numel());
    }
    //auto shape_trans = std::vector<size_t>({a, c, b});
    auto lhs_expand = tensor_factory()->template create<T>(expand_shape);
    std::dynamic_pointer_cast<common::PaddleTensor<T>>(lhs_tile)
          ->template Transpose<3>(std::vector<int>({1, 0, 2}), lhs_expand.get());
    
    // expand rhs
    auto rhs_expand = tensor_factory()
                      ->template create<T>(expand_shape);
    auto rhs_tile = tensor_factory()
                      ->template create<T>(std::vector<size_t>({c, b}));
    const common::PaddleTensor<T>* p_rhs = dynamic_cast<const common::PaddleTensor<T>*>(rhs->share());
    const_cast<common::PaddleTensor<T>*>(p_rhs)->template Transpose<2>(std::vector<int>({1, 0}), rhs_tile.get());
    for (int i = 0; i < a; ++i) {
        std::copy(rhs_tile->data(), rhs_tile->data() + rhs_tile->numel(),
                  rhs_expand->data() + i * rhs_tile->numel());
    }

    // calc <e> and <f>
    auto share_e = tensor_factory()
                      ->template create<T>(expand128_shape);
    auto share_f = tensor_factory()
                      ->template create<T>(expand128_shape);
    lhs_expand->sub128(triplets[0].get(), share_e.get(), false, false);
    rhs_expand->sub128(triplets[1].get(), share_f.get(), false, false);

    // reconstruct  e, f
    auto share_e_f = tensor_factory()
                      ->template create<T>(std::vector<size_t>({4, a, c, b}));
    auto remote_share_e_f = tensor_factory()
                      ->template create<T>(std::vector<size_t>({4, a, c, b}));
    std::copy(share_e->data(), share_e->data() + share_e->numel(),
              share_e_f->data());
    std::copy(share_f->data(), share_f->data() + share_f->numel(),
              share_e_f->data() + share_e->numel());
    if (party() == 0) {
      net()->template send(next_party(), *share_e_f);
      net()->template recv(next_party(), *remote_share_e_f);
    } else {
      net()->template recv(next_party(), *remote_share_e_f);
      net()->template send(next_party(), *share_e_f);
    }
    auto& e_and_f = share_e_f;
    share_e_f->add128(remote_share_e_f.get(), e_and_f.get(), true, true);

    auto e = tensor_factory()
                ->template create<T>(expand128_shape);
    auto f = tensor_factory()
                ->template create<T>(expand128_shape);
    
    
    e_and_f->slice(0, 2, e.get());
    e_and_f->slice(2, 4, f.get());

    // calc z = f<a> + e<b> + <c> or z = ef + f<a> + e<b> + <c>
    auto z = tensor_factory()
                ->template create<T>(expand_shape);

    f->scaling_factor() = N;
    f->mul128_with_truncate(triplets[0].get(), z.get(), true, false);
    auto eb = tensor_factory()
                ->template create<T>(expand_shape);

    e->scaling_factor() = N;
    e->mul128_with_truncate(triplets[1].get(), eb.get(), true, false);
    z->add(eb.get(), z.get());
    z->add(triplets[2].get(), z.get());
    if (party() == 0) {
        auto ef = tensor_factory()
                ->template create<T>(expand_shape);
        e->mul128_with_truncate(f.get(), ef.get(), true, true);
        z->add(ef.get(), z.get());
    }

    // reduce expand shape (a, c, b) to (a, c)
    auto ret_ptr = ret->mutable_share()->data();
    for (int i = 0; i < a * c; ++i) {
        T trunc_sum = 0;
        T* z_ptr = z->data();
        std::for_each(z_ptr + i * b, z_ptr + (i + 1) * b,
                   [&trunc_sum] (T n) {
                        trunc_sum += n;
                    });
        *(ret_ptr + i) = trunc_sum;
    }
}

} // namespace privc
