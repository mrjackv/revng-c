//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <set>

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/TypeSwitch.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"

#include "revng-c/mlir/Dialect/Clift/IR/Clift.h"
#include "revng-c/mlir/Dialect/Clift/IR/CliftAttributes.h"
#include "revng-c/mlir/Dialect/Clift/IR/CliftInterfaces.h"
#include "revng-c/mlir/Dialect/Clift/IR/CliftTypes.h"

#include "CliftParser.h"
#include "CliftStorage.h"
#include "CliftTypeHelpers.h"

// This include should stay here for correct build procedure
//
#define GET_ATTRDEF_CLASSES
#include "revng-c/mlir/Dialect/Clift/IR/CliftAttributes.cpp.inc"

using namespace mlir::clift;

using EmitErrorType = llvm::function_ref<mlir::InFlightDiagnostic()>;

static bool isCompleteType(const mlir::Type Type) {
  if (auto T = mlir::dyn_cast<DefinedType>(Type)) {
    auto Definition = T.getElementType();
    if (auto D = mlir::dyn_cast<StructTypeAttr>(Definition))
      return D.isDefinition();
    if (auto D = mlir::dyn_cast<UnionTypeAttr>(Definition))
      return D.isDefinition();
    return true;
  }

  if (auto T = mlir::dyn_cast<ScalarTupleType>(Type))
    return T.isComplete();

  return true;
}

//===---------------------------- CliftDialect ----------------------------===//

void CliftDialect::registerAttributes() {
  addAttributes<StructTypeAttr, UnionTypeAttr,

  // Include the list of auto-generated attributes
#define GET_ATTRDEF_LIST
#include "revng-c/mlir/Dialect/Clift/IR/CliftAttributes.cpp.inc"
                /* End of auto-generated list */>();
}

/// Parse an attribute registered to this dialect
mlir::Attribute CliftDialect::parseAttribute(mlir::DialectAsmParser &Parser,
                                             mlir::Type Type) const {
  llvm::SMLoc TypeLoc = Parser.getCurrentLocation();

  llvm::StringRef Mnemonic;
  if (mlir::Attribute GenAttr;
      generatedAttributeParser(Parser, &Mnemonic, Type, GenAttr).has_value())
    return GenAttr;

  if (Mnemonic == StructTypeAttr::getMnemonic())
    return StructTypeAttr::parse(Parser);

  if (Mnemonic == UnionTypeAttr::getMnemonic())
    return UnionTypeAttr::parse(Parser);

  Parser.emitError(TypeLoc) << "unknown attr `" << Mnemonic << "` in dialect `"
                            << getNamespace() << "`";
  return {};
}

/// Print an attribute registered to this dialect
void CliftDialect::printAttribute(mlir::Attribute Attr,
                                  mlir::DialectAsmPrinter &Printer) const {

  if (mlir::succeeded(generatedAttributePrinter(Attr, Printer)))
    return;
  if (auto T = mlir::dyn_cast<StructTypeAttr>(Attr)) {
    T.print(Printer);
    return;
  }
  if (auto T = mlir::dyn_cast<UnionTypeAttr>(Attr)) {
    T.print(Printer);
    return;
  }
  revng_abort("cannot print attribute");
}

//===------------------------------ FieldAttr -----------------------------===//

mlir::LogicalResult FieldAttr::verify(EmitErrorType EmitError,
                                      uint64_t Offset,
                                      mlir::Type ElementType,
                                      llvm::StringRef Name) {
  if (auto Definition = mlir::dyn_cast<DefinedType>(ElementType))
    if (mlir::isa<FunctionTypeAttr>(Definition.getElementType()))
      return EmitError() << "Underlying type of field attr cannot be a "
                            "function type";
  clift::ValueType Casted = mlir::dyn_cast<clift::ValueType>(ElementType);
  if (Casted == nullptr) {
    return EmitError() << "Underlying type of a field attr must be a value "
                          "type";
  }
  if (Casted.getByteSize() == 0) {
    return EmitError() << "Field cannot be of zero size";
  }
  return mlir::success();
}

//===---------------------------- EnumFieldAttr ---------------------------===//

mlir::LogicalResult EnumFieldAttr::verify(EmitErrorType EmitError,
                                          uint64_t RawValue,
                                          llvm::StringRef Name) {
  return mlir::success();
}

//===---------------------------- EnumTypeAttr ----------------------------===//

mlir::LogicalResult EnumTypeAttr::verify(EmitErrorType EmitError,
                                         uint64_t ID,
                                         llvm::StringRef Name,
                                         mlir::Type UnderlyingType,
                                         llvm::ArrayRef<EnumFieldAttr> Fields) {
  UnderlyingType = dealias(UnderlyingType);

  if (not UnderlyingType.isa<PrimitiveType>())
    return EmitError() << "type of enum must be a primitive type";

  const auto PrimitiveType = UnderlyingType.cast<clift::PrimitiveType>();
  const uint64_t BitWidth = PrimitiveType.getSize() * 8;

  if (Fields.empty())
    return EmitError() << "enum requires at least one field";

  uint64_t MinValue = 0;
  uint64_t MaxValue = 0;
  bool IsSigned = false;

  switch (PrimitiveType.getKind()) {
  case PrimitiveKind::UnsignedKind:
    MaxValue = llvm::APInt::getMaxValue(BitWidth).getZExtValue();
    break;
  case PrimitiveKind::SignedKind:
    MinValue = llvm::APInt::getSignedMinValue(BitWidth).getSExtValue();
    MaxValue = llvm::APInt::getSignedMaxValue(BitWidth).getSExtValue();
    IsSigned = true;
    break;
  default:
    return EmitError() << "enum underlying type must be an integral type";
  }

  uint64_t LastValue = 0;
  bool CheckEqual = false;

  for (const auto &Field : Fields) {
    const uint64_t Value = Field.getRawValue();

    const auto UsingSigned = [&](auto Callable, const auto... V) {
      return IsSigned ? Callable(static_cast<int64_t>(V)...) : Callable(V...);
    };

    const auto CheckSigned =
      [EmitError](const auto Value,
                  const auto MinValue,
                  const auto MaxValue) -> mlir::LogicalResult {
      if (Value < MinValue)
        return EmitError() << "enum field " << Value
                           << " is less than the min value of the "
                              "underlying type "
                           << MinValue;

      if (Value > MaxValue)
        return EmitError() << "enum field " << Value
                           << " is greater than the max value of the "
                              "underlying type "
                           << MaxValue;

      return mlir::success();
    };

    const mlir::LogicalResult R = UsingSigned(CheckSigned,
                                              Value,
                                              MinValue,
                                              MaxValue);

    if (failed(R))
      return R;

    if (Value < LastValue || (CheckEqual && Value == LastValue))
      return EmitError() << "enum fields must be strictly ordered by their "
                            "unsigned values";

    LastValue = Value;
    CheckEqual = true;
  }

  return mlir::success();
}

uint64_t EnumTypeAttr::getByteSize() const {
  return mlir::cast<PrimitiveType>(getUnderlyingType()).getSize();
}

std::string EnumTypeAttr::getAlias() const {
  return getName().str();
}

//===--------------------------- TypedefTypeAttr --------------------------===//

mlir::LogicalResult TypedefTypeAttr::verify(EmitErrorType EmitError,
                                            uint64_t ID,
                                            llvm::StringRef Name,
                                            clift::ValueType UnderlyingType) {
  return mlir::success();
}

uint64_t TypedefTypeAttr::getByteSize() const {
  return getUnderlyingType().getByteSize();
}

std::string TypedefTypeAttr::getAlias() const {
  return getName().str();
}

//===------------------------ FunctionArgumentAttr ------------------------===//

mlir::LogicalResult FunctionArgumentAttr::verify(EmitErrorType EmitError,
                                                 clift::ValueType Underlying,
                                                 llvm::StringRef Name) {
  if (Underlying.getByteSize() == 0)
    return EmitError() << "type of argument of function cannot be zero size";
  return mlir::success();
}

//===-------------------------- FunctionTypeAttr --------------------------===//

mlir::LogicalResult
FunctionTypeAttr::verify(EmitErrorType EmitError,
                         uint64_t ID,
                         llvm::StringRef Name,
                         clift::ValueType ReturnType,
                         llvm::ArrayRef<FunctionArgumentAttr> Args) {
  if (const auto Type = mlir::dyn_cast<DefinedType>(ReturnType)) {
    if (mlir::isa<FunctionTypeAttr>(Type.getElementType()))
      return EmitError() << "function type cannot return another function type";
  }

  return mlir::success();
}

uint64_t FunctionTypeAttr::getByteSize() const {
  return 0;
}

std::string FunctionTypeAttr::getAlias() const {
  return getName().str();
}

//===----------------------- ScalarTupleElementAttr -----------------------===//

mlir::LogicalResult
ScalarTupleElementAttr::verify(const EmitErrorType EmitError,
                               mlir::Type Type,
                               const llvm::StringRef Name) {
  if (not mlir::isa<clift::ValueType>(Type))
    return EmitError() << "Scalar tuple element type must be a value type";

  return mlir::success();
}

//===--------------------------- StructTypeAttr ---------------------------===//

mlir::LogicalResult StructTypeAttr::verify(EmitErrorType EmitError,
                                           uint64_t ID) {
  return mlir::success();
}

mlir::LogicalResult
StructTypeAttr::verify(const EmitErrorType EmitError,
                       const uint64_t ID,
                       const llvm::StringRef Name,
                       const uint64_t Size,
                       const llvm::ArrayRef<FieldAttr> Fields) {
  if (Size == 0)
    return EmitError() << "struct type cannot have a size of zero";

  if (not Fields.empty()) {
    uint64_t LastEndOffset = 0;

    llvm::SmallSet<llvm::StringRef, 16> NameSet;
    for (const auto &Field : Fields) {
      if (not isCompleteType(Field.getType()))
        return EmitError() << "Fields of structs must be complete types";

      if (Field.getOffset() < LastEndOffset)
        return EmitError() << "Fields of structs must be ordered by offset, "
                              "and "
                              "they cannot overlap";

      LastEndOffset = Field.getOffset()
                      + Field.getType().cast<clift::ValueType>().getByteSize();

      if (not Field.getName().empty()) {
        if (not NameSet.insert(Field.getName()).second)
          return EmitError() << "struct field names must be empty or unique";
      }
    }

    if (LastEndOffset > Size)
      return EmitError() << "offset + size of field of struct type is greater "
                            "than the struct type size.";
  }

  return mlir::success();
}

StructTypeAttr StructTypeAttr::get(MLIRContext *Context, uint64_t ID) {
  return Base::get(Context, ID);
}

StructTypeAttr StructTypeAttr::getChecked(EmitErrorType EmitError,
                                          MLIRContext *Context,
                                          uint64_t ID) {
  return Base::get(Context, ID);
}

StructTypeAttr StructTypeAttr::get(MLIRContext *Context,
                                   uint64_t ID,
                                   llvm::StringRef Name,
                                   uint64_t Size,
                                   llvm::ArrayRef<FieldAttr> Fields) {
  auto Result = Base::get(Context, ID);
  Result.define(Name, Size, Fields);
  return Result;
}

StructTypeAttr StructTypeAttr::getChecked(EmitErrorType EmitError,
                                          MLIRContext *Context,
                                          uint64_t ID,
                                          llvm::StringRef Name,
                                          uint64_t Size,
                                          llvm::ArrayRef<FieldAttr> Fields) {
  if (failed(verify(EmitError, ID, Name, Size, Fields)))
    return {};
  return get(Context, ID, Name, Size, Fields);
}

void StructTypeAttr::define(const llvm::StringRef Name,
                            const uint64_t Size,
                            const llvm::ArrayRef<FieldAttr> Fields) {
  // Call into the base to mutate the type.
  LogicalResult Result = Base::mutate(Name, Fields, Size);

  // Most types expect the mutation to always succeed, but types can implement
  // custom logic for handling mutation failures.
  revng_assert(succeeded(Result)
               && "attempting to change the body of an already-initialized "
                  "type");
}

uint64_t StructTypeAttr::getId() const {
  return getImpl()->getID();
}

llvm::StringRef StructTypeAttr::getName() const {
  return getImpl()->getName();
}

llvm::ArrayRef<FieldAttr> StructTypeAttr::getFields() const {
  return getImpl()->getSubobjects();
}

bool StructTypeAttr::isDefinition() const {
  return getImpl()->isInitialized();
}

uint64_t StructTypeAttr::getByteSize() const {
  return getImpl()->getSize();
}

std::string StructTypeAttr::getAlias() const {
  return getName().str();
}

mlir::Attribute StructTypeAttr::parse(AsmParser &Parser) {
  return parseCompositeType<StructTypeAttr>(Parser, /*MinSubobjects=*/0);
}

void StructTypeAttr::print(AsmPrinter &Printer) const {
  printCompositeType(Printer, *this);
}

void StructTypeAttr::walkImmediateSubElements(function_ref<void(Attribute)>
                                                WalkAttr,
                                              function_ref<void(Type)> WalkType)
  const {
  if (not getImpl()->isInitialized())
    return;
  for (auto Field : getFields())
    WalkAttr(Field);
}

mlir::Attribute
StructTypeAttr::replaceImmediateSubElements(llvm::ArrayRef<mlir::Attribute>,
                                            llvm::ArrayRef<mlir::Type>) const {
  revng_abort("it does not make any sense to replace the elements of a "
              "defined struct");
  return {};
}

//===---------------------------- UnionTypeAttr ---------------------------===//

mlir::LogicalResult UnionTypeAttr::verify(EmitErrorType EmitError,
                                          uint64_t ID) {
  return mlir::success();
}

mlir::LogicalResult UnionTypeAttr::verify(EmitErrorType EmitError,
                                          uint64_t ID,
                                          llvm::StringRef Name,
                                          llvm::ArrayRef<FieldAttr> Fields) {
  if (Fields.empty())
    return EmitError() << "union types must have at least one field";

  llvm::SmallSet<llvm::StringRef, 16> NameSet;
  for (const auto &Field : Fields) {
    if (not isCompleteType(Field.getType()))
      return EmitError() << "Fields of unions must be complete types";

    if (Field.getOffset() != 0)
      return EmitError() << "union field offsets must be zero";

    if (not Field.getName().empty()) {
      if (not NameSet.insert(Field.getName()).second)
        return EmitError() << "union field names must be empty or unique";
    }
  }

  return mlir::success();
}

UnionTypeAttr UnionTypeAttr::get(MLIRContext *Context, uint64_t ID) {
  return Base::get(Context, ID);
}

UnionTypeAttr UnionTypeAttr::getChecked(EmitErrorType EmitError,
                                        MLIRContext *Context,
                                        uint64_t ID) {
  return Base::get(Context, ID);
}

UnionTypeAttr UnionTypeAttr::get(MLIRContext *Context,
                                 uint64_t ID,
                                 llvm::StringRef Name,
                                 llvm::ArrayRef<FieldAttr> Fields) {
  auto Result = Base::get(Context, ID);
  Result.define(Name, Fields);
  return Result;
}

UnionTypeAttr UnionTypeAttr::getChecked(EmitErrorType EmitError,
                                        MLIRContext *Context,
                                        uint64_t ID,
                                        llvm::StringRef Name,
                                        llvm::ArrayRef<FieldAttr> Fields) {
  if (failed(verify(EmitError, ID, Name, Fields)))
    return {};
  return get(Context, ID, Name, Fields);
}

void UnionTypeAttr::define(const llvm::StringRef Name,
                           const llvm::ArrayRef<FieldAttr> Fields) {
  // Call into the base to mutate the type.
  LogicalResult Result = Base::mutate(Name, Fields);

  // Most types expect the mutation to always succeed, but types can implement
  // custom logic for handling mutation failures.
  revng_assert(succeeded(Result)
               && "attempting to change the body of an already-initialized "
                  "type");
}

uint64_t UnionTypeAttr::getId() const {
  return getImpl()->getID();
}

llvm::StringRef UnionTypeAttr::getName() const {
  return getImpl()->getName();
}

llvm::ArrayRef<FieldAttr> UnionTypeAttr::getFields() const {
  return getImpl()->getSubobjects();
}

bool UnionTypeAttr::isDefinition() const {
  return getImpl()->isInitialized();
}

uint64_t UnionTypeAttr::getByteSize() const {
  uint64_t Max = 0;
  for (const auto &Field : getFields()) {
    mlir::Type FieldType = Field.getType();
    uint64_t Size = FieldType.cast<clift::ValueType>().getByteSize();
    Max = Size > Max ? Size : Max;
  }
  return Max;
}

std::string UnionTypeAttr::getAlias() const {
  return getName().str();
}

mlir::Attribute UnionTypeAttr::parse(AsmParser &Parser) {
  return parseCompositeType<UnionTypeAttr>(Parser, /*MinSubobjects=*/1);
}

void UnionTypeAttr::print(AsmPrinter &Printer) const {
  printCompositeType(Printer, *this);
}

void UnionTypeAttr::walkImmediateSubElements(function_ref<void(Attribute)>
                                               WalkAttr,
                                             function_ref<void(Type)> WalkType)
  const {
  if (not getImpl()->isInitialized())
    return;
  for (auto Field : getFields())
    WalkAttr(Field);
}

mlir::Attribute
UnionTypeAttr::replaceImmediateSubElements(llvm::ArrayRef<mlir::Attribute>,
                                           llvm::ArrayRef<mlir::Type>) const {
  revng_abort("it does not make any sense to replace the elements of a "
              "defined union");
  return {};
}
