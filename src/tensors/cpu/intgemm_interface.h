#pragma once

#include "graph/node.h"
#include "graph/node_operators_unary.h"
#include "3rd_party/intgemm/intgemm.h"

namespace marian {

namespace { //Convenient function to get rows and columns of a tensor, shadowed by namespace.
  inline int cols(Tensor& tensor) { return tensor->shape()[-1]; }
  inline int rows(Tensor& tensor) { return tensor->shape().elements() / cols(tensor); }
}

namespace cpu {
namespace int8 {

//@TODO template it for A, B 8bit and 16bit
struct QuantMultNodeOp : public UnaryNodeOp {
  QuantMultNodeOp(Expr input) : UnaryNodeOp(input, Shape({1}), Type::float32) {}

  NodeOps forwardOps() override {
    return {NodeOp(
      *val_->data() = 127.0f / intgemm::MaxAbsolute(child(0)->val()->data(),
                         child(0)->val()->data() + child(0)->val()->shape().elements());
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  const std::string type() override { return "quantMult"; }
};

struct PrepareANodeOp : public NaryNodeOp {
float clipValue_;
float quantMult_;
  PrepareANodeOp(Expr input, Expr quant_mult, float clipValue)
      : NaryNodeOp({input, quant_mult}, input->shape(), Type::int8), clipValue_{clipValue} {
    ABORT_IF(children().size() != 2, "expected 2 children");

    // Check if arguments are not null
    ABORT_IF(child(0) == nullptr, "A cannot be null");
    ABORT_IF(child(1) == nullptr, "Quant mult of A cannot be null");
  }

  NodeOps forwardOps() override {
    return {NodeOp(
      quantMult_ = *child(1)->val()->data();
      intgemm::Int8::PrepareA(child(0)->val()->data(), /*input*/
                              val_->data<int8_t>(), /*output*/
                              *child(1)->val()->data(), /*Quant Mult*/
                              rows(child(0)->val()),
                              cols(child(0)->val()));
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  const std::string type() override { return "int8PrepareA"; }
};

struct PrepareBNodeOp : public NaryNodeOp {
float clipValue_;
float quantMult_;

  PrepareBNodeOp(Expr input, Expr quant_mult, float clipValue)
      : NaryNodeOp({input, quant_mult}, input->shape(), Type::int8), clipValue_{clipValue} {
    ABORT_IF(children().size() != 2, "expected 2 children");

    // Check if arguments are not null
    ABORT_IF(child(0) == nullptr, "A cannot be null");
    ABORT_IF(child(1) == nullptr, "Quant mult of B cannot be null");
  }

  NodeOps forwardOps() override {
    //float qm = *(reinterpret_cast<float *>(child(1)->val()->data<int8_t>() + child(1)->val()->shape().elements() + 1));
    //std::cerr << "Argument name: " << this->name() << " type " << this->type() << " QuantMult: " << qm << std::endl;
    return {NodeOp(
      quantMult_ = *child(1)->val()->data();
      intgemm::Int8::PrepareB(child(0)->val()->data(), /*input*/
                                val_->data<int8_t>(), /*output*/
                                *child(1)->val()->data(), /*Quant Mult*/
                                rows(child(0)->val()),
                                cols(child(0)->val()));
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  const std::string type() override { return "int8PrepareB"; }
};

struct SelectColumnsBNodeOp : public UnaryNodeOp {
public:
  float clipValue_;
  float quantMult_;
  SelectColumnsBNodeOp(Expr input, const std::vector<uint_least32_t>  &indices)
      : UnaryNodeOp(input, newShape(input, indices), Type::int8), indices_(indices) {
    // Check if arguments are not null
    ABORT_IF(child(0) == nullptr, "B cannot be null");

    // Check number of selected columns
    // @TODO remove asserts
    assert(indices.size() % 8 == 0);
  }

  NodeOps forwardOps() override {
    return {NodeOp(
      //We get the quantization multiplier from a PrepareB
      auto bPreppedNode = std::static_pointer_cast<PrepareBNodeOp>(child(0));
      clipValue_ = bPreppedNode->clipValue_;
      quantMult_ = bPreppedNode->quantMult_;
      auto input = child(0)->val();
      intgemm::Int8::SelectColumnsB(
          input->data<int8_t>(),
          val_->data<int8_t>(),
          rows(input),
          &*indices_.begin(),
          &*indices_.end());
    )};
  }

  const std::string type() override { return "int8SelectColumnsB"; }

  /* The only point of caching shortlists is if we have the same sentence(s) over and over again.
   * Since this is not a realistic scenario, disable hashing and matching by always returning false */
  size_t hash() override {return 0;}

  bool equal(Expr node) override {return false;}

private:
  static Shape newShape(Expr a, const std::vector<uint_least32_t>& indices) {
    Shape ret = a->shape();
    ret.set(1, indices.size());
    return ret;
  }

  std::vector<uint_least32_t> indices_;
};

class DotNodeOp : public NaryNodeOp {
private:
float scalar_;

public:
  DotNodeOp(Expr a, Expr b, float scalar)
      : NaryNodeOp({a, b}, newShape(a, b), Type::float32), scalar_(scalar) {}

  Shape newShape(Expr a, Expr b) {
    Shape result = a->shape();
    result.set(-1, b->shape()[-1]);
    return result;
  }
  /*
  Shape newShape(Expr a, Expr b) {
    auto shapeA = a->shape();
    auto shapeB = b->shape();

    // Computing A * B^T
    shapeB.set(-2, b->shape()[-1]);
    shapeB.set(-1, b->shape()[-2]);

    Shape outShape = shapeA;
    outShape.set(-1, shapeB[-1]);
    ABORT_IF(shapeA[-1] != shapeB[-2],
             "matrix product requires dimensions to match");
    return outShape;
  }
*/
  NodeOps forwardOps() override {
    return {NodeOp(
          float aQuantMult = std::static_pointer_cast<PrepareANodeOp>(child(0))->quantMult_;
          float bQuantMult;
          if (child(1)->type() == "int8SelectColumnsB") {
            bQuantMult = std::static_pointer_cast<SelectColumnsBNodeOp>(child(1))->quantMult_;
          } else {
            bQuantMult = std::static_pointer_cast<PrepareBNodeOp>(child(1))->quantMult_;
          }
          float unquant_mult = 1.0f/(aQuantMult*bQuantMult);

          unquant_mult = unquant_mult*scalar_;
          intgemm::Int8::Multiply(child(0)->val()->data<int8_t>(), /*A*/
                                  child(1)->val()->data<int8_t>(), /*B*/
                                   rows(child(0)->val()),
                                   cols(child(0)->val()),
                                   cols(child(1)->val()),
                                   intgemm::callbacks::UnquantizeAndWrite(unquant_mult, val_->data()));
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  const std::string type() override { return "dotInt8"; }
};

class AffineNodeOp : public NaryNodeOp {
private:
  float scalar_;

public:
  AffineNodeOp(Expr a, Expr b, Expr Bias, float scalar)
      : NaryNodeOp({a, b, Bias}, newShape(a, b), Type::float32), scalar_(scalar) {}

  Shape newShape(Expr a, Expr b) {
    Shape result = a->shape();
    result.set(-1, b->shape()[-1]);
    return result;
  }
/*
  Shape newShape(Expr a, Expr b) {
    auto shapeA = a->shape();
    auto shapeB = b->shape();

    // Computing A * B^T
    shapeB.set(-2, b->shape()[-1]);
    shapeB.set(-1, b->shape()[-2]);

    Shape outShape = shapeA;
    outShape.set(-1, shapeB[-1]);
    ABORT_IF(shapeA[-1] != shapeB[-2],
             "matrix product requires dimensions to match");
    return outShape;
  }*/

  NodeOps forwardOps() override {
    return {NodeOp(
          float aQuantMult = std::static_pointer_cast<PrepareANodeOp>(child(0))->quantMult_;
          float bQuantMult;
          if (child(1)->type() == "int8SelectColumnsB") {
            bQuantMult = std::static_pointer_cast<SelectColumnsBNodeOp>(child(1))->quantMult_;
          } else {
            bQuantMult = std::static_pointer_cast<PrepareBNodeOp>(child(1))->quantMult_;
          }
          float unquant_mult = 1.0f/(aQuantMult*bQuantMult);

          unquant_mult = unquant_mult*scalar_;
          intgemm::Int8::Multiply(child(0)->val()->data<int8_t>(), /*A*/
                                  child(1)->val()->data<int8_t>(), /*B*/
                                   rows(child(0)->val()),
                                   cols(child(0)->val()),
                                   cols(child(1)->val()),                                          /*child(2) is bias*/
                                   intgemm::callbacks::UnquantizeAndAddBiasAndWrite(unquant_mult, child(2)->val()->data(), val_->data()));
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  const std::string type() override { return "affineInt8"; }
};

static inline Expr dot(Expr a, Expr b, float scalar) {
  return Expression<cpu::int8::DotNodeOp>(a, b, scalar);
}

static inline Expr affine(Expr a, Expr b, Expr c, float scalar) {
  return Expression<cpu::int8::AffineNodeOp>(a, b, c, scalar);
}

static inline Expr quantMult(Expr a) {
  return Expression<cpu::int8::QuantMultNodeOp>(a);
}

static inline Expr prepareA(Expr a, Expr quantMult, float clipValue) {
  return Expression<cpu::int8::PrepareANodeOp>(a, quantMult, clipValue);
}

static inline Expr prepareB(Expr b, Expr quantMult, float clipValue) {
  return Expression<cpu::int8::PrepareBNodeOp>(b, quantMult, clipValue);
}

static inline Expr selectColumnsB(Expr b, const std::vector<uint_least32_t> &cols) {
  return Expression<SelectColumnsBNodeOp>(b, cols);
}

}  // namespace int8
}  // namespace cpu
}  // namespace marian
