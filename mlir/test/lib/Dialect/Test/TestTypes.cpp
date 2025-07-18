//===- TestTypes.cpp - MLIR Test Dialect Types ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains types defined by the TestDialect for testing various
// features of MLIR.
//
//===----------------------------------------------------------------------===//

#include "TestTypes.h"
#include "TestDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/ExtensibleDialect.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/TypeSize.h"
#include <optional>

using namespace mlir;
using namespace test;

// Custom parser for SignednessSemantics.
static ParseResult
parseSignedness(AsmParser &parser,
                TestIntegerType::SignednessSemantics &result) {
  StringRef signStr;
  auto loc = parser.getCurrentLocation();
  if (parser.parseKeyword(&signStr))
    return failure();
  if (signStr.equals_insensitive("u") || signStr.equals_insensitive("unsigned"))
    result = TestIntegerType::SignednessSemantics::Unsigned;
  else if (signStr.equals_insensitive("s") ||
           signStr.equals_insensitive("signed"))
    result = TestIntegerType::SignednessSemantics::Signed;
  else if (signStr.equals_insensitive("n") ||
           signStr.equals_insensitive("none"))
    result = TestIntegerType::SignednessSemantics::Signless;
  else
    return parser.emitError(loc, "expected signed, unsigned, or none");
  return success();
}

// Custom printer for SignednessSemantics.
static void printSignedness(AsmPrinter &printer,
                            const TestIntegerType::SignednessSemantics &ss) {
  switch (ss) {
  case TestIntegerType::SignednessSemantics::Unsigned:
    printer << "unsigned";
    break;
  case TestIntegerType::SignednessSemantics::Signed:
    printer << "signed";
    break;
  case TestIntegerType::SignednessSemantics::Signless:
    printer << "none";
    break;
  }
}

// The functions don't need to be in the header file, but need to be in the mlir
// namespace. Declare them here, then define them immediately below. Separating
// the declaration and definition adheres to the LLVM coding standards.
namespace test {
// FieldInfo is used as part of a parameter, so equality comparison is
// compulsory.
static bool operator==(const FieldInfo &a, const FieldInfo &b);
// FieldInfo is used as part of a parameter, so a hash will be computed.
static llvm::hash_code hash_value(const FieldInfo &fi); // NOLINT
} // namespace test

// FieldInfo is used as part of a parameter, so equality comparison is
// compulsory.
static bool test::operator==(const FieldInfo &a, const FieldInfo &b) {
  return a.name == b.name && a.type == b.type;
}

// FieldInfo is used as part of a parameter, so a hash will be computed.
static llvm::hash_code test::hash_value(const FieldInfo &fi) { // NOLINT
  return llvm::hash_combine(fi.name, fi.type);
}

//===----------------------------------------------------------------------===//
// TestCustomType
//===----------------------------------------------------------------------===//

static ParseResult parseCustomTypeA(AsmParser &parser, int &aResult) {
  return parser.parseInteger(aResult);
}

static void printCustomTypeA(AsmPrinter &printer, int a) { printer << a; }

static ParseResult parseCustomTypeB(AsmParser &parser, int a,
                                    std::optional<int> &bResult) {
  if (a < 0)
    return success();
  for (int i : llvm::seq(0, a))
    if (failed(parser.parseInteger(i)))
      return failure();
  bResult.emplace(0);
  return parser.parseInteger(*bResult);
}

static void printCustomTypeB(AsmPrinter &printer, int a, std::optional<int> b) {
  if (a < 0)
    return;
  printer << ' ';
  for (int i : llvm::seq(0, a))
    printer << i << ' ';
  printer << *b;
}

static ParseResult parseFooString(AsmParser &parser, std::string &foo) {
  std::string result;
  if (parser.parseString(&result))
    return failure();
  foo = std::move(result);
  return success();
}

static void printFooString(AsmPrinter &printer, StringRef foo) {
  printer << '"' << foo << '"';
}

static ParseResult parseBarString(AsmParser &parser, StringRef foo) {
  return parser.parseKeyword(foo);
}

static void printBarString(AsmPrinter &printer, StringRef foo) {
  printer << foo;
}
//===----------------------------------------------------------------------===//
// Tablegen Generated Definitions
//===----------------------------------------------------------------------===//

#include "TestTypeInterfaces.cpp.inc"
#define GET_TYPEDEF_CLASSES
#include "TestTypeDefs.cpp.inc"

//===----------------------------------------------------------------------===//
// CompoundAType
//===----------------------------------------------------------------------===//

Type CompoundAType::parse(AsmParser &parser) {
  int widthOfSomething;
  Type oneType;
  SmallVector<int, 4> arrayOfInts;
  if (parser.parseLess() || parser.parseInteger(widthOfSomething) ||
      parser.parseComma() || parser.parseType(oneType) || parser.parseComma() ||
      parser.parseLSquare())
    return Type();

  int i;
  while (!*parser.parseOptionalInteger(i)) {
    arrayOfInts.push_back(i);
    if (parser.parseOptionalComma())
      break;
  }

  if (parser.parseRSquare() || parser.parseGreater())
    return Type();

  return get(parser.getContext(), widthOfSomething, oneType, arrayOfInts);
}
void CompoundAType::print(AsmPrinter &printer) const {
  printer << "<" << getWidthOfSomething() << ", " << getOneType() << ", [";
  auto intArray = getArrayOfInts();
  llvm::interleaveComma(intArray, printer);
  printer << "]>";
}

//===----------------------------------------------------------------------===//
// TestIntegerType
//===----------------------------------------------------------------------===//

// Example type validity checker.
LogicalResult
TestIntegerType::verify(function_ref<InFlightDiagnostic()> emitError,
                        unsigned width,
                        TestIntegerType::SignednessSemantics ss) {
  if (width > 8)
    return failure();
  return success();
}

Type TestIntegerType::parse(AsmParser &parser) {
  SignednessSemantics signedness;
  int width;
  if (parser.parseLess() || parseSignedness(parser, signedness) ||
      parser.parseComma() || parser.parseInteger(width) ||
      parser.parseGreater())
    return Type();
  Location loc = parser.getEncodedSourceLoc(parser.getNameLoc());
  return getChecked(loc, loc.getContext(), width, signedness);
}

void TestIntegerType::print(AsmPrinter &p) const {
  p << "<";
  printSignedness(p, getSignedness());
  p << ", " << getWidth() << ">";
}

//===----------------------------------------------------------------------===//
// TestStructType
//===----------------------------------------------------------------------===//

Type StructType::parse(AsmParser &p) {
  SmallVector<FieldInfo, 4> parameters;
  if (p.parseLess())
    return Type();
  while (succeeded(p.parseOptionalLBrace())) {
    Type type;
    StringRef name;
    if (p.parseKeyword(&name) || p.parseComma() || p.parseType(type) ||
        p.parseRBrace())
      return Type();
    parameters.push_back(FieldInfo{name, type});
    if (p.parseOptionalComma())
      break;
  }
  if (p.parseGreater())
    return Type();
  return get(p.getContext(), parameters);
}

void StructType::print(AsmPrinter &p) const {
  p << "<";
  llvm::interleaveComma(getFields(), p, [&](const FieldInfo &field) {
    p << "{" << field.name << "," << field.type << "}";
  });
  p << ">";
}

//===----------------------------------------------------------------------===//
// TestType
//===----------------------------------------------------------------------===//

void TestType::printTypeC(Location loc) const {
  emitRemark(loc) << *this << " - TestC";
}

//===----------------------------------------------------------------------===//
// TestTypeWithLayout
//===----------------------------------------------------------------------===//

Type TestTypeWithLayoutType::parse(AsmParser &parser) {
  unsigned val;
  if (parser.parseLess() || parser.parseInteger(val) || parser.parseGreater())
    return Type();
  return TestTypeWithLayoutType::get(parser.getContext(), val);
}

void TestTypeWithLayoutType::print(AsmPrinter &printer) const {
  printer << "<" << getKey() << ">";
}

llvm::TypeSize
TestTypeWithLayoutType::getTypeSizeInBits(const DataLayout &dataLayout,
                                          DataLayoutEntryListRef params) const {
  return llvm::TypeSize::getFixed(extractKind(params, "size"));
}

uint64_t
TestTypeWithLayoutType::getABIAlignment(const DataLayout &dataLayout,
                                        DataLayoutEntryListRef params) const {
  return extractKind(params, "alignment");
}

uint64_t TestTypeWithLayoutType::getPreferredAlignment(
    const DataLayout &dataLayout, DataLayoutEntryListRef params) const {
  return extractKind(params, "preferred");
}

std::optional<uint64_t>
TestTypeWithLayoutType::getIndexBitwidth(const DataLayout &dataLayout,
                                         DataLayoutEntryListRef params) const {
  return extractKind(params, "index");
}

bool TestTypeWithLayoutType::areCompatible(
    DataLayoutEntryListRef oldLayout, DataLayoutEntryListRef newLayout,
    DataLayoutSpecInterface newSpec,
    const DataLayoutIdentifiedEntryMap &map) const {
  unsigned old = extractKind(oldLayout, "alignment");
  return old == 1 || extractKind(newLayout, "alignment") <= old;
}

LogicalResult
TestTypeWithLayoutType::verifyEntries(DataLayoutEntryListRef params,
                                      Location loc) const {
  for (DataLayoutEntryInterface entry : params) {
    // This is for testing purposes only, so assert well-formedness.
    assert(entry.isTypeEntry() && "unexpected identifier entry");
    assert(
        llvm::isa<TestTypeWithLayoutType>(llvm::cast<Type>(entry.getKey())) &&
        "wrong type passed in");
    auto array = llvm::dyn_cast<ArrayAttr>(entry.getValue());
    assert(array && array.getValue().size() == 2 &&
           "expected array of two elements");
    auto kind = llvm::dyn_cast<StringAttr>(array.getValue().front());
    (void)kind;
    assert(kind &&
           (kind.getValue() == "size" || kind.getValue() == "alignment" ||
            kind.getValue() == "preferred" || kind.getValue() == "index") &&
           "unexpected kind");
    assert(llvm::isa<IntegerAttr>(array.getValue().back()));
  }
  return success();
}

uint64_t TestTypeWithLayoutType::extractKind(DataLayoutEntryListRef params,
                                             StringRef expectedKind) const {
  for (DataLayoutEntryInterface entry : params) {
    ArrayRef<Attribute> pair =
        llvm::cast<ArrayAttr>(entry.getValue()).getValue();
    StringRef kind = llvm::cast<StringAttr>(pair.front()).getValue();
    if (kind == expectedKind)
      return llvm::cast<IntegerAttr>(pair.back()).getValue().getZExtValue();
  }
  return 1;
}

//===----------------------------------------------------------------------===//
// Dynamic Types
//===----------------------------------------------------------------------===//

/// Define a singleton dynamic type.
static std::unique_ptr<DynamicTypeDefinition>
getSingletonDynamicType(TestDialect *testDialect) {
  return DynamicTypeDefinition::get(
      "dynamic_singleton", testDialect,
      [](function_ref<InFlightDiagnostic()> emitError,
         ArrayRef<Attribute> args) {
        if (!args.empty()) {
          emitError() << "expected 0 type arguments, but had " << args.size();
          return failure();
        }
        return success();
      });
}

/// Define a dynamic type representing a pair.
static std::unique_ptr<DynamicTypeDefinition>
getPairDynamicType(TestDialect *testDialect) {
  return DynamicTypeDefinition::get(
      "dynamic_pair", testDialect,
      [](function_ref<InFlightDiagnostic()> emitError,
         ArrayRef<Attribute> args) {
        if (args.size() != 2) {
          emitError() << "expected 2 type arguments, but had " << args.size();
          return failure();
        }
        return success();
      });
}

static std::unique_ptr<DynamicTypeDefinition>
getCustomAssemblyFormatDynamicType(TestDialect *testDialect) {
  auto verifier = [](function_ref<InFlightDiagnostic()> emitError,
                     ArrayRef<Attribute> args) {
    if (args.size() != 2) {
      emitError() << "expected 2 type arguments, but had " << args.size();
      return failure();
    }
    return success();
  };

  auto parser = [](AsmParser &parser,
                   llvm::SmallVectorImpl<Attribute> &parsedParams) {
    Attribute leftAttr, rightAttr;
    if (parser.parseLess() || parser.parseAttribute(leftAttr) ||
        parser.parseColon() || parser.parseAttribute(rightAttr) ||
        parser.parseGreater())
      return failure();
    parsedParams.push_back(leftAttr);
    parsedParams.push_back(rightAttr);
    return success();
  };

  auto printer = [](AsmPrinter &printer, ArrayRef<Attribute> params) {
    printer << "<" << params[0] << ":" << params[1] << ">";
  };

  return DynamicTypeDefinition::get("dynamic_custom_assembly_format",
                                    testDialect, std::move(verifier),
                                    std::move(parser), std::move(printer));
}

test::detail::TestCustomStorageCtorTypeStorage *
test::detail::TestCustomStorageCtorTypeStorage::construct(
    mlir::StorageUniquer::StorageAllocator &, std::tuple<int> &&) {
  // Note: this tests linker error ("undefined symbol"), the actual
  // implementation is not important.
  return nullptr;
}

//===----------------------------------------------------------------------===//
// TestDialect
//===----------------------------------------------------------------------===//

namespace {

struct PtrElementModel
    : public LLVM::PointerElementTypeInterface::ExternalModel<PtrElementModel,
                                                              SimpleAType> {};
} // namespace

void TestDialect::registerTypes() {
  addTypes<TestRecursiveType,
#define GET_TYPEDEF_LIST
#include "TestTypeDefs.cpp.inc"
           >();
  SimpleAType::attachInterface<PtrElementModel>(*getContext());

  registerDynamicType(getSingletonDynamicType(this));
  registerDynamicType(getPairDynamicType(this));
  registerDynamicType(getCustomAssemblyFormatDynamicType(this));
}

Type TestDialect::parseType(DialectAsmParser &parser) const {
  StringRef typeTag;
  {
    Type genType;
    auto parseResult = generatedTypeParser(parser, &typeTag, genType);
    if (parseResult.has_value())
      return genType;
  }

  {
    Type dynType;
    auto parseResult = parseOptionalDynamicType(typeTag, parser, dynType);
    if (parseResult.has_value()) {
      if (succeeded(parseResult.value()))
        return dynType;
      return Type();
    }
  }

  if (typeTag != "test_rec") {
    parser.emitError(parser.getNameLoc()) << "unknown type!";
    return Type();
  }

  StringRef name;
  if (parser.parseLess() || parser.parseKeyword(&name))
    return Type();
  auto rec = TestRecursiveType::get(parser.getContext(), name);

  FailureOr<AsmParser::CyclicParseReset> cyclicParse =
      parser.tryStartCyclicParse(rec);

  // If this type already has been parsed above in the stack, expect just the
  // name.
  if (failed(cyclicParse)) {
    if (failed(parser.parseGreater()))
      return Type();
    return rec;
  }

  // Otherwise, parse the body and update the type.
  if (failed(parser.parseComma()))
    return Type();
  Type subtype = parseType(parser);
  if (!subtype || failed(parser.parseGreater()) || failed(rec.setBody(subtype)))
    return Type();

  return rec;
}

void TestDialect::printType(Type type, DialectAsmPrinter &printer) const {
  if (succeeded(generatedTypePrinter(type, printer)))
    return;

  if (succeeded(printIfDynamicType(type, printer)))
    return;

  auto rec = llvm::cast<TestRecursiveType>(type);

  FailureOr<AsmPrinter::CyclicPrintReset> cyclicPrint =
      printer.tryStartCyclicPrint(rec);

  printer << "test_rec<" << rec.getName();
  if (succeeded(cyclicPrint)) {
    printer << ", ";
    printType(rec.getBody(), printer);
  }
  printer << ">";
}

Type TestRecursiveAliasType::getBody() const { return getImpl()->body; }

void TestRecursiveAliasType::setBody(Type type) { (void)Base::mutate(type); }

StringRef TestRecursiveAliasType::getName() const { return getImpl()->name; }

Type TestRecursiveAliasType::parse(AsmParser &parser) {
  StringRef name;
  if (parser.parseLess() || parser.parseKeyword(&name))
    return Type();
  auto rec = TestRecursiveAliasType::get(parser.getContext(), name);

  FailureOr<AsmParser::CyclicParseReset> cyclicParse =
      parser.tryStartCyclicParse(rec);

  // If this type already has been parsed above in the stack, expect just the
  // name.
  if (failed(cyclicParse)) {
    if (failed(parser.parseGreater()))
      return Type();
    return rec;
  }

  // Otherwise, parse the body and update the type.
  if (failed(parser.parseComma()))
    return Type();
  Type subtype;
  if (parser.parseType(subtype))
    return nullptr;
  if (!subtype || failed(parser.parseGreater()))
    return Type();

  rec.setBody(subtype);

  return rec;
}

void TestRecursiveAliasType::print(AsmPrinter &printer) const {

  FailureOr<AsmPrinter::CyclicPrintReset> cyclicPrint =
      printer.tryStartCyclicPrint(*this);

  printer << "<" << getName();
  if (succeeded(cyclicPrint)) {
    printer << ", ";
    printer << getBody();
  }
  printer << ">";
}

void TestTypeOpAsmTypeInterfaceType::getAsmName(
    OpAsmSetNameFn setNameFn) const {
  setNameFn("op_asm_type_interface");
}

::mlir::OpAsmDialectInterface::AliasResult
TestTypeOpAsmTypeInterfaceType::getAlias(::llvm::raw_ostream &os) const {
  os << "op_asm_type_interface_type";
  return ::mlir::OpAsmDialectInterface::AliasResult::FinalAlias;
}

::mlir::FailureOr<::mlir::bufferization::BufferLikeType>
TestTensorType::getBufferType(
    const ::mlir::bufferization::BufferizationOptions &,
    ::llvm::function_ref<::mlir::InFlightDiagnostic()>) {
  return cast<bufferization::BufferLikeType>(
      TestMemrefType::get(getContext(), getShape(), getElementType(), nullptr));
}

::mlir::LogicalResult TestTensorType::verifyCompatibleBufferType(
    ::mlir::bufferization::BufferLikeType bufferType,
    ::llvm::function_ref<::mlir::InFlightDiagnostic()> emitError) {
  auto testMemref = dyn_cast<TestMemrefType>(bufferType);
  if (!testMemref)
    return emitError() << "expected TestMemrefType";

  const bool valid = getShape() == testMemref.getShape() &&
                     getElementType() == testMemref.getElementType();
  return mlir::success(valid);
}
