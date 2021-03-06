//===-- X86ShuffleDecodeConstantPool.cpp - X86 shuffle decode -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Define several functions to decode x86 specific shuffle semantics using
// constants from the constant pool.
//
//===----------------------------------------------------------------------===//

#include "X86ShuffleDecodeConstantPool.h"
#include "Utils/X86ShuffleDecode.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/CodeGen/MachineValueType.h"
#include "llvm/IR/Constants.h"

//===----------------------------------------------------------------------===//
//  Vector Mask Decoding
//===----------------------------------------------------------------------===//

namespace llvm {

static bool extractConstantMask(const Constant *C, unsigned MaskEltSizeInBits,
                                SmallBitVector &UndefElts,
                                SmallVectorImpl<uint64_t> &RawMask) {
  // It is not an error for shuffle masks to not be a vector of
  // MaskEltSizeInBits because the constant pool uniques constants by their
  // bit representation.
  // e.g. the following take up the same space in the constant pool:
  //   i128 -170141183420855150465331762880109871104
  //
  //   <2 x i64> <i64 -9223372034707292160, i64 -9223372034707292160>
  //
  //   <4 x i32> <i32 -2147483648, i32 -2147483648,
  //              i32 -2147483648, i32 -2147483648>
  Type *CstTy = C->getType();
  if (!CstTy->isVectorTy())
    return false;

  Type *CstEltTy = CstTy->getVectorElementType();
  if (!CstEltTy->isIntegerTy())
    return false;

  unsigned CstSizeInBits = CstTy->getPrimitiveSizeInBits();
  unsigned CstEltSizeInBits = CstTy->getScalarSizeInBits();
  unsigned NumCstElts = CstTy->getVectorNumElements();

  // Extract all the undef/constant element data and pack into single bitsets.
  APInt UndefBits(CstSizeInBits, 0);
  APInt MaskBits(CstSizeInBits, 0);
  for (unsigned i = 0; i != NumCstElts; ++i) {
    Constant *COp = C->getAggregateElement(i);
    if (!COp || (!isa<UndefValue>(COp) && !isa<ConstantInt>(COp)))
      return false;

    if (isa<UndefValue>(COp)) {
      APInt EltUndef = APInt::getLowBitsSet(CstSizeInBits, CstEltSizeInBits);
      UndefBits |= EltUndef.shl(i * CstEltSizeInBits);
      continue;
    }

    APInt EltBits = cast<ConstantInt>(COp)->getValue();
    EltBits = EltBits.zextOrTrunc(CstSizeInBits);
    MaskBits |= EltBits.shl(i * CstEltSizeInBits);
  }

  // Now extract the undef/constant bit data into the raw shuffle masks.
  assert((CstSizeInBits % MaskEltSizeInBits) == 0 &&
         "Unaligned shuffle mask size");

  unsigned NumMaskElts = CstSizeInBits / MaskEltSizeInBits;
  UndefElts = SmallBitVector(NumMaskElts, false);
  RawMask.resize(NumMaskElts, 0);

  for (unsigned i = 0; i != NumMaskElts; ++i) {
    APInt EltUndef = UndefBits.lshr(i * MaskEltSizeInBits);
    EltUndef = EltUndef.zextOrTrunc(MaskEltSizeInBits);

    // Only treat the element as UNDEF if all bits are UNDEF, otherwise
    // treat it as zero.
    if (EltUndef.isAllOnesValue()) {
      UndefElts[i] = true;
      RawMask[i] = 0;
      continue;
    }

    APInt EltBits = MaskBits.lshr(i * MaskEltSizeInBits);
    EltBits = EltBits.zextOrTrunc(MaskEltSizeInBits);
    RawMask[i] = EltBits.getZExtValue();
  }

  return true;
}

void DecodePSHUFBMask(const Constant *C, SmallVectorImpl<int> &ShuffleMask) {
  Type *MaskTy = C->getType();
  unsigned MaskTySize = MaskTy->getPrimitiveSizeInBits();
  (void)MaskTySize;
  assert((MaskTySize == 128 || MaskTySize == 256 || MaskTySize == 512) &&
         "Unexpected vector size.");

  // The shuffle mask requires a byte vector.
  SmallBitVector UndefElts;
  SmallVector<uint64_t, 32> RawMask;
  if (!extractConstantMask(C, 8, UndefElts, RawMask))
    return;

  unsigned NumElts = RawMask.size();
  assert((NumElts == 16 || NumElts == 32 || NumElts == 64) &&
         "Unexpected number of vector elements.");

  for (unsigned i = 0; i != NumElts; ++i) {
    if (UndefElts[i]) {
      ShuffleMask.push_back(SM_SentinelUndef);
      continue;
    }

    uint64_t Element = RawMask[i];
    // If the high bit (7) of the byte is set, the element is zeroed.
    if (Element & (1 << 7))
      ShuffleMask.push_back(SM_SentinelZero);
    else {
      // For AVX vectors with 32 bytes the base of the shuffle is the 16-byte
      // lane of the vector we're inside.
      unsigned Base = i & ~0xf;

      // Only the least significant 4 bits of the byte are used.
      int Index = Base + (Element & 0xf);
      ShuffleMask.push_back(Index);
    }
  }
}

void DecodeVPERMILPMask(const Constant *C, unsigned ElSize,
                        SmallVectorImpl<int> &ShuffleMask) {
  Type *MaskTy = C->getType();
  unsigned MaskTySize = MaskTy->getPrimitiveSizeInBits();
  (void)MaskTySize;
  assert((MaskTySize == 128 || MaskTySize == 256 || MaskTySize == 512) &&
         "Unexpected vector size.");
  assert((ElSize == 32 || ElSize == 64) && "Unexpected vector element size.");

  // The shuffle mask requires elements the same size as the target.
  SmallBitVector UndefElts;
  SmallVector<uint64_t, 8> RawMask;
  if (!extractConstantMask(C, ElSize, UndefElts, RawMask))
    return;

  unsigned NumElts = RawMask.size();
  unsigned NumEltsPerLane = 128 / ElSize;
  assert((NumElts == 2 || NumElts == 4 || NumElts == 8 || NumElts == 16) &&
         "Unexpected number of vector elements.");

  for (unsigned i = 0; i != NumElts; ++i) {
    if (UndefElts[i]) {
      ShuffleMask.push_back(SM_SentinelUndef);
      continue;
    }

    int Index = i & ~(NumEltsPerLane - 1);
    uint64_t Element = RawMask[i];
    if (ElSize == 64)
      Index += (Element >> 1) & 0x1;
    else
      Index += Element & 0x3;

    ShuffleMask.push_back(Index);
  }
}

void DecodeVPERMIL2PMask(const Constant *C, unsigned M2Z, unsigned ElSize,
                         SmallVectorImpl<int> &ShuffleMask) {
  Type *MaskTy = C->getType();
  unsigned MaskTySize = MaskTy->getPrimitiveSizeInBits();
  (void)MaskTySize;
  assert((MaskTySize == 128 || MaskTySize == 256) && "Unexpected vector size.");

  // The shuffle mask requires elements the same size as the target.
  SmallBitVector UndefElts;
  SmallVector<uint64_t, 8> RawMask;
  if (!extractConstantMask(C, ElSize, UndefElts, RawMask))
    return;

  unsigned NumElts = RawMask.size();
  unsigned NumEltsPerLane = 128 / ElSize;
  assert((NumElts == 2 || NumElts == 4 || NumElts == 8) &&
         "Unexpected number of vector elements.");

  for (unsigned i = 0; i != NumElts; ++i) {
    if (UndefElts[i]) {
      ShuffleMask.push_back(SM_SentinelUndef);
      continue;
    }

    // VPERMIL2 Operation.
    // Bits[3] - Match Bit.
    // Bits[2:1] - (Per Lane) PD Shuffle Mask.
    // Bits[2:0] - (Per Lane) PS Shuffle Mask.
    uint64_t Selector = RawMask[i];
    unsigned MatchBit = (Selector >> 3) & 0x1;

    // M2Z[0:1]     MatchBit
    //   0Xb           X        Source selected by Selector index.
    //   10b           0        Source selected by Selector index.
    //   10b           1        Zero.
    //   11b           0        Zero.
    //   11b           1        Source selected by Selector index.
    if ((M2Z & 0x2) != 0u && MatchBit != (M2Z & 0x1)) {
      ShuffleMask.push_back(SM_SentinelZero);
      continue;
    }

    int Index = i & ~(NumEltsPerLane - 1);
    if (ElSize == 64)
      Index += (Selector >> 1) & 0x1;
    else
      Index += Selector & 0x3;

    int Src = (Selector >> 2) & 0x1;
    Index += Src * NumElts;
    ShuffleMask.push_back(Index);
  }
}

void DecodeVPPERMMask(const Constant *C, SmallVectorImpl<int> &ShuffleMask) {
  assert(C->getType()->getPrimitiveSizeInBits() == 128 &&
         "Unexpected vector size.");

  // The shuffle mask requires a byte vector.
  SmallBitVector UndefElts;
  SmallVector<uint64_t, 32> RawMask;
  if (!extractConstantMask(C, 8, UndefElts, RawMask))
    return;

  unsigned NumElts = RawMask.size();
  assert(NumElts == 16 && "Unexpected number of vector elements.");

  for (unsigned i = 0; i != NumElts; ++i) {
    if (UndefElts[i]) {
      ShuffleMask.push_back(SM_SentinelUndef);
      continue;
    }

    // VPPERM Operation
    // Bits[4:0] - Byte Index (0 - 31)
    // Bits[7:5] - Permute Operation
    //
    // Permute Operation:
    // 0 - Source byte (no logical operation).
    // 1 - Invert source byte.
    // 2 - Bit reverse of source byte.
    // 3 - Bit reverse of inverted source byte.
    // 4 - 00h (zero - fill).
    // 5 - FFh (ones - fill).
    // 6 - Most significant bit of source byte replicated in all bit positions.
    // 7 - Invert most significant bit of source byte and replicate in all bit
    // positions.
    uint64_t Element = RawMask[i];
    uint64_t Index = Element & 0x1F;
    uint64_t PermuteOp = (Element >> 5) & 0x7;

    if (PermuteOp == 4) {
      ShuffleMask.push_back(SM_SentinelZero);
      continue;
    }
    if (PermuteOp != 0) {
      ShuffleMask.clear();
      return;
    }
    ShuffleMask.push_back((int)Index);
  }
}

void DecodeVPERMVMask(const Constant *C, unsigned ElSize,
                      SmallVectorImpl<int> &ShuffleMask) {
  Type *MaskTy = C->getType();
  unsigned MaskTySize = MaskTy->getPrimitiveSizeInBits();
  (void)MaskTySize;
  assert((MaskTySize == 128 || MaskTySize == 256 || MaskTySize == 512) &&
         "Unexpected vector size.");
  assert((ElSize == 8 || ElSize == 16 || ElSize == 32 || ElSize == 64) &&
         "Unexpected vector element size.");

  // The shuffle mask requires elements the same size as the target.
  SmallBitVector UndefElts;
  SmallVector<uint64_t, 8> RawMask;
  if (!extractConstantMask(C, ElSize, UndefElts, RawMask))
    return;

  unsigned NumElts = RawMask.size();

  for (unsigned i = 0; i != NumElts; ++i) {
    if (UndefElts[i]) {
      ShuffleMask.push_back(SM_SentinelUndef);
      continue;
    }
    int Index = RawMask[i] & (NumElts - 1);
    ShuffleMask.push_back(Index);
  }
}

void DecodeVPERMV3Mask(const Constant *C, unsigned ElSize,
                       SmallVectorImpl<int> &ShuffleMask) {
  Type *MaskTy = C->getType();
  unsigned MaskTySize = MaskTy->getPrimitiveSizeInBits();
  (void)MaskTySize;
  assert((MaskTySize == 128 || MaskTySize == 256 || MaskTySize == 512) &&
         "Unexpected vector size.");
  assert((ElSize == 8 || ElSize == 16 || ElSize == 32 || ElSize == 64) &&
         "Unexpected vector element size.");

  // The shuffle mask requires elements the same size as the target.
  SmallBitVector UndefElts;
  SmallVector<uint64_t, 8> RawMask;
  if (!extractConstantMask(C, ElSize, UndefElts, RawMask))
    return;

  unsigned NumElts = RawMask.size();

  for (unsigned i = 0; i != NumElts; ++i) {
    if (UndefElts[i]) {
      ShuffleMask.push_back(SM_SentinelUndef);
      continue;
    }
    int Index = RawMask[i] & (NumElts*2 - 1);
    ShuffleMask.push_back(Index);
  }
}
} // llvm namespace
