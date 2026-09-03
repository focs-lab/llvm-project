//===- Function.cpp - Implement the Global object classes -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Function class for the IR library.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Function.h"
#include "SymbolTableListTraitsImpl.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/IntrinsicsARM.h"
#include "llvm/IR/IntrinsicsBPF.h"
#include "llvm/IR/IntrinsicsDirectX.h"
#include "llvm/IR/IntrinsicsHexagon.h"
#include "llvm/IR/IntrinsicsLoongArch.h"
#include "llvm/IR/IntrinsicsMips.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/IntrinsicsPowerPC.h"
#include "llvm/IR/IntrinsicsR600.h"
#include "llvm/IR/IntrinsicsRISCV.h"
#include "llvm/IR/IntrinsicsS390.h"
#include "llvm/IR/IntrinsicsSPIRV.h"
#include "llvm/IR/IntrinsicsVE.h"
#include "llvm/IR/IntrinsicsWebAssembly.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/IntrinsicsXCore.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/SymbolTableListTraits.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ModRef.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

using namespace llvm;
using ProfileCount = Function::ProfileCount;

// Explicit instantiations of SymbolTableListTraits since some of the methods
// are not in the public header file...
template class llvm::SymbolTableListTraits<BasicBlock>;

static cl::opt<unsigned> NonGlobalValueMaxNameSize(
    "non-global-value-max-name-size", cl::Hidden, cl::init(1024),
    cl::desc("Maximum size for the name of non-global values."));

extern cl::opt<bool> UseNewDbgInfoFormat;

void Function::convertToNewDbgValues() {
  IsNewDbgInfoFormat = true;
  for (auto &BB : *this) {
    BB.convertToNewDbgValues();
  }
}

void Function::convertFromNewDbgValues() {
  IsNewDbgInfoFormat = false;
  for (auto &BB : *this) {
    BB.convertFromNewDbgValues();
  }
}

void Function::setIsNewDbgInfoFormat(bool NewFlag) {
  if (NewFlag && !IsNewDbgInfoFormat)
    convertToNewDbgValues();
  else if (!NewFlag && IsNewDbgInfoFormat)
    convertFromNewDbgValues();
}
void Function::setNewDbgInfoFormatFlag(bool NewFlag) {
  for (auto &BB : *this) {
    BB.setNewDbgInfoFormatFlag(NewFlag);
  }
  IsNewDbgInfoFormat = NewFlag;
}

//===----------------------------------------------------------------------===//
// Argument Implementation
//===----------------------------------------------------------------------===//

Argument::Argument(Type *Ty, const Twine &Name, Function *Par, unsigned ArgNo)
    : Value(Ty, Value::ArgumentVal), Parent(Par), ArgNo(ArgNo) {
  setName(Name);
}

void Argument::setParent(Function *parent) {
  Parent = parent;
}

bool Argument::hasNonNullAttr(bool AllowUndefOrPoison) const {
  if (!getType()->isPointerTy()) return false;
  if (getParent()->hasParamAttribute(getArgNo(), Attribute::NonNull) &&
      (AllowUndefOrPoison ||
       getParent()->hasParamAttribute(getArgNo(), Attribute::NoUndef)))
    return true;
  else if (getDereferenceableBytes() > 0 &&
           !NullPointerIsDefined(getParent(),
                                 getType()->getPointerAddressSpace()))
    return true;
  return false;
}

bool Argument::hasByValAttr() const {
  if (!getType()->isPointerTy()) return false;
  return hasAttribute(Attribute::ByVal);
}

bool Argument::hasByRefAttr() const {
  if (!getType()->isPointerTy())
    return false;
  return hasAttribute(Attribute::ByRef);
}

bool Argument::hasSwiftSelfAttr() const {
  return getParent()->hasParamAttribute(getArgNo(), Attribute::SwiftSelf);
}

bool Argument::hasSwiftErrorAttr() const {
  return getParent()->hasParamAttribute(getArgNo(), Attribute::SwiftError);
}

bool Argument::hasInAllocaAttr() const {
  if (!getType()->isPointerTy()) return false;
  return hasAttribute(Attribute::InAlloca);
}

bool Argument::hasPreallocatedAttr() const {
  if (!getType()->isPointerTy())
    return false;
  return hasAttribute(Attribute::Preallocated);
}

bool Argument::hasPassPointeeByValueCopyAttr() const {
  if (!getType()->isPointerTy()) return false;
  AttributeList Attrs = getParent()->getAttributes();
  return Attrs.hasParamAttr(getArgNo(), Attribute::ByVal) ||
         Attrs.hasParamAttr(getArgNo(), Attribute::InAlloca) ||
         Attrs.hasParamAttr(getArgNo(), Attribute::Preallocated);
}

bool Argument::hasPointeeInMemoryValueAttr() const {
  if (!getType()->isPointerTy())
    return false;
  AttributeList Attrs = getParent()->getAttributes();
  return Attrs.hasParamAttr(getArgNo(), Attribute::ByVal) ||
         Attrs.hasParamAttr(getArgNo(), Attribute::StructRet) ||
         Attrs.hasParamAttr(getArgNo(), Attribute::InAlloca) ||
         Attrs.hasParamAttr(getArgNo(), Attribute::Preallocated) ||
         Attrs.hasParamAttr(getArgNo(), Attribute::ByRef);
}

/// For a byval, sret, inalloca, or preallocated parameter, get the in-memory
/// parameter type.
static Type *getMemoryParamAllocType(AttributeSet ParamAttrs) {
  // FIXME: All the type carrying attributes are mutually exclusive, so there
  // should be a single query to get the stored type that handles any of them.
  if (Type *ByValTy = ParamAttrs.getByValType())
    return ByValTy;
  if (Type *ByRefTy = ParamAttrs.getByRefType())
    return ByRefTy;
  if (Type *PreAllocTy = ParamAttrs.getPreallocatedType())
    return PreAllocTy;
  if (Type *InAllocaTy = ParamAttrs.getInAllocaType())
    return InAllocaTy;
  if (Type *SRetTy = ParamAttrs.getStructRetType())
    return SRetTy;

  return nullptr;
}

uint64_t Argument::getPassPointeeByValueCopySize(const DataLayout &DL) const {
  AttributeSet ParamAttrs =
      getParent()->getAttributes().getParamAttrs(getArgNo());
  if (Type *MemTy = getMemoryParamAllocType(ParamAttrs))
    return DL.getTypeAllocSize(MemTy);
  return 0;
}

Type *Argument::getPointeeInMemoryValueType() const {
  AttributeSet ParamAttrs =
      getParent()->getAttributes().getParamAttrs(getArgNo());
  return getMemoryParamAllocType(ParamAttrs);
}

MaybeAlign Argument::getParamAlign() const {
  assert(getType()->isPointerTy() && "Only pointers have alignments");
  return getParent()->getParamAlign(getArgNo());
}

MaybeAlign Argument::getParamStackAlign() const {
  return getParent()->getParamStackAlign(getArgNo());
}

Type *Argument::getParamByValType() const {
  assert(getType()->isPointerTy() && "Only pointers have byval types");
  return getParent()->getParamByValType(getArgNo());
}

Type *Argument::getParamStructRetType() const {
  assert(getType()->isPointerTy() && "Only pointers have sret types");
  return getParent()->getParamStructRetType(getArgNo());
}

Type *Argument::getParamByRefType() const {
  assert(getType()->isPointerTy() && "Only pointers have byref types");
  return getParent()->getParamByRefType(getArgNo());
}

Type *Argument::getParamInAllocaType() const {
  assert(getType()->isPointerTy() && "Only pointers have inalloca types");
  return getParent()->getParamInAllocaType(getArgNo());
}

uint64_t Argument::getDereferenceableBytes() const {
  assert(getType()->isPointerTy() &&
         "Only pointers have dereferenceable bytes");
  return getParent()->getParamDereferenceableBytes(getArgNo());
}

uint64_t Argument::getDereferenceableOrNullBytes() const {
  assert(getType()->isPointerTy() &&
         "Only pointers have dereferenceable bytes");
  return getParent()->getParamDereferenceableOrNullBytes(getArgNo());
}

FPClassTest Argument::getNoFPClass() const {
  return getParent()->getParamNoFPClass(getArgNo());
}

std::optional<ConstantRange> Argument::getRange() const {
  const Attribute RangeAttr = getAttribute(llvm::Attribute::Range);
  if (RangeAttr.isValid())
    return RangeAttr.getRange();
  return std::nullopt;
}

bool Argument::hasNestAttr() const {
  if (!getType()->isPointerTy()) return false;
  return hasAttribute(Attribute::Nest);
}

bool Argument::hasNoAliasAttr() const {
  if (!getType()->isPointerTy()) return false;
  return hasAttribute(Attribute::NoAlias);
}

bool Argument::hasNoCaptureAttr() const {
  if (!getType()->isPointerTy()) return false;
  return hasAttribute(Attribute::NoCapture);
}

bool Argument::hasNoFreeAttr() const {
  if (!getType()->isPointerTy()) return false;
  return hasAttribute(Attribute::NoFree);
}

bool Argument::hasStructRetAttr() const {
  if (!getType()->isPointerTy()) return false;
  return hasAttribute(Attribute::StructRet);
}

bool Argument::hasInRegAttr() const {
  return hasAttribute(Attribute::InReg);
}

bool Argument::hasReturnedAttr() const {
  return hasAttribute(Attribute::Returned);
}

bool Argument::hasZExtAttr() const {
  return hasAttribute(Attribute::ZExt);
}

bool Argument::hasSExtAttr() const {
  return hasAttribute(Attribute::SExt);
}

bool Argument::onlyReadsMemory() const {
  AttributeList Attrs = getParent()->getAttributes();
  return Attrs.hasParamAttr(getArgNo(), Attribute::ReadOnly) ||
         Attrs.hasParamAttr(getArgNo(), Attribute::ReadNone);
}

void Argument::addAttrs(AttrBuilder &B) {
  AttributeList AL = getParent()->getAttributes();
  AL = AL.addParamAttributes(Parent->getContext(), getArgNo(), B);
  getParent()->setAttributes(AL);
}

void Argument::addAttr(Attribute::AttrKind Kind) {
  getParent()->addParamAttr(getArgNo(), Kind);
}

void Argument::addAttr(Attribute Attr) {
  getParent()->addParamAttr(getArgNo(), Attr);
}

void Argument::removeAttr(Attribute::AttrKind Kind) {
  getParent()->removeParamAttr(getArgNo(), Kind);
}

void Argument::removeAttrs(const AttributeMask &AM) {
  AttributeList AL = getParent()->getAttributes();
  AL = AL.removeParamAttributes(Parent->getContext(), getArgNo(), AM);
  getParent()->setAttributes(AL);
}

bool Argument::hasAttribute(Attribute::AttrKind Kind) const {
  return getParent()->hasParamAttribute(getArgNo(), Kind);
}

Attribute Argument::getAttribute(Attribute::AttrKind Kind) const {
  return getParent()->getParamAttribute(getArgNo(), Kind);
}

//===----------------------------------------------------------------------===//
// Helper Methods in Function
//===----------------------------------------------------------------------===//

LLVMContext &Function::getContext() const {
  return getType()->getContext();
}

unsigned Function::getInstructionCount() const {
  unsigned NumInstrs = 0;
  for (const BasicBlock &BB : BasicBlocks)
    NumInstrs += std::distance(BB.instructionsWithoutDebug().begin(),
                               BB.instructionsWithoutDebug().end());
  return NumInstrs;
}

Function *Function::Create(FunctionType *Ty, LinkageTypes Linkage,
                           const Twine &N, Module &M) {
  return Create(Ty, Linkage, M.getDataLayout().getProgramAddressSpace(), N, &M);
}

Function *Function::createWithDefaultAttr(FunctionType *Ty,
                                          LinkageTypes Linkage,
                                          unsigned AddrSpace, const Twine &N,
                                          Module *M) {
  auto *F = new Function(Ty, Linkage, AddrSpace, N, M);
  AttrBuilder B(F->getContext());
  UWTableKind UWTable = M->getUwtable();
  if (UWTable != UWTableKind::None)
    B.addUWTableAttr(UWTable);
  switch (M->getFramePointer()) {
  case FramePointerKind::None:
    // 0 ("none") is the default.
    break;
  case FramePointerKind::NonLeaf:
    B.addAttribute("frame-pointer", "non-leaf");
    break;
  case FramePointerKind::All:
    B.addAttribute("frame-pointer", "all");
    break;
  }
  if (M->getModuleFlag("function_return_thunk_extern"))
    B.addAttribute(Attribute::FnRetThunkExtern);
  F->addFnAttrs(B);
  return F;
}

void Function::removeFromParent() {
  getParent()->getFunctionList().remove(getIterator());
}

void Function::eraseFromParent() {
  getParent()->getFunctionList().erase(getIterator());
}

void Function::splice(Function::iterator ToIt, Function *FromF,
                      Function::iterator FromBeginIt,
                      Function::iterator FromEndIt) {
#ifdef EXPENSIVE_CHECKS
  // Check that FromBeginIt is before FromEndIt.
  auto FromFEnd = FromF->end();
  for (auto It = FromBeginIt; It != FromEndIt; ++It)
    assert(It != FromFEnd && "FromBeginIt not before FromEndIt!");
#endif // EXPENSIVE_CHECKS
  BasicBlocks.splice(ToIt, FromF->BasicBlocks, FromBeginIt, FromEndIt);
}

Function::iterator Function::erase(Function::iterator FromIt,
                                   Function::iterator ToIt) {
  return BasicBlocks.erase(FromIt, ToIt);
}

//===----------------------------------------------------------------------===//
// Function Implementation
//===----------------------------------------------------------------------===//

static unsigned computeAddrSpace(unsigned AddrSpace, Module *M) {
  // If AS == -1 and we are passed a valid module pointer we place the function
  // in the program address space. Otherwise we default to AS0.
  if (AddrSpace == static_cast<unsigned>(-1))
    return M ? M->getDataLayout().getProgramAddressSpace() : 0;
  return AddrSpace;
}

Function::Function(FunctionType *Ty, LinkageTypes Linkage, unsigned AddrSpace,
                   const Twine &name, Module *ParentModule)
    : GlobalObject(Ty, Value::FunctionVal,
                   OperandTraits<Function>::op_begin(this), 0, Linkage, name,
                   computeAddrSpace(AddrSpace, ParentModule)),
      NumArgs(Ty->getNumParams()), IsNewDbgInfoFormat(UseNewDbgInfoFormat) {
  assert(FunctionType::isValidReturnType(getReturnType()) &&
         "invalid return type");
  setGlobalObjectSubClassData(0);

  // We only need a symbol table for a function if the context keeps value names
  if (!getContext().shouldDiscardValueNames())
    SymTab = std::make_unique<ValueSymbolTable>(NonGlobalValueMaxNameSize);

  // If the function has arguments, mark them as lazily built.
  if (Ty->getNumParams())
    setValueSubclassData(1);   // Set the "has lazy arguments" bit.

  if (ParentModule) {
    ParentModule->getFunctionList().push_back(this);
    IsNewDbgInfoFormat = ParentModule->IsNewDbgInfoFormat;
  }

  HasLLVMReservedName = getName().starts_with("llvm.");
  // Ensure intrinsics have the right parameter attributes.
  // Note, the IntID field will have been set in Value::setName if this function
  // name is a valid intrinsic ID.
  if (IntID)
    setAttributes(Intrinsic::getAttributes(getContext(), IntID));
}

Function::~Function() {
  dropAllReferences();    // After this it is safe to delete instructions.

  // Delete all of the method arguments and unlink from symbol table...
  if (Arguments)
    clearArguments();

  // Remove the function from the on-the-side GC table.
  clearGC();
}

void Function::BuildLazyArguments() const {
  // Create the arguments vector, all arguments start out unnamed.
  auto *FT = getFunctionType();
  if (NumArgs > 0) {
    Arguments = std::allocator<Argument>().allocate(NumArgs);
    for (unsigned i = 0, e = NumArgs; i != e; ++i) {
      Type *ArgTy = FT->getParamType(i);
      assert(!ArgTy->isVoidTy() && "Cannot have void typed arguments!");
      new (Arguments + i) Argument(ArgTy, "", const_cast<Function *>(this), i);
    }
  }

  // Clear the lazy arguments bit.
  unsigned SDC = getSubclassDataFromValue();
  SDC &= ~(1 << 0);
  const_cast<Function*>(this)->setValueSubclassData(SDC);
  assert(!hasLazyArguments());
}

static MutableArrayRef<Argument> makeArgArray(Argument *Args, size_t Count) {
  return MutableArrayRef<Argument>(Args, Count);
}

bool Function::isConstrainedFPIntrinsic() const {
  return Intrinsic::isConstrainedFPIntrinsic(getIntrinsicID());
}

void Function::clearArguments() {
  for (Argument &A : makeArgArray(Arguments, NumArgs)) {
    A.setName("");
    A.~Argument();
  }
  std::allocator<Argument>().deallocate(Arguments, NumArgs);
  Arguments = nullptr;
}

void Function::stealArgumentListFrom(Function &Src) {
  assert(isDeclaration() && "Expected no references to current arguments");

  // Drop the current arguments, if any, and set the lazy argument bit.
  if (!hasLazyArguments()) {
    assert(llvm::all_of(makeArgArray(Arguments, NumArgs),
                        [](const Argument &A) { return A.use_empty(); }) &&
           "Expected arguments to be unused in declaration");
    clearArguments();
    setValueSubclassData(getSubclassDataFromValue() | (1 << 0));
  }

  // Nothing to steal if Src has lazy arguments.
  if (Src.hasLazyArguments())
    return;

  // Steal arguments from Src, and fix the lazy argument bits.
  assert(arg_size() == Src.arg_size());
  Arguments = Src.Arguments;
  Src.Arguments = nullptr;
  for (Argument &A : makeArgArray(Arguments, NumArgs)) {
    // FIXME: This does the work of transferNodesFromList inefficiently.
    SmallString<128> Name;
    if (A.hasName())
      Name = A.getName();
    if (!Name.empty())
      A.setName("");
    A.setParent(this);
    if (!Name.empty())
      A.setName(Name);
  }

  setValueSubclassData(getSubclassDataFromValue() & ~(1 << 0));
  assert(!hasLazyArguments());
  Src.setValueSubclassData(Src.getSubclassDataFromValue() | (1 << 0));
}

void Function::deleteBodyImpl(bool ShouldDrop) {
  setIsMaterializable(false);

  for (BasicBlock &BB : *this)
    BB.dropAllReferences();

  // Delete all basic blocks. They are now unused, except possibly by
  // blockaddresses, but BasicBlock's destructor takes care of those.
  while (!BasicBlocks.empty())
    BasicBlocks.begin()->eraseFromParent();

  if (getNumOperands()) {
    if (ShouldDrop) {
      // Drop uses of any optional data (real or placeholder).
      User::dropAllReferences();
      setNumHungOffUseOperands(0);
    } else {
      // The code needs to match Function::allocHungoffUselist().
      auto *CPN = ConstantPointerNull::get(PointerType::get(getContext(), 0));
      Op<0>().set(CPN);
      Op<1>().set(CPN);
      Op<2>().set(CPN);
    }
    setValueSubclassData(getSubclassDataFromValue() & ~0xe);
  }

  // Metadata is stored in a side-table.
  clearMetadata();
}

void Function::addAttributeAtIndex(unsigned i, Attribute Attr) {
  AttributeSets = AttributeSets.addAttributeAtIndex(getContext(), i, Attr);
}

void Function::addFnAttr(Attribute::AttrKind Kind) {
  AttributeSets = AttributeSets.addFnAttribute(getContext(), Kind);
}

void Function::addFnAttr(StringRef Kind, StringRef Val) {
  AttributeSets = AttributeSets.addFnAttribute(getContext(), Kind, Val);
}

void Function::addFnAttr(Attribute Attr) {
  AttributeSets = AttributeSets.addFnAttribute(getContext(), Attr);
}

void Function::addFnAttrs(const AttrBuilder &Attrs) {
  AttributeSets = AttributeSets.addFnAttributes(getContext(), Attrs);
}

void Function::addRetAttr(Attribute::AttrKind Kind) {
  AttributeSets = AttributeSets.addRetAttribute(getContext(), Kind);
}

void Function::addRetAttr(Attribute Attr) {
  AttributeSets = AttributeSets.addRetAttribute(getContext(), Attr);
}

void Function::addRetAttrs(const AttrBuilder &Attrs) {
  AttributeSets = AttributeSets.addRetAttributes(getContext(), Attrs);
}

void Function::addParamAttr(unsigned ArgNo, Attribute::AttrKind Kind) {
  AttributeSets = AttributeSets.addParamAttribute(getContext(), ArgNo, Kind);
}

void Function::addParamAttr(unsigned ArgNo, Attribute Attr) {
  AttributeSets = AttributeSets.addParamAttribute(getContext(), ArgNo, Attr);
}

void Function::addParamAttrs(unsigned ArgNo, const AttrBuilder &Attrs) {
  AttributeSets = AttributeSets.addParamAttributes(getContext(), ArgNo, Attrs);
}

void Function::removeAttributeAtIndex(unsigned i, Attribute::AttrKind Kind) {
  AttributeSets = AttributeSets.removeAttributeAtIndex(getContext(), i, Kind);
}

void Function::removeAttributeAtIndex(unsigned i, StringRef Kind) {
  AttributeSets = AttributeSets.removeAttributeAtIndex(getContext(), i, Kind);
}

void Function::removeFnAttr(Attribute::AttrKind Kind) {
  AttributeSets = AttributeSets.removeFnAttribute(getContext(), Kind);
}

void Function::removeFnAttr(StringRef Kind) {
  AttributeSets = AttributeSets.removeFnAttribute(getContext(), Kind);
}

void Function::removeFnAttrs(const AttributeMask &AM) {
  AttributeSets = AttributeSets.removeFnAttributes(getContext(), AM);
}

void Function::removeRetAttr(Attribute::AttrKind Kind) {
  AttributeSets = AttributeSets.removeRetAttribute(getContext(), Kind);
}

void Function::removeRetAttr(StringRef Kind) {
  AttributeSets = AttributeSets.removeRetAttribute(getContext(), Kind);
}

void Function::removeRetAttrs(const AttributeMask &Attrs) {
  AttributeSets = AttributeSets.removeRetAttributes(getContext(), Attrs);
}

void Function::removeParamAttr(unsigned ArgNo, Attribute::AttrKind Kind) {
  AttributeSets = AttributeSets.removeParamAttribute(getContext(), ArgNo, Kind);
}

void Function::removeParamAttr(unsigned ArgNo, StringRef Kind) {
  AttributeSets = AttributeSets.removeParamAttribute(getContext(), ArgNo, Kind);
}

void Function::removeParamAttrs(unsigned ArgNo, const AttributeMask &Attrs) {
  AttributeSets =
      AttributeSets.removeParamAttributes(getContext(), ArgNo, Attrs);
}

void Function::addDereferenceableParamAttr(unsigned ArgNo, uint64_t Bytes) {
  AttributeSets =
      AttributeSets.addDereferenceableParamAttr(getContext(), ArgNo, Bytes);
}

bool Function::hasFnAttribute(Attribute::AttrKind Kind) const {
  return AttributeSets.hasFnAttr(Kind);
}

bool Function::hasFnAttribute(StringRef Kind) const {
  return AttributeSets.hasFnAttr(Kind);
}

bool Function::hasRetAttribute(Attribute::AttrKind Kind) const {
  return AttributeSets.hasRetAttr(Kind);
}

bool Function::hasParamAttribute(unsigned ArgNo,
                                 Attribute::AttrKind Kind) const {
  return AttributeSets.hasParamAttr(ArgNo, Kind);
}

Attribute Function::getAttributeAtIndex(unsigned i,
                                        Attribute::AttrKind Kind) const {
  return AttributeSets.getAttributeAtIndex(i, Kind);
}

Attribute Function::getAttributeAtIndex(unsigned i, StringRef Kind) const {
  return AttributeSets.getAttributeAtIndex(i, Kind);
}

Attribute Function::getFnAttribute(Attribute::AttrKind Kind) const {
  return AttributeSets.getFnAttr(Kind);
}

Attribute Function::getFnAttribute(StringRef Kind) const {
  return AttributeSets.getFnAttr(Kind);
}

Attribute Function::getRetAttribute(Attribute::AttrKind Kind) const {
  return AttributeSets.getRetAttr(Kind);
}

uint64_t Function::getFnAttributeAsParsedInteger(StringRef Name,
                                                 uint64_t Default) const {
  Attribute A = getFnAttribute(Name);
  uint64_t Result = Default;
  if (A.isStringAttribute()) {
    StringRef Str = A.getValueAsString();
    if (Str.getAsInteger(0, Result))
      getContext().emitError("cannot parse integer attribute " + Name);
  }

  return Result;
}

/// gets the specified attribute from the list of attributes.
Attribute Function::getParamAttribute(unsigned ArgNo,
                                      Attribute::AttrKind Kind) const {
  return AttributeSets.getParamAttr(ArgNo, Kind);
}

void Function::addDereferenceableOrNullParamAttr(unsigned ArgNo,
                                                 uint64_t Bytes) {
  AttributeSets = AttributeSets.addDereferenceableOrNullParamAttr(getContext(),
                                                                  ArgNo, Bytes);
}

void Function::addRangeRetAttr(const ConstantRange &CR) {
  AttributeSets = AttributeSets.addRangeRetAttr(getContext(), CR);
}

DenormalMode Function::getDenormalMode(const fltSemantics &FPType) const {
  if (&FPType == &APFloat::IEEEsingle()) {
    DenormalMode Mode = getDenormalModeF32Raw();
    // If the f32 variant of the attribute isn't specified, try to use the
    // generic one.
    if (Mode.isValid())
      return Mode;
  }

  return getDenormalModeRaw();
}

DenormalMode Function::getDenormalModeRaw() const {
  Attribute Attr = getFnAttribute("denormal-fp-math");
  StringRef Val = Attr.getValueAsString();
  return parseDenormalFPAttribute(Val);
}

DenormalMode Function::getDenormalModeF32Raw() const {
  Attribute Attr = getFnAttribute("denormal-fp-math-f32");
  if (Attr.isValid()) {
    StringRef Val = Attr.getValueAsString();
    return parseDenormalFPAttribute(Val);
  }

  return DenormalMode::getInvalid();
}

const std::string &Function::getGC() const {
  assert(hasGC() && "Function has no collector");
  return getContext().getGC(*this);
}

void Function::setGC(std::string Str) {
  setValueSubclassDataBit(14, !Str.empty());
  getContext().setGC(*this, std::move(Str));
}

void Function::clearGC() {
  if (!hasGC())
    return;
  getContext().deleteGC(*this);
  setValueSubclassDataBit(14, false);
}

bool Function::hasStackProtectorFnAttr() const {
  return hasFnAttribute(Attribute::StackProtect) ||
         hasFnAttribute(Attribute::StackProtectStrong) ||
         hasFnAttribute(Attribute::StackProtectReq);
}

/// Copy all additional attributes (those not needed to create a Function) from
/// the Function Src to this one.
void Function::copyAttributesFrom(const Function *Src) {
  GlobalObject::copyAttributesFrom(Src);
  setCallingConv(Src->getCallingConv());
  setAttributes(Src->getAttributes());
  if (Src->hasGC())
    setGC(Src->getGC());
  else
    clearGC();
  if (Src->hasPersonalityFn())
    setPersonalityFn(Src->getPersonalityFn());
  if (Src->hasPrefixData())
    setPrefixData(Src->getPrefixData());
  if (Src->hasPrologueData())
    setPrologueData(Src->getPrologueData());
}

MemoryEffects Function::getMemoryEffects() const {
  return getAttributes().getMemoryEffects();
}
void Function::setMemoryEffects(MemoryEffects ME) {
  addFnAttr(Attribute::getWithMemoryEffects(getContext(), ME));
}

/// Determine if the function does not access memory.
bool Function::doesNotAccessMemory() const {
  return getMemoryEffects().doesNotAccessMemory();
}
void Function::setDoesNotAccessMemory() {
  setMemoryEffects(MemoryEffects::none());
}

/// Determine if the function does not access or only reads memory.
bool Function::onlyReadsMemory() const {
  return getMemoryEffects().onlyReadsMemory();
}
void Function::setOnlyReadsMemory() {
  setMemoryEffects(getMemoryEffects() & MemoryEffects::readOnly());
}

/// Determine if the function does not access or only writes memory.
bool Function::onlyWritesMemory() const {
  return getMemoryEffects().onlyWritesMemory();
}
void Function::setOnlyWritesMemory() {
  setMemoryEffects(getMemoryEffects() & MemoryEffects::writeOnly());
}

/// Determine if the call can access memmory only using pointers based
/// on its arguments.
bool Function::onlyAccessesArgMemory() const {
  return getMemoryEffects().onlyAccessesArgPointees();
}
void Function::setOnlyAccessesArgMemory() {
  setMemoryEffects(getMemoryEffects() & MemoryEffects::argMemOnly());
}

/// Determine if the function may only access memory that is
///  inaccessible from the IR.
bool Function::onlyAccessesInaccessibleMemory() const {
  return getMemoryEffects().onlyAccessesInaccessibleMem();
}
void Function::setOnlyAccessesInaccessibleMemory() {
  setMemoryEffects(getMemoryEffects() & MemoryEffects::inaccessibleMemOnly());
}

/// Determine if the function may only access memory that is
///  either inaccessible from the IR or pointed to by its arguments.
bool Function::onlyAccessesInaccessibleMemOrArgMem() const {
  return getMemoryEffects().onlyAccessesInaccessibleOrArgMem();
}
void Function::setOnlyAccessesInaccessibleMemOrArgMem() {
  setMemoryEffects(getMemoryEffects() &
                   MemoryEffects::inaccessibleOrArgMemOnly());
}

/// Table of string intrinsic names indexed by enum value.
static const char * const IntrinsicNameTable[] = {
  "not_intrinsic",
#define GET_INTRINSIC_NAME_TABLE
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_NAME_TABLE
};

/// Table of per-target intrinsic name tables.
#define GET_INTRINSIC_TARGET_DATA
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_TARGET_DATA

bool Function::isTargetIntrinsic(Intrinsic::ID IID) {
  return IID > TargetInfos[0].Count;
}

bool Function::isTargetIntrinsic() const {
  return isTargetIntrinsic(IntID);
}

/// Find the segment of \c IntrinsicNameTable for intrinsics with the same
/// target as \c Name, or the generic table if \c Name is not target specific.
///
/// Returns the relevant slice of \c IntrinsicNameTable
static ArrayRef<const char *> findTargetSubtable(StringRef Name) {
  assert(Name.starts_with("llvm."));

  ArrayRef<IntrinsicTargetInfo> Targets(TargetInfos);
  // Drop "llvm." and take the first dotted component. That will be the target
  // if this is target specific.
  StringRef Target = Name.drop_front(5).split('.').first;
  auto It = partition_point(
      Targets, [=](const IntrinsicTargetInfo &TI) { return TI.Name < Target; });
  // We've either found the target or just fall back to the generic set, which
  // is always first.
  const auto &TI = It != Targets.end() && It->Name == Target ? *It : Targets[0];
  return ArrayRef(&IntrinsicNameTable[1] + TI.Offset, TI.Count);
}

/// This does the actual lookup of an intrinsic ID which
/// matches the given function name.
Intrinsic::ID Function::lookupIntrinsicID(StringRef Name) {
  ArrayRef<const char *> NameTable = findTargetSubtable(Name);
  int Idx = Intrinsic::lookupLLVMIntrinsicByName(NameTable, Name);
  if (Idx == -1)
    return Intrinsic::not_intrinsic;

  // Intrinsic IDs correspond to the location in IntrinsicNameTable, but we have
  // an index into a sub-table.
  int Adjust = NameTable.data() - IntrinsicNameTable;
  Intrinsic::ID ID = static_cast<Intrinsic::ID>(Idx + Adjust);

  // If the intrinsic is not overloaded, require an exact match. If it is
  // overloaded, require either exact or prefix match.
  const auto MatchSize = strlen(NameTable[Idx]);
  assert(Name.size() >= MatchSize && "Expected either exact or prefix match");
  bool IsExactMatch = Name.size() == MatchSize;
  return IsExactMatch || Intrinsic::isOverloaded(ID) ? ID
                                                     : Intrinsic::not_intrinsic;
}

void Function::updateAfterNameChange() {
  LibFuncCache = UnknownLibFunc;
  StringRef Name = getName();
  if (!Name.starts_with("llvm.")) {
    HasLLVMReservedName = false;
    IntID = Intrinsic::not_intrinsic;
    return;
  }
  HasLLVMReservedName = true;
  IntID = lookupIntrinsicID(Name);
}

/// Returns a stable mangling for the type specified for use in the name
/// mangling scheme used by 'any' types in intrinsic signatures.  The mangling
/// of named types is simply their name.  Manglings for unnamed types consist
/// of a prefix ('p' for pointers, 'a' for arrays, 'f_' for functions)
/// combined with the mangling of their component types.  A vararg function
/// type will have a suffix of 'vararg'.  Since function types can contain
/// other function types, we close a function type mangling with suffix 'f'
/// which can't be confused with it's prefix.  This ensures we don't have
/// collisions between two unrelated function types. Otherwise, you might
/// parse ffXX as f(fXX) or f(fX)X.  (X is a placeholder for any other type.)
/// The HasUnnamedType boolean is set if an unnamed type was encountered,
/// indicating that extra care must be taken to ensure a unique name.
static std::string getMangledTypeStr(Type *Ty, bool &HasUnnamedType) {
  std::string Result;
  if (PointerType *PTyp = dyn_cast<PointerType>(Ty)) {
    Result += "p" + utostr(PTyp->getAddressSpace());
  } else if (ArrayType *ATyp = dyn_cast<ArrayType>(Ty)) {
    Result += "a" + utostr(ATyp->getNumElements()) +
              getMangledTypeStr(ATyp->getElementType(), HasUnnamedType);
  } else if (StructType *STyp = dyn_cast<StructType>(Ty)) {
    if (!STyp->isLiteral()) {
      Result += "s_";
      if (STyp->hasName())
        Result += STyp->getName();
      else
        HasUnnamedType = true;
    } else {
      Result += "sl_";
      for (auto *Elem : STyp->elements())
        Result += getMangledTypeStr(Elem, HasUnnamedType);
    }
    // Ensure nested structs are distinguishable.
    Result += "s";
  } else if (FunctionType *FT = dyn_cast<FunctionType>(Ty)) {
    Result += "f_" + getMangledTypeStr(FT->getReturnType(), HasUnnamedType);
    for (size_t i = 0; i < FT->getNumParams(); i++)
      Result += getMangledTypeStr(FT->getParamType(i), HasUnnamedType);
    if (FT->isVarArg())
      Result += "vararg";
    // Ensure nested function types are distinguishable.
    Result += "f";
  } else if (VectorType *VTy = dyn_cast<VectorType>(Ty)) {
    ElementCount EC = VTy->getElementCount();
    if (EC.isScalable())
      Result += "nx";
    Result += "v" + utostr(EC.getKnownMinValue()) +
              getMangledTypeStr(VTy->getElementType(), HasUnnamedType);
  } else if (TargetExtType *TETy = dyn_cast<TargetExtType>(Ty)) {
    Result += "t";
    Result += TETy->getName();
    for (Type *ParamTy : TETy->type_params())
      Result += "_" + getMangledTypeStr(ParamTy, HasUnnamedType);
    for (unsigned IntParam : TETy->int_params())
      Result += "_" + utostr(IntParam);
    // Ensure nested target extension types are distinguishable.
    Result += "t";
  } else if (Ty) {
    switch (Ty->getTypeID()) {
    default: llvm_unreachable("Unhandled type");
    case Type::VoidTyID:      Result += "isVoid";   break;
    case Type::MetadataTyID:  Result += "Metadata"; break;
    case Type::HalfTyID:      Result += "f16";      break;
    case Type::BFloatTyID:    Result += "bf16";     break;
    case Type::FloatTyID:     Result += "f32";      break;
    case Type::DoubleTyID:    Result += "f64";      break;
    case Type::X86_FP80TyID:  Result += "f80";      break;
    case Type::FP128TyID:     Result += "f128";     break;
    case Type::PPC_FP128TyID: Result += "ppcf128";  break;
    case Type::X86_MMXTyID:   Result += "x86mmx";   break;
    case Type::X86_AMXTyID:   Result += "x86amx";   break;
    case Type::IntegerTyID:
      Result += "i" + utostr(cast<IntegerType>(Ty)->getBitWidth());
      break;
    }
  }
  return Result;
}

StringRef Intrinsic::getBaseName(ID id) {
  assert(id < num_intrinsics && "Invalid intrinsic ID!");
  return IntrinsicNameTable[id];
}

StringRef Intrinsic::getName(ID id) {
  assert(id < num_intrinsics && "Invalid intrinsic ID!");
  assert(!Intrinsic::isOverloaded(id) &&
         "This version of getName does not support overloading");
  return getBaseName(id);
}

static std::string getIntrinsicNameImpl(Intrinsic::ID Id, ArrayRef<Type *> Tys,
                                        Module *M, FunctionType *FT,
                                        bool EarlyModuleCheck) {

  assert(Id < Intrinsic::num_intrinsics && "Invalid intrinsic ID!");
  assert((Tys.empty() || Intrinsic::isOverloaded(Id)) &&
         "This version of getName is for overloaded intrinsics only");
  (void)EarlyModuleCheck;
  assert((!EarlyModuleCheck || M ||
          !any_of(Tys, [](Type *T) { return isa<PointerType>(T); })) &&
         "Intrinsic overloading on pointer types need to provide a Module");
  bool HasUnnamedType = false;
  std::string Result(Intrinsic::getBaseName(Id));
  for (Type *Ty : Tys)
    Result += "." + getMangledTypeStr(Ty, HasUnnamedType);
  if (HasUnnamedType) {
    assert(M && "unnamed types need a module");
    if (!FT)
      FT = Intrinsic::getType(M->getContext(), Id, Tys);
    else
      assert((FT == Intrinsic::getType(M->getContext(), Id, Tys)) &&
             "Provided FunctionType must match arguments");
    return M->getUniqueIntrinsicName(Result, Id, FT);
  }
  return Result;
}

std::string Intrinsic::getName(ID Id, ArrayRef<Type *> Tys, Module *M,
                               FunctionType *FT) {
  assert(M && "We need to have a Module");
  return getIntrinsicNameImpl(Id, Tys, M, FT, true);
}

std::string Intrinsic::getNameNoUnnamedTypes(ID Id, ArrayRef<Type *> Tys) {
  return getIntrinsicNameImpl(Id, Tys, nullptr, nullptr, false);
}

/// IIT_Info - These are enumerators that describe the entries returned by the
/// getIntrinsicInfoTableEntries function.
///
/// Defined in Intrinsics.td.
enum IIT_Info {
#define GET_INTRINSIC_IITINFO
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_IITINFO
};

static void DecodeIITType(unsigned &NextElt, ArrayRef<unsigned char> Infos,
                      IIT_Info LastInfo,
                      SmallVectorImpl<Intrinsic::IITDescriptor> &OutputTable) {
  using namespace Intrinsic;

  bool IsScalableVector = (LastInfo == IIT_SCALABLE_VEC);

  IIT_Info Info = IIT_Info(Infos[NextElt++]);
  unsigned StructElts = 2;

  switch (Info) {
  case IIT_Done:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Void, 0));
    return;
  case IIT_VARARG:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::VarArg, 0));
    return;
  case IIT_MMX:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::MMX, 0));
    return;
  case IIT_AMX:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::AMX, 0));
    return;
  case IIT_TOKEN:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Token, 0));
    return;
  case IIT_METADATA:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Metadata, 0));
    return;
  case IIT_F16:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Half, 0));
    return;
  case IIT_BF16:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::BFloat, 0));
    return;
  case IIT_F32:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Float, 0));
    return;
  case IIT_F64:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Double, 0));
    return;
  case IIT_F128:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Quad, 0));
    return;
  case IIT_PPCF128:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::PPCQuad, 0));
    return;
  case IIT_I1:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer, 1));
    return;
  case IIT_I2:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer, 2));
    return;
  case IIT_I4:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer, 4));
    return;
  case IIT_AARCH64_SVCOUNT:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::AArch64Svcount, 0));
    return;
  case IIT_I8:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer, 8));
    return;
  case IIT_I16:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer,16));
    return;
  case IIT_I32:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer, 32));
    return;
  case IIT_I64:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer, 64));
    return;
  case IIT_I128:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Integer, 128));
    return;
  case IIT_V1:
    OutputTable.push_back(IITDescriptor::getVector(1, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V2:
    OutputTable.push_back(IITDescriptor::getVector(2, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V3:
    OutputTable.push_back(IITDescriptor::getVector(3, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V4:
    OutputTable.push_back(IITDescriptor::getVector(4, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V6:
    OutputTable.push_back(IITDescriptor::getVector(6, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V8:
    OutputTable.push_back(IITDescriptor::getVector(8, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V10:
    OutputTable.push_back(IITDescriptor::getVector(10, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V16:
    OutputTable.push_back(IITDescriptor::getVector(16, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V32:
    OutputTable.push_back(IITDescriptor::getVector(32, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V64:
    OutputTable.push_back(IITDescriptor::getVector(64, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V128:
    OutputTable.push_back(IITDescriptor::getVector(128, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V256:
    OutputTable.push_back(IITDescriptor::getVector(256, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V512:
    OutputTable.push_back(IITDescriptor::getVector(512, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_V1024:
    OutputTable.push_back(IITDescriptor::getVector(1024, IsScalableVector));
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  case IIT_EXTERNREF:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Pointer, 10));
    return;
  case IIT_FUNCREF:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Pointer, 20));
    return;
  case IIT_PTR:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Pointer, 0));
    return;
  case IIT_ANYPTR: // [ANYPTR addrspace]
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Pointer,
                                             Infos[NextElt++]));
    return;
  case IIT_ARG: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Argument, ArgInfo));
    return;
  }
  case IIT_EXTEND_ARG: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::ExtendArgument,
                                             ArgInfo));
    return;
  }
  case IIT_TRUNC_ARG: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::TruncArgument,
                                             ArgInfo));
    return;
  }
  case IIT_HALF_VEC_ARG: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::HalfVecArgument,
                                             ArgInfo));
    return;
  }
  case IIT_SAME_VEC_WIDTH_ARG: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::SameVecWidthArgument,
                                             ArgInfo));
    return;
  }
  case IIT_VEC_OF_ANYPTRS_TO_ELT: {
    unsigned short ArgNo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    unsigned short RefNo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(
        IITDescriptor::get(IITDescriptor::VecOfAnyPtrsToElt, ArgNo, RefNo));
    return;
  }
  case IIT_EMPTYSTRUCT:
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Struct, 0));
    return;
  case IIT_STRUCT9: ++StructElts; [[fallthrough]];
  case IIT_STRUCT8: ++StructElts; [[fallthrough]];
  case IIT_STRUCT7: ++StructElts; [[fallthrough]];
  case IIT_STRUCT6: ++StructElts; [[fallthrough]];
  case IIT_STRUCT5: ++StructElts; [[fallthrough]];
  case IIT_STRUCT4: ++StructElts; [[fallthrough]];
  case IIT_STRUCT3: ++StructElts; [[fallthrough]];
  case IIT_STRUCT2: {
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Struct,StructElts));

    for (unsigned i = 0; i != StructElts; ++i)
      DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  }
  case IIT_SUBDIVIDE2_ARG: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Subdivide2Argument,
                                             ArgInfo));
    return;
  }
  case IIT_SUBDIVIDE4_ARG: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::Subdivide4Argument,
                                             ArgInfo));
    return;
  }
  case IIT_VEC_ELEMENT: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::VecElementArgument,
                                             ArgInfo));
    return;
  }
  case IIT_SCALABLE_VEC: {
    DecodeIITType(NextElt, Infos, Info, OutputTable);
    return;
  }
  case IIT_VEC_OF_BITCASTS_TO_INT: {
    unsigned ArgInfo = (NextElt == Infos.size() ? 0 : Infos[NextElt++]);
    OutputTable.push_back(IITDescriptor::get(IITDescriptor::VecOfBitcastsToInt,
                                             ArgInfo));
    return;
  }
  }
  llvm_unreachable("unhandled");
}

#define GET_INTRINSIC_GENERATOR_GLOBAL
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_GENERATOR_GLOBAL

void Intrinsic::getIntrinsicInfoTableEntries(ID id,
                                             SmallVectorImpl<IITDescriptor> &T){
  // Check to see if the intrinsic's type was expressible by the table.
  unsigned TableVal = IIT_Table[id-1];

  // Decode the TableVal into an array of IITValues.
  SmallVector<unsigned char, 8> IITValues;
  ArrayRef<unsigned char> IITEntries;
  unsigned NextElt = 0;
  if ((TableVal >> 31) != 0) {
    // This is an offset into the IIT_LongEncodingTable.
    IITEntries = IIT_LongEncodingTable;

    // Strip sentinel bit.
    NextElt = (TableVal << 1) >> 1;
  } else {
    // Decode the TableVal into an array of IITValues.  If the entry was encoded
    // into a single word in the table itself, decode it now.
    do {
      IITValues.push_back(TableVal & 0xF);
      TableVal >>= 4;
    } while (TableVal);

    IITEntries = IITValues;
    NextElt = 0;
  }

  // Okay, decode the table into the output vector of IITDescriptors.
  DecodeIITType(NextElt, IITEntries, IIT_Done, T);
  while (NextElt != IITEntries.size() && IITEntries[NextElt] != 0)
    DecodeIITType(NextElt, IITEntries, IIT_Done, T);
}

static Type *DecodeFixedType(ArrayRef<Intrinsic::IITDescriptor> &Infos,
                             ArrayRef<Type*> Tys, LLVMContext &Context) {
  using namespace Intrinsic;

  IITDescriptor D = Infos.front();
  Infos = Infos.slice(1);

  switch (D.Kind) {
  case IITDescriptor::Void: return Type::getVoidTy(Context);
  case IITDescriptor::VarArg: return Type::getVoidTy(Context);
  case IITDescriptor::MMX: return Type::getX86_MMXTy(Context);
  case IITDescriptor::AMX: return Type::getX86_AMXTy(Context);
  case IITDescriptor::Token: return Type::getTokenTy(Context);
  case IITDescriptor::Metadata: return Type::getMetadataTy(Context);
  case IITDescriptor::Half: return Type::getHalfTy(Context);
  case IITDescriptor::BFloat: return Type::getBFloatTy(Context);
  case IITDescriptor::Float: return Type::getFloatTy(Context);
  case IITDescriptor::Double: return Type::getDoubleTy(Context);
  case IITDescriptor::Quad: return Type::getFP128Ty(Context);
  case IITDescriptor::PPCQuad: return Type::getPPC_FP128Ty(Context);
  case IITDescriptor::AArch64Svcount:
    return TargetExtType::get(Context, "aarch64.svcount");

  case IITDescriptor::Integer:
    return IntegerType::get(Context, D.Integer_Width);
  case IITDescriptor::Vector:
    return VectorType::get(DecodeFixedType(Infos, Tys, Context),
                           D.Vector_Width);
  case IITDescriptor::Pointer:
    return PointerType::get(Context, D.Pointer_AddressSpace);
  case IITDescriptor::Struct: {
    SmallVector<Type *, 8> Elts;
    for (unsigned i = 0, e = D.Struct_NumElements; i != e; ++i)
      Elts.push_back(DecodeFixedType(Infos, Tys, Context));
    return StructType::get(Context, Elts);
  }
  case IITDescriptor::Argument:
    return Tys[D.getArgumentNumber()];
  case IITDescriptor::ExtendArgument: {
    Type *Ty = Tys[D.getArgumentNumber()];
    if (VectorType *VTy = dyn_cast<VectorType>(Ty))
      return VectorType::getExtendedElementVectorType(VTy);

    return IntegerType::get(Context, 2 * cast<IntegerType>(Ty)->getBitWidth());
  }
  case IITDescriptor::TruncArgument: {
    Type *Ty = Tys[D.getArgumentNumber()];
    if (VectorType *VTy = dyn_cast<VectorType>(Ty))
      return VectorType::getTruncatedElementVectorType(VTy);

    IntegerType *ITy = cast<IntegerType>(Ty);
    assert(ITy->getBitWidth() % 2 == 0);
    return IntegerType::get(Context, ITy->getBitWidth() / 2);
  }
  case IITDescriptor::Subdivide2Argument:
  case IITDescriptor::Subdivide4Argument: {
    Type *Ty = Tys[D.getArgumentNumber()];
    VectorType *VTy = dyn_cast<VectorType>(Ty);
    assert(VTy && "Expected an argument of Vector Type");
    int SubDivs = D.Kind == IITDescriptor::Subdivide2Argument ? 1 : 2;
    return VectorType::getSubdividedVectorType(VTy, SubDivs);
  }
  case IITDescriptor::HalfVecArgument:
    return VectorType::getHalfElementsVectorType(cast<VectorType>(
                                                  Tys[D.getArgumentNumber()]));
  case IITDescriptor::SameVecWidthArgument: {
    Type *EltTy = DecodeFixedType(Infos, Tys, Context);
    Type *Ty = Tys[D.getArgumentNumber()];
    if (auto *VTy = dyn_cast<VectorType>(Ty))
      return VectorType::get(EltTy, VTy->getElementCount());
    return EltTy;
  }
  case IITDescriptor::VecElementArgument: {
    Type *Ty = Tys[D.getArgumentNumber()];
    if (VectorType *VTy = dyn_cast<VectorType>(Ty))
      return VTy->getElementType();
    llvm_unreachable("Expected an argument of Vector Type");
  }
  case IITDescriptor::VecOfBitcastsToInt: {
    Type *Ty = Tys[D.getArgumentNumber()];
    VectorType *VTy = dyn_cast<VectorType>(Ty);
    assert(VTy && "Expected an argument of Vector Type");
    return VectorType::getInteger(VTy);
  }
  case IITDescriptor::VecOfAnyPtrsToElt:
    // Return the overloaded type (which determines the pointers address space)
    return Tys[D.getOverloadArgNumber()];
  }
  llvm_unreachable("unhandled");
}

FunctionType *Intrinsic::getType(LLVMContext &Context,
                                 ID id, ArrayRef<Type*> Tys) {
  SmallVector<IITDescriptor, 8> Table;
  getIntrinsicInfoTableEntries(id, Table);

  ArrayRef<IITDescriptor> TableRef = Table;
  Type *ResultTy = DecodeFixedType(TableRef, Tys, Context);

  SmallVector<Type*, 8> ArgTys;
  while (!TableRef.empty())
    ArgTys.push_back(DecodeFixedType(TableRef, Tys, Context));

  // DecodeFixedType returns Void for IITDescriptor::Void and IITDescriptor::VarArg
  // If we see void type as the type of the last argument, it is vararg intrinsic
  if (!ArgTys.empty() && ArgTys.back()->isVoidTy()) {
    ArgTys.pop_back();
    return FunctionType::get(ResultTy, ArgTys, true);
  }
  return FunctionType::get(ResultTy, ArgTys, false);
}

bool Intrinsic::isOverloaded(ID id) {
#define GET_INTRINSIC_OVERLOAD_TABLE
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_OVERLOAD_TABLE
}

/// This defines the "Intrinsic::getAttributes(ID id)" method.
#define GET_INTRINSIC_ATTRIBUTES
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_ATTRIBUTES

Function *Intrinsic::getDeclaration(Module *M, ID id, ArrayRef<Type*> Tys) {
  // There can never be multiple globals with the same name of different types,
  // because intrinsics must be a specific type.
  auto *FT = getType(M->getContext(), id, Tys);
  return cast<Function>(
      M->getOrInsertFunction(
           Tys.empty() ? getName(id) : getName(id, Tys, M, FT), FT)
          .getCallee());
}

// This defines the "Intrinsic::getIntrinsicForClangBuiltin()" method.
#define GET_LLVM_INTRINSIC_FOR_CLANG_BUILTIN
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_LLVM_INTRINSIC_FOR_CLANG_BUILTIN

// This defines the "Intrinsic::getIntrinsicForMSBuiltin()" method.
#define GET_LLVM_INTRINSIC_FOR_MS_BUILTIN
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_LLVM_INTRINSIC_FOR_MS_BUILTIN

bool Intrinsic::isConstrainedFPIntrinsic(ID QID) {
  switch (QID) {
#define INSTRUCTION(NAME, NARG, ROUND_MODE, INTRINSIC)                         \
  case Intrinsic::INTRINSIC:
#include "llvm/IR/ConstrainedOps.def"
    return true;
#undef INSTRUCTION
  default:
    return false;
  }
}

using DeferredIntrinsicMatchPair =
    std::pair<Type *, ArrayRef<Intrinsic::IITDescriptor>>;

static bool matchIntrinsicType(
    Type *Ty, ArrayRef<Intrinsic::IITDescriptor> &Infos,
    SmallVectorImpl<Type *> &ArgTys,
    SmallVectorImpl<DeferredIntrinsicMatchPair> &DeferredChecks,
    bool IsDeferredCheck) {
  using namespace Intrinsic;

  // If we ran out of descriptors, there are too many arguments.
  if (Infos.empty()) return true;

  // Do this before slicing off the 'front' part
  auto InfosRef = Infos;
  auto DeferCheck = [&DeferredChecks, &InfosRef](Type *T) {
    DeferredChecks.emplace_back(T, InfosRef);
    return false;
  };

  IITDescriptor D = Infos.front();
  Infos = Infos.slice(1);

  switch (D.Kind) {
    case IITDescriptor::Void: return !Ty->isVoidTy();
    case IITDescriptor::VarArg: return true;
    case IITDescriptor::MMX:  return !Ty->isX86_MMXTy();
    case IITDescriptor::AMX:  return !Ty->isX86_AMXTy();
    case IITDescriptor::Token: return !Ty->isTokenTy();
    case IITDescriptor::Metadata: return !Ty->isMetadataTy();
    case IITDescriptor::Half: return !Ty->isHalfTy();
    case IITDescriptor::BFloat: return !Ty->isBFloatTy();
    case IITDescriptor::Float: return !Ty->isFloatTy();
    case IITDescriptor::Double: return !Ty->isDoubleTy();
    case IITDescriptor::Quad: return !Ty->isFP128Ty();
    case IITDescriptor::PPCQuad: return !Ty->isPPC_FP128Ty();
    case IITDescriptor::Integer: return !Ty->isIntegerTy(D.Integer_Width);
    case IITDescriptor::AArch64Svcount:
      return !isa<TargetExtType>(Ty) ||
             cast<TargetExtType>(Ty)->getName() != "aarch64.svcount";
    case IITDescriptor::Vector: {
      VectorType *VT = dyn_cast<VectorType>(Ty);
      return !VT || VT->getElementCount() != D.Vector_Width ||
             matchIntrinsicType(VT->getElementType(), Infos, ArgTys,
                                DeferredChecks, IsDeferredCheck);
    }
    case IITDescriptor::Pointer: {
      PointerType *PT = dyn_cast<PointerType>(Ty);
      return !PT || PT->getAddressSpace() != D.Pointer_AddressSpace;
    }

    case IITDescriptor::Struct: {
      StructType *ST = dyn_cast<StructType>(Ty);
      if (!ST || !ST->isLiteral() || ST->isPacked() ||
          ST->getNumElements() != D.Struct_NumElements)
        return true;

      for (unsigned i = 0, e = D.Struct_NumElements; i != e; ++i)
        if (matchIntrinsicType(ST->getElementType(i), Infos, ArgTys,
                               DeferredChecks, IsDeferredCheck))
          return true;
      return false;
    }

    case IITDescriptor::Argument:
      // If this is the second occurrence of an argument,
      // verify that the later instance matches the previous instance.
      if (D.getArgumentNumber() < ArgTys.size())
        return Ty != ArgTys[D.getArgumentNumber()];

      if (D.getArgumentNumber() > ArgTys.size() ||
          D.getArgumentKind() == IITDescriptor::AK_MatchType)
        return IsDeferredCheck || DeferCheck(Ty);

      assert(D.getArgumentNumber() == ArgTys.size() && !IsDeferredCheck &&
             "Table consistency error");
      ArgTys.push_back(Ty);

      switch (D.getArgumentKind()) {
        case IITDescriptor::AK_Any:        return false; // Success
        case IITDescriptor::AK_AnyInteger: return !Ty->isIntOrIntVectorTy();
        case IITDescriptor::AK_AnyFloat:   return !Ty->isFPOrFPVectorTy();
        case IITDescriptor::AK_AnyVector:  return !isa<VectorType>(Ty);
        case IITDescriptor::AK_AnyPointer: return !isa<PointerType>(Ty);
        default:                           break;
      }
      llvm_unreachable("all argument kinds not covered");

    case IITDescriptor::ExtendArgument: {
      // If this is a forward reference, defer the check for later.
      if (D.getArgumentNumber() >= ArgTys.size())
        return IsDeferredCheck || DeferCheck(Ty);

      Type *NewTy = ArgTys[D.getArgumentNumber()];
      if (VectorType *VTy = dyn_cast<VectorType>(NewTy))
        NewTy = VectorType::getExtendedElementVectorType(VTy);
      else if (IntegerType *ITy = dyn_cast<IntegerType>(NewTy))
        NewTy = IntegerType::get(ITy->getContext(), 2 * ITy->getBitWidth());
      else
        return true;

      return Ty != NewTy;
    }
    case IITDescriptor::TruncArgument: {
      // If this is a forward reference, defer the check for later.
      if (D.getArgumentNumber() >= ArgTys.size())
        return IsDeferredCheck || DeferCheck(Ty);

      Type *NewTy = ArgTys[D.getArgumentNumber()];
      if (VectorType *VTy = dyn_cast<VectorType>(NewTy))
        NewTy = VectorType::getTruncatedElementVectorType(VTy);
      else if (IntegerType *ITy = dyn_cast<IntegerType>(NewTy))
        NewTy = IntegerType::get(ITy->getContext(), ITy->getBitWidth() / 2);
      else
        return true;

      return Ty != NewTy;
    }
    case IITDescriptor::HalfVecArgument:
      // If this is a forward reference, defer the check for later.
      if (D.getArgumentNumber() >= ArgTys.size())
        return IsDeferredCheck || DeferCheck(Ty);
      return !isa<VectorType>(ArgTys[D.getArgumentNumber()]) ||
             VectorType::getHalfElementsVectorType(
                     cast<VectorType>(ArgTys[D.getArgumentNumber()])) != Ty;
    case IITDescriptor::SameVecWidthArgument: {
      if (D.getArgumentNumber() >= ArgTys.size()) {
        // Defer check and subsequent check for the vector element type.
        Infos = Infos.slice(1);
        return IsDeferredCheck || DeferCheck(Ty);
      }
      auto *ReferenceType = dyn_cast<VectorType>(ArgTys[D.getArgumentNumber()]);
      auto *ThisArgType = dyn_cast<VectorType>(Ty);
      // Both must be vectors of the same number of elements or neither.
      if ((ReferenceType != nullptr) != (ThisArgType != nullptr))
        return true;
      Type *EltTy = Ty;
      if (ThisArgType) {
        if (ReferenceType->getElementCount() !=
            ThisArgType->getElementCount())
          return true;
        EltTy = ThisArgType->getElementType();
      }
      return matchIntrinsicType(EltTy, Infos, ArgTys, DeferredChecks,
                                IsDeferredCheck);
    }
    case IITDescriptor::VecOfAnyPtrsToElt: {
      unsigned RefArgNumber = D.getRefArgNumber();
      if (RefArgNumber >= ArgTys.size()) {
        if (IsDeferredCheck)
          return true;
        // If forward referencing, already add the pointer-vector type and
        // defer the checks for later.
        ArgTys.push_back(Ty);
        return DeferCheck(Ty);
      }

      if (!IsDeferredCheck){
        assert(D.getOverloadArgNumber() == ArgTys.size() &&
               "Table consistency error");
        ArgTys.push_back(Ty);
      }

      // Verify the overloaded type "matches" the Ref type.
      // i.e. Ty is a vector with the same width as Ref.
      // Composed of pointers to the same element type as Ref.
      auto *ReferenceType = dyn_cast<VectorType>(ArgTys[RefArgNumber]);
      auto *ThisArgVecTy = dyn_cast<VectorType>(Ty);
      if (!ThisArgVecTy || !ReferenceType ||
          (ReferenceType->getElementCount() != ThisArgVecTy->getElementCount()))
        return true;
      return !ThisArgVecTy->getElementType()->isPointerTy();
    }
    case IITDescriptor::VecElementArgument: {
      if (D.getArgumentNumber() >= ArgTys.size())
        return IsDeferredCheck ? true : DeferCheck(Ty);
      auto *ReferenceType = dyn_cast<VectorType>(ArgTys[D.getArgumentNumber()]);
      return !ReferenceType || Ty != ReferenceType->getElementType();
    }
    case IITDescriptor::Subdivide2Argument:
    case IITDescriptor::Subdivide4Argument: {
      // If this is a forward reference, defer the check for later.
      if (D.getArgumentNumber() >= ArgTys.size())
        return IsDeferredCheck || DeferCheck(Ty);

      Type *NewTy = ArgTys[D.getArgumentNumber()];
      if (auto *VTy = dyn_cast<VectorType>(NewTy)) {
        int SubDivs = D.Kind == IITDescriptor::Subdivide2Argument ? 1 : 2;
        NewTy = VectorType::getSubdividedVectorType(VTy, SubDivs);
        return Ty != NewTy;
      }
      return true;
    }
    case IITDescriptor::VecOfBitcastsToInt: {
      if (D.getArgumentNumber() >= ArgTys.size())
        return IsDeferredCheck || DeferCheck(Ty);
      auto *ReferenceType = dyn_cast<VectorType>(ArgTys[D.getArgumentNumber()]);
      auto *ThisArgVecTy = dyn_cast<VectorType>(Ty);
      if (!ThisArgVecTy || !ReferenceType)
        return true;
      return ThisArgVecTy != VectorType::getInteger(ReferenceType);
    }
  }
  llvm_unreachable("unhandled");
}

Intrinsic::MatchIntrinsicTypesResult
Intrinsic::matchIntrinsicSignature(FunctionType *FTy,
                                   ArrayRef<Intrinsic::IITDescriptor> &Infos,
                                   SmallVectorImpl<Type *> &ArgTys) {
  SmallVector<DeferredIntrinsicMatchPair, 2> DeferredChecks;
  if (matchIntrinsicType(FTy->getReturnType(), Infos, ArgTys, DeferredChecks,
                         false))
    return MatchIntrinsicTypes_NoMatchRet;

  unsigned NumDeferredReturnChecks = DeferredChecks.size();

  for (auto *Ty : FTy->params())
    if (matchIntrinsicType(Ty, Infos, ArgTys, DeferredChecks, false))
      return MatchIntrinsicTypes_NoMatchArg;

  for (unsigned I = 0, E = DeferredChecks.size(); I != E; ++I) {
    DeferredIntrinsicMatchPair &Check = DeferredChecks[I];
    if (matchIntrinsicType(Check.first, Check.second, ArgTys, DeferredChecks,
                           true))
      return I < NumDeferredReturnChecks ? MatchIntrinsicTypes_NoMatchRet
                                         : MatchIntrinsicTypes_NoMatchArg;
  }

  return MatchIntrinsicTypes_Match;
}

bool
Intrinsic::matchIntrinsicVarArg(bool isVarArg,
                                ArrayRef<Intrinsic::IITDescriptor> &Infos) {
  // If there are no descriptors left, then it can't be a vararg.
  if (Infos.empty())
    return isVarArg;

  // There should be only one descriptor remaining at this point.
  if (Infos.size() != 1)
    return true;

  // Check and verify the descriptor.
  IITDescriptor D = Infos.front();
  Infos = Infos.slice(1);
  if (D.Kind == IITDescriptor::VarArg)
    return !isVarArg;

  return true;
}

bool Intrinsic::getIntrinsicSignature(Intrinsic::ID ID, FunctionType *FT,
                                      SmallVectorImpl<Type *> &ArgTys) {
  if (!ID)
    return false;

  SmallVector<Intrinsic::IITDescriptor, 8> Table;
  getIntrinsicInfoTableEntries(ID, Table);
  ArrayRef<Intrinsic::IITDescriptor> TableRef = Table;

  if (Intrinsic::matchIntrinsicSignature(FT, TableRef, ArgTys) !=
      Intrinsic::MatchIntrinsicTypesResult::MatchIntrinsicTypes_Match) {
    return false;
  }
  if (Intrinsic::matchIntrinsicVarArg(FT->isVarArg(), TableRef))
    return false;
  return true;
}

bool Intrinsic::getIntrinsicSignature(Function *F,
                                      SmallVectorImpl<Type *> &ArgTys) {
  return getIntrinsicSignature(F->getIntrinsicID(), F->getFunctionType(),
                               ArgTys);
}

std::optional<Function *> Intrinsic::remangleIntrinsicFunction(Function *F) {
  SmallVector<Type *, 4> ArgTys;
  if (!getIntrinsicSignature(F, ArgTys))
    return std::nullopt;

  Intrinsic::ID ID = F->getIntrinsicID();
  StringRef Name = F->getName();
  std::string WantedName =
      Intrinsic::getName(ID, ArgTys, F->getParent(), F->getFunctionType());
  if (Name == WantedName)
    return std::nullopt;

  Function *NewDecl = [&] {
    if (auto *ExistingGV = F->getParent()->getNamedValue(WantedName)) {
      if (auto *ExistingF = dyn_cast<Function>(ExistingGV))
        if (ExistingF->getFunctionType() == F->getFunctionType())
          return ExistingF;

      // The name already exists, but is not a function or has the wrong
      // prototype. Make place for the new one by renaming the old version.
      // Either this old version will be removed later on or the module is
      // invalid and we'll get an error.
      ExistingGV->setName(WantedName + ".renamed");
    }
    return Intrinsic::getDeclaration(F->getParent(), ID, ArgTys);
  }();

  NewDecl->setCallingConv(F->getCallingConv());
  assert(NewDecl->getFunctionType() == F->getFunctionType() &&
         "Shouldn't change the signature");
  return NewDecl;
}

/// hasAddressTaken - returns true if there are any uses of this function
/// other than direct calls or invokes to it. Optionally ignores callback
/// uses, assume like pointer annotation calls, and references in llvm.used
/// and llvm.compiler.used variables.
bool Function::hasAddressTaken(const User **PutOffender,
                               bool IgnoreCallbackUses,
                               bool IgnoreAssumeLikeCalls, bool IgnoreLLVMUsed,
                               bool IgnoreARCAttachedCall,
                               bool IgnoreCastedDirectCall) const {
  for (const Use &U : uses()) {
    const User *FU = U.getUser();
    if (isa<BlockAddress>(FU))
      continue;

    if (IgnoreCallbackUses) {
      AbstractCallSite ACS(&U);
      if (ACS && ACS.isCallbackCall())
        continue;
    }

    const auto *Call = dyn_cast<CallBase>(FU);
    if (!Call) {
      if (IgnoreAssumeLikeCalls &&
          isa<BitCastOperator, AddrSpaceCastOperator>(FU) &&
          all_of(FU->users(), [](const User *U) {
            if (const auto *I = dyn_cast<IntrinsicInst>(U))
              return I->isAssumeLikeIntrinsic();
            return false;
          })) {
        continue;
      }

      if (IgnoreLLVMUsed && !FU->user_empty()) {
        const User *FUU = FU;
        if (isa<BitCastOperator, AddrSpaceCastOperator>(FU) &&
            FU->hasOneUse() && !FU->user_begin()->user_empty())
          FUU = *FU->user_begin();
        if (llvm::all_of(FUU->users(), [](const User *U) {
              if (const auto *GV = dyn_cast<GlobalVariable>(U))
                return GV->hasName() &&
                       (GV->getName() == "llvm.compiler.used" ||
                        GV->getName() == "llvm.used");
              return false;
            }))
          continue;
      }
      if (PutOffender)
        *PutOffender = FU;
      return true;
    }

    if (IgnoreAssumeLikeCalls) {
      if (const auto *I = dyn_cast<IntrinsicInst>(Call))
        if (I->isAssumeLikeIntrinsic())
          continue;
    }

    if (!Call->isCallee(&U) || (!IgnoreCastedDirectCall &&
                                Call->getFunctionType() != getFunctionType())) {
      if (IgnoreARCAttachedCall &&
          Call->isOperandBundleOfType(LLVMContext::OB_clang_arc_attachedcall,
                                      U.getOperandNo()))
        continue;

      if (PutOffender)
        *PutOffender = FU;
      return true;
    }
  }
  return false;
}

bool Function::isDefTriviallyDead() const {
  // Check the linkage
  if (!hasLinkOnceLinkage() && !hasLocalLinkage() &&
      !hasAvailableExternallyLinkage())
    return false;

  // Check if the function is used by anything other than a blockaddress.
  for (const User *U : users())
    if (!isa<BlockAddress>(U))
      return false;

  return true;
}

/// callsFunctionThatReturnsTwice - Return true if the function has a call to
/// setjmp or other function that gcc recognizes as "returning twice".
bool Function::callsFunctionThatReturnsTwice() const {
  for (const Instruction &I : instructions(this))
    if (const auto *Call = dyn_cast<CallBase>(&I))
      if (Call->hasFnAttr(Attribute::ReturnsTwice))
        return true;

  return false;
}

Constant *Function::getPersonalityFn() const {
  assert(hasPersonalityFn() && getNumOperands());
  return cast<Constant>(Op<0>());
}

void Function::setPersonalityFn(Constant *Fn) {
  setHungoffOperand<0>(Fn);
  setValueSubclassDataBit(3, Fn != nullptr);
}

Constant *Function::getPrefixData() const {
  assert(hasPrefixData() && getNumOperands());
  return cast<Constant>(Op<1>());
}

void Function::setPrefixData(Constant *PrefixData) {
  setHungoffOperand<1>(PrefixData);
  setValueSubclassDataBit(1, PrefixData != nullptr);
}

Constant *Function::getPrologueData() const {
  assert(hasPrologueData() && getNumOperands());
  return cast<Constant>(Op<2>());
}

void Function::setPrologueData(Constant *PrologueData) {
  setHungoffOperand<2>(PrologueData);
  setValueSubclassDataBit(2, PrologueData != nullptr);
}

void Function::allocHungoffUselist() {
  // If we've already allocated a uselist, stop here.
  if (getNumOperands())
    return;

  allocHungoffUses(3, /*IsPhi=*/ false);
  setNumHungOffUseOperands(3);

  // Initialize the uselist with placeholder operands to allow traversal.
  auto *CPN = ConstantPointerNull::get(PointerType::get(getContext(), 0));
  Op<0>().set(CPN);
  Op<1>().set(CPN);
  Op<2>().set(CPN);
}

template <int Idx>
void Function::setHungoffOperand(Constant *C) {
  if (C) {
    allocHungoffUselist();
    Op<Idx>().set(C);
  } else if (getNumOperands()) {
    Op<Idx>().set(ConstantPointerNull::get(PointerType::get(getContext(), 0)));
  }
}

void Function::setValueSubclassDataBit(unsigned Bit, bool On) {
  assert(Bit < 16 && "SubclassData contains only 16 bits");
  if (On)
    setValueSubclassData(getSubclassDataFromValue() | (1 << Bit));
  else
    setValueSubclassData(getSubclassDataFromValue() & ~(1 << Bit));
}

void Function::setEntryCount(ProfileCount Count,
                             const DenseSet<GlobalValue::GUID> *S) {
#if !defined(NDEBUG)
  auto PrevCount = getEntryCount();
  assert(!PrevCount || PrevCount->getType() == Count.getType());
#endif

  auto ImportGUIDs = getImportGUIDs();
  if (S == nullptr && ImportGUIDs.size())
    S = &ImportGUIDs;

  MDBuilder MDB(getContext());
  setMetadata(
      LLVMContext::MD_prof,
      MDB.createFunctionEntryCount(Count.getCount(), Count.isSynthetic(), S));
}

void Function::setEntryCount(uint64_t Count, Function::ProfileCountType Type,
                             const DenseSet<GlobalValue::GUID> *Imports) {
  setEntryCount(ProfileCount(Count, Type), Imports);
}

std::optional<ProfileCount> Function::getEntryCount(bool AllowSynthetic) const {
  MDNode *MD = getMetadata(LLVMContext::MD_prof);
  if (MD && MD->getOperand(0))
    if (MDString *MDS = dyn_cast<MDString>(MD->getOperand(0))) {
      if (MDS->getString() == "function_entry_count") {
        ConstantInt *CI = mdconst::extract<ConstantInt>(MD->getOperand(1));
        uint64_t Count = CI->getValue().getZExtValue();
        // A value of -1 is used for SamplePGO when there were no samples.
        // Treat this the same as unknown.
        if (Count == (uint64_t)-1)
          return std::nullopt;
        return ProfileCount(Count, PCT_Real);
      } else if (AllowSynthetic &&
                 MDS->getString() == "synthetic_function_entry_count") {
        ConstantInt *CI = mdconst::extract<ConstantInt>(MD->getOperand(1));
        uint64_t Count = CI->getValue().getZExtValue();
        return ProfileCount(Count, PCT_Synthetic);
      }
    }
  return std::nullopt;
}

DenseSet<GlobalValue::GUID> Function::getImportGUIDs() const {
  DenseSet<GlobalValue::GUID> R;
  if (MDNode *MD = getMetadata(LLVMContext::MD_prof))
    if (MDString *MDS = dyn_cast<MDString>(MD->getOperand(0)))
      if (MDS->getString() == "function_entry_count")
        for (unsigned i = 2; i < MD->getNumOperands(); i++)
          R.insert(mdconst::extract<ConstantInt>(MD->getOperand(i))
                       ->getValue()
                       .getZExtValue());
  return R;
}

void Function::setSectionPrefix(StringRef Prefix) {
  MDBuilder MDB(getContext());
  setMetadata(LLVMContext::MD_section_prefix,
              MDB.createFunctionSectionPrefix(Prefix));
}

std::optional<StringRef> Function::getSectionPrefix() const {
  if (MDNode *MD = getMetadata(LLVMContext::MD_section_prefix)) {
    assert(cast<MDString>(MD->getOperand(0))->getString() ==
               "function_section_prefix" &&
           "Metadata not match");
    return cast<MDString>(MD->getOperand(1))->getString();
  }
  return std::nullopt;
}

bool Function::nullPointerIsDefined() const {
  return hasFnAttribute(Attribute::NullPointerIsValid);
}

bool llvm::NullPointerIsDefined(const Function *F, unsigned AS) {
  if (F && F->nullPointerIsDefined())
    return true;

  if (AS != 0)
    return true;

  return false;
}

bool Intrinsic::isIntrinsicSyncFree(const Function &F) {
  switch (F.getIntrinsicID()) {
    // --- Intrinsics that are NOT sync-free ---

    // Atomics & Element-wise Atomics
    // These are by definition synchronization primitives or involve atomic operations.
    case Intrinsic::memcpy_element_unordered_atomic:
    case Intrinsic::memmove_element_unordered_atomic:
    case Intrinsic::memset_element_unordered_atomic:
    // Generic atomicrmw, cmpxchg are often instructions. If specific load/store/rmw/cmpxchg
    // intrinsics existed as llvm.atomic.*, they would be here.
    // Many target-specific atomics (e.g., for ARM, X86) would also be non-sync-free.

    // Memory Barriers / Fences
    case Intrinsic::arithmetic_fence: // Orders arithmetic operations.
    // A generic llvm.fence intrinsic doesn't exist; fences are usually instructions or target-specific.

    // Coroutine related intrinsics
    // Many involve suspension, resumption, or management of shared coroutine state.
    case Intrinsic::coro_alloc: case Intrinsic::coro_alloca_alloc:
    case Intrinsic::coro_alloca_free: case Intrinsic::coro_alloca_get:
    case Intrinsic::coro_async_context_alloc:
    case Intrinsic::coro_async_context_dealloc:
    case Intrinsic::coro_async_resume: case Intrinsic::coro_async_size_replace:
    case Intrinsic::coro_await_suspend_bool:
    case Intrinsic::coro_await_suspend_handle:
    case Intrinsic::coro_await_suspend_void:
    case Intrinsic::coro_begin: case Intrinsic::coro_destroy:
    case Intrinsic::coro_end: // Marks end, might involve cleanup/signaling
    case Intrinsic::coro_end_async: case Intrinsic::coro_free:
    case Intrinsic::coro_id_async: // May interact with runtime for ID generation/lookup
    case Intrinsic::coro_id_retcon: case Intrinsic::coro_id_retcon_once:
    case Intrinsic::coro_prepare_async: case Intrinsic::coro_prepare_retcon:
    case Intrinsic::coro_resume: case Intrinsic::coro_save:
    case Intrinsic::coro_suspend: case Intrinsic::coro_suspend_async:
    case Intrinsic::coro_suspend_retcon:
    // coro_frame, coro_promise, coro_id, coro_size, coro_done, coro_noop are generally okay,
    // but the ones involving allocation, suspension, or async management are suspect.

    // Exception Handling (EH) related intrinsics
    // These interact with runtime systems for unwinding and dispatch.
    case Intrinsic::callbr_landingpad:
    case Intrinsic::eh_dwarf_cfa: // Interacts with unwind information
    case Intrinsic::eh_exceptioncode: case Intrinsic::eh_exceptionpointer:
    case Intrinsic::eh_recoverfp: case Intrinsic::eh_return_i32:
    case Intrinsic::eh_return_i64: case Intrinsic::eh_sjlj_callsite:
    case Intrinsic::eh_sjlj_functioncontext: case Intrinsic::eh_sjlj_longjmp:
    case Intrinsic::eh_sjlj_lsda: case Intrinsic::eh_sjlj_setjmp:
    case Intrinsic::eh_sjlj_setup_dispatch: case Intrinsic::eh_typeid_for:
    case Intrinsic::eh_unwind_init:
    case Intrinsic::seh_scope_begin: // Structured Exception Handling (Windows)
    case Intrinsic::seh_scope_end: case Intrinsic::seh_try_begin:
    case Intrinsic::seh_try_end:


    // Garbage Collection (GC) related intrinsics
    case Intrinsic::experimental_gc_get_pointer_base:
    case Intrinsic::experimental_gc_get_pointer_offset:
    case Intrinsic::experimental_gc_relocate:
    case Intrinsic::experimental_gc_result:
    case Intrinsic::experimental_gc_statepoint:
    case Intrinsic::gcread:  // Might involve read barriers
    case Intrinsic::gcroot:  // Modifies GC root set
    case Intrinsic::gcwrite: // Might involve write barriers

    // Objective-C ARC and Synchronization Intrinsics
    case Intrinsic::objc_arc_annotation_bottomup_bbend: // Annotations, likely passive
    case Intrinsic::objc_arc_annotation_bottomup_bbstart:
    case Intrinsic::objc_arc_annotation_topdown_bbend:
    case Intrinsic::objc_arc_annotation_topdown_bbstart:
    case Intrinsic::objc_autorelease: case Intrinsic::objc_autoreleasePoolPop:
    case Intrinsic::objc_autoreleasePoolPush:
    case Intrinsic::objc_autoreleaseReturnValue:
    case Intrinsic::objc_copyWeak: case Intrinsic::objc_destroyWeak:
    case Intrinsic::objc_initWeak: case Intrinsic::objc_loadWeak:
    case Intrinsic::objc_loadWeakRetained: case Intrinsic::objc_moveWeak:
    case Intrinsic::objc_release: case Intrinsic::objc_retain:
    case Intrinsic::objc_retain_autorelease:
    case Intrinsic::objc_retainAutorelease:
    case Intrinsic::objc_retainAutoreleaseReturnValue:
    case Intrinsic::objc_retainAutoreleasedReturnValue:
    case Intrinsic::objc_retainBlock: case Intrinsic::objc_storeStrong:
    case Intrinsic::objc_storeWeak:
    case Intrinsic::objc_sync_enter: // Explicit lock
    case Intrinsic::objc_sync_exit:  // Explicit unlock
    case Intrinsic::objc_unsafeClaimAutoreleasedReturnValue:
    // objc_clang_arc_noop_use, objc_clang_arc_use, objc_retainedObject,
    // objc_unretainedObject, objc_unretainedPointer are generally okay.

    // Thread Local Storage
    case Intrinsic::threadlocal_address: // Getting address may involve runtime calls.

    // Other system/runtime interaction, stack manipulation, or control flow
    case Intrinsic::addressofreturnaddress: case Intrinsic::adjust_trampoline:
    case Intrinsic::clear_cache: // System-level operation affecting global state.
    case Intrinsic::frameaddress: case Intrinsic::init_trampoline:
    case Intrinsic::localescape:  // For non-local gotos.
    case Intrinsic::localrecover: // For non-local gotos.
    case Intrinsic::read_register: // Accessing global machine state.
    case Intrinsic::read_volatile_register: // Accessing global, volatile machine state.
    case Intrinsic::returnaddress:
    case Intrinsic::stackguard:     // May involve checks against global canary.
    case Intrinsic::stackprotector: // Setup/check stack canary.
    case Intrinsic::stackrestore: case Intrinsic::stacksave:
    case Intrinsic::thread_pointer: // Accessing thread-specific global pointer.
    case Intrinsic::write_register: // Modifying global machine state.

    // Sanitizer checks and related operations
    // These call into runtime libraries which can be synchronized.
    case Intrinsic::asan_check_memaccess:
    case Intrinsic::hwasan_check_memaccess:
    case Intrinsic::hwasan_check_memaccess_fixedshadow:
    case Intrinsic::hwasan_check_memaccess_shortgranules:
    case Intrinsic::hwasan_check_memaccess_shortgranules_fixedshadow:
    case Intrinsic::allow_runtime_check: case Intrinsic::allow_ubsan_check:
    case Intrinsic::ubsantrap: // Often calls a runtime handler.
    case Intrinsic::trap:      // Usually results in termination or debugger, a strong sync point.
    case Intrinsic::debugtrap: // Similar to trap.

    // Experimental features for managed runtimes or complex control flow
    case Intrinsic::experimental_deoptimize:
    case Intrinsic::experimental_patchpoint:
    case Intrinsic::experimental_patchpoint_void:
    case Intrinsic::experimental_stackmap:
    case Intrinsic::experimental_guard: // May involve runtime checks or side effects.

    // Floating point environment access - can be global or thread-global state.
    case Intrinsic::get_fpenv: case Intrinsic::set_fpenv:
    case Intrinsic::reset_fpenv:
    case Intrinsic::get_fpmode: // Less likely global sync, but conservative.
    case Intrinsic::set_fpmode: case Intrinsic::reset_fpmode:
    case Intrinsic::get_rounding: // Could be global.
    case Intrinsic::set_rounding:

    // Pointer Authentication - may involve system keys or special registers
    case Intrinsic::ptrauth_auth: case Intrinsic::ptrauth_blend:
    case Intrinsic::ptrauth_resign: case Intrinsic::ptrauth_sign:
    case Intrinsic::ptrauth_sign_generic: case Intrinsic::ptrauth_strip:

    // Profiling and Coverage
    // These often write to global data structures.
    case Intrinsic::instrprof_callsite: case Intrinsic::instrprof_cover:
    case Intrinsic::instrprof_increment:
    case Intrinsic::instrprof_increment_step:
    case Intrinsic::instrprof_mcdc_condbitmap_update:
    case Intrinsic::instrprof_mcdc_parameters: // Might just be setup
    case Intrinsic::instrprof_mcdc_tvbitmap_update:
    case Intrinsic::instrprof_timestamp:
    case Intrinsic::instrprof_value_profile:
    case Intrinsic::pseudoprobe: // For AutoFDO, interacts with profiling data.
    case Intrinsic::xray_customevent: // XRay tracing system calls.
    case Intrinsic::xray_typedevent:

    // Other special control flow / side effects
    case Intrinsic::icall_branch_funnel: // Complex control flow, potentially for speculation.
    case Intrinsic::sponentry: // Shadow stack related.

      return false; // These intrinsics are NOT sync-free.

    // --- Intrinsics that ARE generally sync-free ---
    // This includes most:
    // - Pure mathematical operations (sin, cos, sqrt, pow, log, etc.)
    // - Bitwise operations (bswap, ctlz, ctpop, cttz)
    // - Simple type conversions (fptosi, sitofp, fpext, fptrunc, bitcast)
    // - Vector operations (shuffles, inserts, extracts, reductions if not on shared memory)
    // - Memory operations like llvm.memcpy, llvm.memmove, llvm.memset (non-atomic versions)
    // - Optimizer hints (assume, expect, lifetime.start/end, invariant.start/end)
    // - Debug information (dbg.declare, dbg.value, dbg.label)
    // - Simple comparisons
    // - Many target-specific SIMD computational intrinsics.

    // Explicitly listing all ~13000 sync-free intrinsics is impractical.
    // The default case handles them.

    // Examples of definitely sync-free (for clarity, not exhaustive):
    /*
    case Intrinsic::abs: case Intrinsic::assume: case Intrinsic::bitreverse:
    case Intrinsic::bswap:
    case Intrinsic::call_preallocated_arg: // Argument passing mechanism
    case Intrinsic::call_preallocated_setup:
    case Intrinsic::call_preallocated_teardown:
    case Intrinsic::canonicalize: case Intrinsic::ceil:
    case Intrinsic::copysign: case Intrinsic::cos: case Intrinsic::ctlz:
    case Intrinsic::ctpop: case Intrinsic::cttz: case Intrinsic::dbg_assign:
    case Intrinsic::dbg_declare: case Intrinsic::dbg_label:
    case Intrinsic::dbg_value: case Intrinsic::donothing:
    case Intrinsic::exp: case Intrinsic::exp10: case Intrinsic::exp2:
    case Intrinsic::expect: case Intrinsic::expect_with_probability:
    case Intrinsic::fabs: case Intrinsic::floor: case Intrinsic::fma:
    case Intrinsic::fmuladd:
    // ... and thousands more ...
    */

    default:
      break;
  }

  // Not named above. Rather than assume the list is complete -- it is not, and
  // could not be kept so across every target's intrinsics -- ask what the
  // intrinsic declares about itself, and treat silence as "may synchronize".
  //
  // Assuming sync-free here was wrong in ways nothing tested:
  // llvm.x86.sse2.mfence and llvm.nvvm.barrier0 are both declared with an empty
  // property list and appear on no blocklist, so a memory fence and a thread
  // barrier were both reported sync-free, and dominance elimination would
  // remove an access across either.
  //
  // nosync alone is far too narrow to use on its own -- only a score of
  // intrinsics carry it, memcpy and memset among those that do not -- so the
  // memory effects carry most of the weight:
  //   - touching no memory at all leaves nothing to synchronize through, which
  //     covers the arithmetic and bit intrinsics;
  //   - touching only the memory passed in as arguments cannot establish
  //     happens-before either, which covers memcpy, memmove and memset. The
  //     element-wise atomic forms are arg-memory-only too, and are blocklisted
  //     above for exactly that reason.
  if (F.hasFnAttribute(Attribute::NoSync))
    return true;
  if (F.doesNotAccessMemory())
    return true;
  if (F.onlyAccessesArgMemory())
    return true;

  return false;
}
