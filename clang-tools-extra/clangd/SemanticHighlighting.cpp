//===--- SemanticHighlighting.cpp - ------------------------- ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SemanticHighlighting.h"
#include "Config.h"
#include "FindTarget.h"
#include "ParsedAST.h"
#include "Protocol.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/HeuristicResolver.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <optional>

namespace clang {
namespace clangd {
namespace {

/// Get the last Position on a given line.
llvm::Expected<Position> endOfLine(llvm::StringRef Code, int Line) {
  auto StartOfLine = positionToOffset(Code, Position{Line, 0});
  if (!StartOfLine)
    return StartOfLine.takeError();
  StringRef LineText = Code.drop_front(*StartOfLine).take_until([](char C) {
    return C == '\n';
  });
  return Position{Line, static_cast<int>(lspLength(LineText))};
}

/// Some names are not written in the source code and cannot be highlighted,
/// e.g. anonymous classes. This function detects those cases.
bool canHighlightName(DeclarationName Name) {
  switch (Name.getNameKind()) {
  case DeclarationName::Identifier: {
    auto *II = Name.getAsIdentifierInfo();
    return II && !II->getName().empty();
  }
  case DeclarationName::CXXConstructorName:
  case DeclarationName::CXXDestructorName:
    return true;
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
    // Multi-arg selectors need special handling, and we handle 0/1 arg
    // selectors there too.
    return false;
  case DeclarationName::CXXConversionFunctionName:
  case DeclarationName::CXXOperatorName:
  case DeclarationName::CXXDeductionGuideName:
  case DeclarationName::CXXLiteralOperatorName:
  case DeclarationName::CXXUsingDirective:
    return false;
  }
  llvm_unreachable("invalid name kind");
}

bool isUniqueDefinition(const NamedDecl *Decl) {
  if (auto *Func = dyn_cast<FunctionDecl>(Decl))
    return Func->isThisDeclarationADefinition();
  if (auto *Klass = dyn_cast<CXXRecordDecl>(Decl))
    return Klass->isThisDeclarationADefinition();
  if (auto *Iface = dyn_cast<ObjCInterfaceDecl>(Decl))
    return Iface->isThisDeclarationADefinition();
  if (auto *Proto = dyn_cast<ObjCProtocolDecl>(Decl))
    return Proto->isThisDeclarationADefinition();
  if (auto *Var = dyn_cast<VarDecl>(Decl))
    return Var->isThisDeclarationADefinition();
  return isa<TemplateTypeParmDecl>(Decl) ||
         isa<NonTypeTemplateParmDecl>(Decl) ||
         isa<TemplateTemplateParmDecl>(Decl) || isa<ObjCCategoryDecl>(Decl) ||
         isa<ObjCImplDecl>(Decl);
}

std::optional<HighlightingKind> kindForType(const Type *TP,
                                            const HeuristicResolver *Resolver);
std::optional<HighlightingKind> kindForDecl(const NamedDecl *D,
                                            const HeuristicResolver *Resolver) {
  if (auto *USD = dyn_cast<UsingShadowDecl>(D)) {
    if (auto *Target = USD->getTargetDecl())
      D = Target;
  }
  if (auto *TD = dyn_cast<TemplateDecl>(D)) {
    if (auto *Templated = TD->getTemplatedDecl())
      D = Templated;
  }
  if (auto *TD = dyn_cast<TypedefNameDecl>(D)) {
    // We try to highlight typedefs as their underlying type.
    if (auto K =
            kindForType(TD->getUnderlyingType().getTypePtrOrNull(), Resolver))
      return K;
    // And fallback to a generic kind if this fails.
    return HighlightingKind::Typedef;
  }
  // We highlight class decls, constructor decls and destructor decls as
  // `Class` type. The destructor decls are handled in `VisitTagTypeLoc` (we
  // will visit a TypeLoc where the underlying Type is a CXXRecordDecl).
  if (auto *RD = llvm::dyn_cast<RecordDecl>(D)) {
    // We don't want to highlight lambdas like classes.
    if (RD->isLambda())
      return std::nullopt;
    return HighlightingKind::Class;
  }
  if (isa<ClassTemplateDecl, RecordDecl, CXXConstructorDecl, ObjCInterfaceDecl,
          ObjCImplementationDecl>(D))
    return HighlightingKind::Class;
  if (isa<ObjCProtocolDecl>(D))
    return HighlightingKind::Interface;
  if (isa<ObjCCategoryDecl, ObjCCategoryImplDecl>(D))
    return HighlightingKind::Namespace;
  if (auto *MD = dyn_cast<CXXMethodDecl>(D))
    return MD->isStatic() ? HighlightingKind::StaticMethod
                          : HighlightingKind::Method;
  if (auto *OMD = dyn_cast<ObjCMethodDecl>(D))
    return OMD->isClassMethod() ? HighlightingKind::StaticMethod
                                : HighlightingKind::Method;
  if (isa<FieldDecl, IndirectFieldDecl, ObjCPropertyDecl>(D))
    return HighlightingKind::Field;
  if (isa<EnumDecl>(D))
    return HighlightingKind::Enum;
  if (isa<EnumConstantDecl>(D))
    return HighlightingKind::EnumConstant;
  if (isa<ParmVarDecl>(D))
    return HighlightingKind::Parameter;
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    if (isa<ImplicitParamDecl>(VD)) // e.g. ObjC Self
      return std::nullopt;
    return VD->isStaticDataMember()
               ? HighlightingKind::StaticField
               : VD->isLocalVarDecl() ? HighlightingKind::LocalVariable
                                      : HighlightingKind::Variable;
  }
  if (const auto *BD = dyn_cast<BindingDecl>(D))
    return BD->getDeclContext()->isFunctionOrMethod()
               ? HighlightingKind::LocalVariable
               : HighlightingKind::Variable;
  if (isa<FunctionDecl>(D))
    return HighlightingKind::Function;
  if (isa<NamespaceDecl>(D) || isa<NamespaceAliasDecl>(D) ||
      isa<UsingDirectiveDecl>(D))
    return HighlightingKind::Namespace;
  if (isa<TemplateTemplateParmDecl>(D) || isa<TemplateTypeParmDecl>(D) ||
      isa<NonTypeTemplateParmDecl>(D))
    return HighlightingKind::TemplateParameter;
  if (isa<ConceptDecl>(D))
    return HighlightingKind::Concept;
  if (isa<LabelDecl>(D))
    return HighlightingKind::Label;
  if (const auto *UUVD = dyn_cast<UnresolvedUsingValueDecl>(D)) {
    auto Targets = Resolver->resolveUsingValueDecl(UUVD);
    if (!Targets.empty() && Targets[0] != UUVD) {
      return kindForDecl(Targets[0], Resolver);
    }
    return HighlightingKind::Unknown;
  }
  return std::nullopt;
}
std::optional<HighlightingKind> kindForType(const Type *TP,
                                            const HeuristicResolver *Resolver) {
  if (!TP)
    return std::nullopt;
  if (TP->isBuiltinType()) // Builtins are special, they do not have decls.
    return HighlightingKind::Primitive;
  if (auto *TD = dyn_cast<TemplateTypeParmType>(TP))
    return kindForDecl(TD->getDecl(), Resolver);
  if (isa<ObjCObjectPointerType>(TP))
    return HighlightingKind::Class;
  if (auto *TD = TP->getAsTagDecl())
    return kindForDecl(TD, Resolver);
  return std::nullopt;
}

// Whether T is const in a loose sense - is a variable with this type readonly?
bool isConst(QualType T) {
  if (T.isNull())
    return false;
  T = T.getNonReferenceType();
  if (T.isConstQualified())
    return true;
  if (const auto *AT = T->getAsArrayTypeUnsafe())
    return isConst(AT->getElementType());
  if (isConst(T->getPointeeType()))
    return true;
  return false;
}

// Whether D is const in a loose sense (should it be highlighted as such?)
// FIXME: This is separate from whether *a particular usage* can mutate D.
//        We may want V in V.size() to be readonly even if V is mutable.
bool isConst(const Decl *D) {
  if (llvm::isa<EnumConstantDecl>(D) || llvm::isa<NonTypeTemplateParmDecl>(D))
    return true;
  if (llvm::isa<FieldDecl>(D) || llvm::isa<VarDecl>(D) ||
      llvm::isa<MSPropertyDecl>(D) || llvm::isa<BindingDecl>(D)) {
    if (isConst(llvm::cast<ValueDecl>(D)->getType()))
      return true;
  }
  if (const auto *OCPD = llvm::dyn_cast<ObjCPropertyDecl>(D)) {
    if (OCPD->isReadOnly())
      return true;
  }
  if (const auto *MPD = llvm::dyn_cast<MSPropertyDecl>(D)) {
    if (!MPD->hasSetter())
      return true;
  }
  if (const auto *CMD = llvm::dyn_cast<CXXMethodDecl>(D)) {
    if (CMD->isConst())
      return true;
  }
  return false;
}

// "Static" means many things in C++, only some get the "static" modifier.
//
// Meanings that do:
// - Members associated with the class rather than the instance.
//   This is what 'static' most often means across languages.
// - static local variables
//   These are similarly "detached from their context" by the static keyword.
//   In practice, these are rarely used inside classes, reducing confusion.
//
// Meanings that don't:
// - Namespace-scoped variables, which have static storage class.
//   This is implicit, so the keyword "static" isn't so strongly associated.
//   If we want a modifier for these, "global scope" is probably the concept.
// - Namespace-scoped variables/functions explicitly marked "static".
//   There the keyword changes *linkage* , which is a totally different concept.
//   If we want to model this, "file scope" would be a nice modifier.
//
// This is confusing, and maybe we should use another name, but because "static"
// is a standard LSP modifier, having one with that name has advantages.
bool isStatic(const Decl *D) {
  if (const auto *CMD = llvm::dyn_cast<CXXMethodDecl>(D))
    return CMD->isStatic();
  if (const VarDecl *VD = llvm::dyn_cast<VarDecl>(D))
    return VD->isStaticDataMember() || VD->isStaticLocal();
  if (const auto *OPD = llvm::dyn_cast<ObjCPropertyDecl>(D))
    return OPD->isClassProperty();
  if (const auto *OMD = llvm::dyn_cast<ObjCMethodDecl>(D))
    return OMD->isClassMethod();
  return false;
}

bool isAbstract(const Decl *D) {
  if (const auto *CMD = llvm::dyn_cast<CXXMethodDecl>(D))
    return CMD->isPureVirtual();
  if (const auto *CRD = llvm::dyn_cast<CXXRecordDecl>(D))
    return CRD->hasDefinition() && CRD->isAbstract();
  return false;
}

bool isVirtual(const Decl *D) {
  if (const auto *CMD = llvm::dyn_cast<CXXMethodDecl>(D))
    return CMD->isVirtual();
  return false;
}

bool isDependent(const Decl *D) {
  if (isa<UnresolvedUsingValueDecl>(D))
    return true;
  return false;
}

/// Returns true if `Decl` is considered to be from a default/system library.
/// This currently checks the systemness of the file by include type, although
/// different heuristics may be used in the future (e.g. sysroot paths).
bool isDefaultLibrary(const Decl *D) {
  SourceLocation Loc = D->getLocation();
  if (!Loc.isValid())
    return false;
  return D->getASTContext().getSourceManager().isInSystemHeader(Loc);
}

bool isDefaultLibrary(const Type *T) {
  if (!T)
    return false;
  const Type *Underlying = T->getPointeeOrArrayElementType();
  if (Underlying->isBuiltinType())
    return true;
  if (auto *TD = dyn_cast<TemplateTypeParmType>(Underlying))
    return isDefaultLibrary(TD->getDecl());
  if (auto *TD = Underlying->getAsTagDecl())
    return isDefaultLibrary(TD);
  return false;
}

// For a macro usage `DUMP(foo)`, we want:
//  - DUMP --> "macro"
//  - foo --> "variable".
SourceLocation getHighlightableSpellingToken(SourceLocation L,
                                             const SourceManager &SM) {
  if (L.isFileID())
    return SM.isWrittenInMainFile(L) ? L : SourceLocation{};
  // Tokens expanded from the macro body contribute no highlightings.
  if (!SM.isMacroArgExpansion(L))
    return {};
  // Tokens expanded from macro args are potentially highlightable.
  return getHighlightableSpellingToken(SM.getImmediateSpellingLoc(L), SM);
}

unsigned evaluateHighlightPriority(const HighlightingToken &Tok) {
  enum HighlightPriority { Dependent = 0, Resolved = 1 };
  return (Tok.Modifiers & (1 << uint32_t(HighlightingModifier::DependentName)))
             ? Dependent
             : Resolved;
}

// Sometimes we get multiple tokens at the same location:
//
// - findExplicitReferences() returns a heuristic result for a dependent name
//   (e.g. Method) and CollectExtraHighlighting returning a fallback dependent
//   highlighting (e.g. Unknown+Dependent).
// - macro arguments are expanded multiple times and have different roles
// - broken code recovery produces several AST nodes at the same location
//
// We should either resolve these to a single token, or drop them all.
// Our heuristics are:
//
// - token kinds that come with "dependent-name" modifiers are less reliable
//   (these tend to be vague, like Type or Unknown)
// - if we have multiple equally reliable kinds, drop token rather than guess
// - take the union of modifiers from all tokens
//
// In particular, heuristically resolved dependent names get their heuristic
// kind, plus the dependent modifier.
std::optional<HighlightingToken> resolveConflict(const HighlightingToken &A,
                                                 const HighlightingToken &B) {
  unsigned Priority1 = evaluateHighlightPriority(A);
  unsigned Priority2 = evaluateHighlightPriority(B);
  if (Priority1 == Priority2 && A.Kind != B.Kind)
    return std::nullopt;
  auto Result = Priority1 > Priority2 ? A : B;
  Result.Modifiers = A.Modifiers | B.Modifiers;
  return Result;
}
std::optional<HighlightingToken>
resolveConflict(ArrayRef<HighlightingToken> Tokens) {
  if (Tokens.size() == 1)
    return Tokens[0];

  assert(Tokens.size() >= 2);
  std::optional<HighlightingToken> Winner =
      resolveConflict(Tokens[0], Tokens[1]);
  for (size_t I = 2; Winner && I < Tokens.size(); ++I)
    Winner = resolveConflict(*Winner, Tokens[I]);
  return Winner;
}

/// Filter to remove particular kinds of highlighting tokens and modifiers from
/// the output.
class HighlightingFilter {
public:
  HighlightingFilter() {
    for (auto &Active : ActiveKindLookup)
      Active = true;

    ActiveModifiersMask = ~0;
  }

  void disableKind(HighlightingKind Kind) {
    ActiveKindLookup[static_cast<size_t>(Kind)] = false;
  }

  void disableModifier(HighlightingModifier Modifier) {
    ActiveModifiersMask &= ~(1 << static_cast<uint32_t>(Modifier));
  }

  bool isHighlightKindActive(HighlightingKind Kind) const {
    return ActiveKindLookup[static_cast<size_t>(Kind)];
  }

  uint32_t maskModifiers(uint32_t Modifiers) const {
    return Modifiers & ActiveModifiersMask;
  }

  static HighlightingFilter fromCurrentConfig() {
    const Config &C = Config::current();
    HighlightingFilter Filter;
    for (const auto &Kind : C.SemanticTokens.DisabledKinds)
      if (auto K = highlightingKindFromString(Kind))
        Filter.disableKind(*K);
    for (const auto &Modifier : C.SemanticTokens.DisabledModifiers)
      if (auto M = highlightingModifierFromString(Modifier))
        Filter.disableModifier(*M);

    return Filter;
  }

private:
  bool ActiveKindLookup[static_cast<size_t>(HighlightingKind::LastKind) + 1];
  uint32_t ActiveModifiersMask;
};

/// Consumes source locations and maps them to text ranges for highlightings.
class HighlightingsBuilder {
public:
  HighlightingsBuilder(const ParsedAST &AST, const HighlightingFilter &Filter)
      : TB(AST.getTokens()), SourceMgr(AST.getSourceManager()),
        LangOpts(AST.getLangOpts()), Filter(Filter),
        Resolver(AST.getHeuristicResolver()) {}

  HighlightingToken &addToken(SourceLocation Loc, HighlightingKind Kind) {
    auto Range = getRangeForSourceLocation(Loc);
    if (!Range)
      return InvalidHighlightingToken;

    return addToken(*Range, Kind);
  }

  // Most of this function works around
  // https://github.com/clangd/clangd/issues/871.
  void addAngleBracketTokens(SourceLocation LLoc, SourceLocation RLoc) {
    if (!LLoc.isValid() || !RLoc.isValid())
      return;

    auto LRange = getRangeForSourceLocation(LLoc);
    if (!LRange)
      return;

    // RLoc might be pointing at a virtual buffer when it's part of a `>>`
    // token.
    RLoc = SourceMgr.getFileLoc(RLoc);
    // Make sure token is part of the main file.
    RLoc = getHighlightableSpellingToken(RLoc, SourceMgr);
    if (!RLoc.isValid())
      return;

    const auto *RTok = TB.spelledTokenContaining(RLoc);
    // Handle `>>`. RLoc is either part of `>>` or a spelled token on its own
    // `>`. If it's the former, slice to have length of 1, if latter use the
    // token as-is.
    if (!RTok || RTok->kind() == tok::greatergreater) {
      Position Begin = sourceLocToPosition(SourceMgr, RLoc);
      Position End = sourceLocToPosition(SourceMgr, RLoc.getLocWithOffset(1));
      addToken(*LRange, HighlightingKind::Bracket);
      addToken({Begin, End}, HighlightingKind::Bracket);
      return;
    }

    // Easy case, we have the `>` token directly available.
    if (RTok->kind() == tok::greater) {
      if (auto RRange = getRangeForSourceLocation(RLoc)) {
        addToken(*LRange, HighlightingKind::Bracket);
        addToken(*RRange, HighlightingKind::Bracket);
      }
      return;
    }
  }

  HighlightingToken &addToken(Range R, HighlightingKind Kind) {
    if (!Filter.isHighlightKindActive(Kind))
      return InvalidHighlightingToken;

    HighlightingToken HT;
    HT.R = std::move(R);
    HT.Kind = Kind;
    Tokens.push_back(std::move(HT));
    return Tokens.back();
  }

  void addExtraModifier(SourceLocation Loc, HighlightingModifier Modifier) {
    if (auto Range = getRangeForSourceLocation(Loc))
      ExtraModifiers[*Range].push_back(Modifier);
  }

  std::vector<HighlightingToken> collect(ParsedAST &AST) && {
    // Initializer lists can give duplicates of tokens, therefore all tokens
    // must be deduplicated.
    llvm::sort(Tokens);
    auto Last = llvm::unique(Tokens);
    Tokens.erase(Last, Tokens.end());

    // Macros can give tokens that have the same source range but conflicting
    // kinds. In this case all tokens sharing this source range should be
    // removed.
    std::vector<HighlightingToken> NonConflicting;
    NonConflicting.reserve(Tokens.size());
    for (ArrayRef<HighlightingToken> TokRef = Tokens; !TokRef.empty();) {
      ArrayRef<HighlightingToken> Conflicting =
          TokRef.take_while([&](const HighlightingToken &T) {
            // TokRef is guaranteed at least one element here because otherwise
            // this predicate would never fire.
            return T.R == TokRef.front().R;
          });
      if (auto Resolved = resolveConflict(Conflicting)) {
        // Apply extra collected highlighting modifiers
        auto Modifiers = ExtraModifiers.find(Resolved->R);
        if (Modifiers != ExtraModifiers.end()) {
          for (HighlightingModifier Mod : Modifiers->second) {
            Resolved->addModifier(Mod);
          }
        }

        Resolved->Modifiers = Filter.maskModifiers(Resolved->Modifiers);
        NonConflicting.push_back(*Resolved);
      }
      // TokRef[Conflicting.size()] is the next token with a different range (or
      // the end of the Tokens).
      TokRef = TokRef.drop_front(Conflicting.size());
    }

    if (!Filter.isHighlightKindActive(HighlightingKind::InactiveCode))
      return NonConflicting;

    const auto &SM = AST.getSourceManager();
    StringRef MainCode = SM.getBufferOrFake(SM.getMainFileID()).getBuffer();

    // Merge token stream with "inactive line" markers.
    std::vector<HighlightingToken> WithInactiveLines;
    auto SortedInactiveRegions = getInactiveRegions(AST);
    llvm::sort(SortedInactiveRegions);
    auto It = NonConflicting.begin();
    for (const Range &R : SortedInactiveRegions) {
      // Create one token for each line in the inactive range, so it works
      // with line-based diffing.
      assert(R.start.line <= R.end.line);
      for (int Line = R.start.line; Line <= R.end.line; ++Line) {
        // Copy tokens before the inactive line
        for (; It != NonConflicting.end() && It->R.start.line < Line; ++It)
          WithInactiveLines.push_back(std::move(*It));
        // Add a token for the inactive line itself.
        auto EndOfLine = endOfLine(MainCode, Line);
        if (EndOfLine) {
          HighlightingToken HT;
          WithInactiveLines.emplace_back();
          WithInactiveLines.back().Kind = HighlightingKind::InactiveCode;
          WithInactiveLines.back().R.start.line = Line;
          WithInactiveLines.back().R.end = *EndOfLine;
        } else {
          elog("Failed to determine end of line: {0}", EndOfLine.takeError());
        }

        // Skip any other tokens on the inactive line. e.g.
        // `#ifndef Foo` is considered as part of an inactive region when Foo is
        // defined, and there is a Foo macro token.
        // FIXME: we should reduce the scope of the inactive region to not
        // include the directive itself.
        while (It != NonConflicting.end() && It->R.start.line == Line)
          ++It;
      }
    }
    // Copy tokens after the last inactive line
    for (; It != NonConflicting.end(); ++It)
      WithInactiveLines.push_back(std::move(*It));
    return WithInactiveLines;
  }

  const HeuristicResolver *getResolver() const { return Resolver; }

private:
  std::optional<Range> getRangeForSourceLocation(SourceLocation Loc) {
    Loc = getHighlightableSpellingToken(Loc, SourceMgr);
    if (Loc.isInvalid())
      return std::nullopt;
    // We might have offsets in the main file that don't correspond to any
    // spelled tokens.
    const auto *Tok = TB.spelledTokenContaining(Loc);
    if (!Tok)
      return std::nullopt;
    return halfOpenToRange(SourceMgr,
                           Tok->range(SourceMgr).toCharRange(SourceMgr));
  }

  const syntax::TokenBuffer &TB;
  const SourceManager &SourceMgr;
  const LangOptions &LangOpts;
  HighlightingFilter Filter;
  std::vector<HighlightingToken> Tokens;
  std::map<Range, llvm::SmallVector<HighlightingModifier, 1>> ExtraModifiers;
  const HeuristicResolver *Resolver;
  // returned from addToken(InvalidLoc)
  HighlightingToken InvalidHighlightingToken;
};

std::optional<HighlightingModifier> scopeModifier(const NamedDecl *D) {
  const DeclContext *DC = D->getDeclContext();
  // Injected "Foo" within the class "Foo" has file scope, not class scope.
  if (auto *R = dyn_cast_or_null<CXXRecordDecl>(D))
    if (R->isInjectedClassName())
      DC = DC->getParent();
  // Lambda captures are considered function scope, not class scope.
  if (llvm::isa<FieldDecl>(D))
    if (const auto *RD = llvm::dyn_cast<RecordDecl>(DC))
      if (RD->isLambda())
        return HighlightingModifier::FunctionScope;
  // Walk up the DeclContext hierarchy until we find something interesting.
  for (; !DC->isFileContext(); DC = DC->getParent()) {
    if (DC->isFunctionOrMethod())
      return HighlightingModifier::FunctionScope;
    if (DC->isRecord())
      return HighlightingModifier::ClassScope;
  }
  // Some template parameters (e.g. those for variable templates) don't have
  // meaningful DeclContexts. That doesn't mean they're global!
  if (DC->isTranslationUnit() && D->isTemplateParameter())
    return std::nullopt;
  // ExternalLinkage threshold could be tweaked, e.g. module-visible as global.
  if (llvm::to_underlying(D->getLinkageInternal()) <
      llvm::to_underlying(Linkage::External))
    return HighlightingModifier::FileScope;
  return HighlightingModifier::GlobalScope;
}

std::optional<HighlightingModifier> scopeModifier(const Type *T) {
  if (!T)
    return std::nullopt;
  if (T->isBuiltinType())
    return HighlightingModifier::GlobalScope;
  if (auto *TD = dyn_cast<TemplateTypeParmType>(T))
    return scopeModifier(TD->getDecl());
  if (auto *TD = T->getAsTagDecl())
    return scopeModifier(TD);
  return std::nullopt;
}

/// Produces highlightings, which are not captured by findExplicitReferences,
/// e.g. highlights dependent names and 'auto' as the underlying type.
class CollectExtraHighlightings
    : public RecursiveASTVisitor<CollectExtraHighlightings> {
  using Base = RecursiveASTVisitor<CollectExtraHighlightings>;

public:
  CollectExtraHighlightings(HighlightingsBuilder &H) : H(H) {}

  bool VisitCXXConstructExpr(CXXConstructExpr *E) {
    highlightMutableReferenceArguments(E->getConstructor(),
                                       {E->getArgs(), E->getNumArgs()});

    return true;
  }

  bool TraverseConstructorInitializer(CXXCtorInitializer *Init) {
    if (Init->isMemberInitializer())
      if (auto *Member = Init->getMember())
        highlightMutableReferenceArgument(Member->getType(), Init->getInit());
    return Base::TraverseConstructorInitializer(Init);
  }

  bool TraverseTypeConstraint(const TypeConstraint *C) {
    if (auto *Args = C->getTemplateArgsAsWritten())
      H.addAngleBracketTokens(Args->getLAngleLoc(), Args->getRAngleLoc());
    return Base::TraverseTypeConstraint(C);
  }

  bool VisitPredefinedExpr(PredefinedExpr *E) {
    H.addToken(E->getLocation(), HighlightingKind::LocalVariable)
        .addModifier(HighlightingModifier::Static)
        .addModifier(HighlightingModifier::Readonly)
        .addModifier(HighlightingModifier::FunctionScope);
    return true;
  }

  bool VisitConceptSpecializationExpr(ConceptSpecializationExpr *E) {
    if (auto *Args = E->getTemplateArgsAsWritten())
      H.addAngleBracketTokens(Args->getLAngleLoc(), Args->getRAngleLoc());
    return true;
  }

  bool VisitTemplateDecl(TemplateDecl *D) {
    if (auto *TPL = D->getTemplateParameters())
      H.addAngleBracketTokens(TPL->getLAngleLoc(), TPL->getRAngleLoc());
    return true;
  }

  bool VisitTagDecl(TagDecl *D) {
    for (unsigned i = 0; i < D->getNumTemplateParameterLists(); ++i) {
      if (auto *TPL = D->getTemplateParameterList(i))
        H.addAngleBracketTokens(TPL->getLAngleLoc(), TPL->getRAngleLoc());
    }
    return true;
  }

  bool
  VisitClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl *D) {
    if (auto *Args = D->getTemplateArgsAsWritten())
      H.addAngleBracketTokens(Args->getLAngleLoc(), Args->getRAngleLoc());
    return true;
  }

  bool VisitClassTemplatePartialSpecializationDecl(
      ClassTemplatePartialSpecializationDecl *D) {
    if (auto *TPL = D->getTemplateParameters())
      H.addAngleBracketTokens(TPL->getLAngleLoc(), TPL->getRAngleLoc());
    return true;
  }

  bool VisitVarTemplateSpecializationDecl(VarTemplateSpecializationDecl *D) {
    if (auto *Args = D->getTemplateArgsAsWritten())
      H.addAngleBracketTokens(Args->getLAngleLoc(), Args->getRAngleLoc());
    return true;
  }

  bool VisitVarTemplatePartialSpecializationDecl(
      VarTemplatePartialSpecializationDecl *D) {
    if (auto *TPL = D->getTemplateParameters())
      H.addAngleBracketTokens(TPL->getLAngleLoc(), TPL->getRAngleLoc());
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    H.addAngleBracketTokens(E->getLAngleLoc(), E->getRAngleLoc());
    return true;
  }
  bool VisitMemberExpr(MemberExpr *E) {
    H.addAngleBracketTokens(E->getLAngleLoc(), E->getRAngleLoc());
    return true;
  }

  bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc L) {
    H.addAngleBracketTokens(L.getLAngleLoc(), L.getRAngleLoc());
    return true;
  }

  bool VisitFunctionDecl(FunctionDecl *D) {
    if (D->isOverloadedOperator()) {
      const auto AddOpDeclToken = [&](SourceLocation Loc) {
        auto &Token = H.addToken(Loc, HighlightingKind::Operator)
                          .addModifier(HighlightingModifier::Declaration);
        if (D->isThisDeclarationADefinition())
          Token.addModifier(HighlightingModifier::Definition);
      };
      const auto Range = D->getNameInfo().getCXXOperatorNameRange();
      AddOpDeclToken(Range.getBegin());
      const auto Kind = D->getOverloadedOperator();
      if (Kind == OO_Call || Kind == OO_Subscript)
        AddOpDeclToken(Range.getEnd());
    }
    if (auto *Args = D->getTemplateSpecializationArgsAsWritten())
      H.addAngleBracketTokens(Args->getLAngleLoc(), Args->getRAngleLoc());
    return true;
  }

  bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *E) {
    const auto AddOpToken = [&](SourceLocation Loc) {
      H.addToken(Loc, HighlightingKind::Operator)
          .addModifier(HighlightingModifier::UserDefined);
    };
    AddOpToken(E->getOperatorLoc());
    const auto Kind = E->getOperator();
    if (Kind == OO_Call || Kind == OO_Subscript) {
      if (auto *Callee = E->getCallee())
        AddOpToken(Callee->getBeginLoc());
    }
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *Op) {
    auto &Token = H.addToken(Op->getOperatorLoc(), HighlightingKind::Operator);
    if (Op->getSubExpr()->isTypeDependent())
      Token.addModifier(HighlightingModifier::UserDefined);
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *Op) {
    auto &Token = H.addToken(Op->getOperatorLoc(), HighlightingKind::Operator);
    if (Op->getLHS()->isTypeDependent() || Op->getRHS()->isTypeDependent())
      Token.addModifier(HighlightingModifier::UserDefined);
    return true;
  }

  bool VisitConditionalOperator(ConditionalOperator *Op) {
    H.addToken(Op->getQuestionLoc(), HighlightingKind::Operator);
    H.addToken(Op->getColonLoc(), HighlightingKind::Operator);
    return true;
  }

  bool VisitCXXNewExpr(CXXNewExpr *E) {
    auto &Token = H.addToken(E->getBeginLoc(), HighlightingKind::Operator);
    if (isa_and_present<CXXMethodDecl>(E->getOperatorNew()))
      Token.addModifier(HighlightingModifier::UserDefined);
    return true;
  }

  bool VisitCXXDeleteExpr(CXXDeleteExpr *E) {
    auto &Token = H.addToken(E->getBeginLoc(), HighlightingKind::Operator);
    if (isa_and_present<CXXMethodDecl>(E->getOperatorDelete()))
      Token.addModifier(HighlightingModifier::UserDefined);
    return true;
  }

  bool VisitCXXNamedCastExpr(CXXNamedCastExpr *E) {
    const auto &B = E->getAngleBrackets();
    H.addAngleBracketTokens(B.getBegin(), B.getEnd());
    return true;
  }

  bool VisitCallExpr(CallExpr *E) {
    // Highlighting parameters passed by non-const reference does not really
    // make sense for literals...
    if (isa<UserDefinedLiteral>(E))
      return true;

    // FIXME: consider highlighting parameters of some other overloaded
    // operators as well
    llvm::ArrayRef<const Expr *> Args = {E->getArgs(), E->getNumArgs()};
    if (auto *CallOp = dyn_cast<CXXOperatorCallExpr>(E)) {
      switch (CallOp->getOperator()) {
      case OO_Call:
      case OO_Subscript:
        Args = Args.drop_front(); // Drop object parameter
        break;
      default:
        return true;
      }
    }

    highlightMutableReferenceArguments(
        dyn_cast_or_null<FunctionDecl>(E->getCalleeDecl()), Args);

    return true;
  }

  void highlightMutableReferenceArgument(QualType T, const Expr *Arg) {
    if (!Arg)
      return;

    // Is this parameter passed by non-const pointer or reference?
    // FIXME The condition T->idDependentType() could be relaxed a bit,
    // e.g. std::vector<T>& is dependent but we would want to highlight it
    bool IsRef = T->isLValueReferenceType();
    bool IsPtr = T->isPointerType();
    if ((!IsRef && !IsPtr) || T->getPointeeType().isConstQualified() ||
        T->isDependentType()) {
      return;
    }

    std::optional<SourceLocation> Location;

    // FIXME Add "unwrapping" for ArraySubscriptExpr,
    //  e.g. highlight `a` in `a[i]`
    // FIXME Handle dependent expression types
    if (auto *IC = dyn_cast<ImplicitCastExpr>(Arg))
      Arg = IC->getSubExprAsWritten();
    if (auto *UO = dyn_cast<UnaryOperator>(Arg)) {
      if (UO->getOpcode() == UO_AddrOf)
        Arg = UO->getSubExpr();
    }
    if (auto *DR = dyn_cast<DeclRefExpr>(Arg))
      Location = DR->getLocation();
    else if (auto *M = dyn_cast<MemberExpr>(Arg))
      Location = M->getMemberLoc();

    if (Location)
      H.addExtraModifier(*Location,
                         IsRef ? HighlightingModifier::UsedAsMutableReference
                               : HighlightingModifier::UsedAsMutablePointer);
  }

  void
  highlightMutableReferenceArguments(const FunctionDecl *FD,
                                     llvm::ArrayRef<const Expr *const> Args) {
    if (!FD)
      return;

    if (auto *ProtoType = FD->getType()->getAs<FunctionProtoType>()) {
      // Iterate over the types of the function parameters.
      // If any of them are non-const reference paramteres, add it as a
      // highlighting modifier to the corresponding expression
      for (size_t I = 0;
           I < std::min(size_t(ProtoType->getNumParams()), Args.size()); ++I) {
        highlightMutableReferenceArgument(ProtoType->getParamType(I), Args[I]);
      }
    }
  }

  bool VisitDecltypeTypeLoc(DecltypeTypeLoc L) {
    if (auto K = kindForType(L.getTypePtr(), H.getResolver())) {
      auto &Tok = H.addToken(L.getBeginLoc(), *K)
                      .addModifier(HighlightingModifier::Deduced);
      if (auto Mod = scopeModifier(L.getTypePtr()))
        Tok.addModifier(*Mod);
      if (isDefaultLibrary(L.getTypePtr()))
        Tok.addModifier(HighlightingModifier::DefaultLibrary);
    }
    return true;
  }

  bool VisitCXXDestructorDecl(CXXDestructorDecl *D) {
    if (auto *TI = D->getNameInfo().getNamedTypeInfo()) {
      SourceLocation Loc = TI->getTypeLoc().getBeginLoc();
      H.addExtraModifier(Loc, HighlightingModifier::ConstructorOrDestructor);
      H.addExtraModifier(Loc, HighlightingModifier::Declaration);
      if (D->isThisDeclarationADefinition())
        H.addExtraModifier(Loc, HighlightingModifier::Definition);
    }
    return true;
  }

  bool VisitCXXMemberCallExpr(CXXMemberCallExpr *CE) {
    // getMethodDecl can return nullptr with member pointers, e.g.
    // `(foo.*pointer_to_member_fun)(arg);`
    if (auto *D = CE->getMethodDecl()) {
      if (isa<CXXDestructorDecl>(D)) {
        if (auto *ME = dyn_cast<MemberExpr>(CE->getCallee())) {
          if (auto *TI = ME->getMemberNameInfo().getNamedTypeInfo()) {
            H.addExtraModifier(TI->getTypeLoc().getBeginLoc(),
                               HighlightingModifier::ConstructorOrDestructor);
          }
        }
      } else if (D->isOverloadedOperator()) {
        if (auto *ME = dyn_cast<MemberExpr>(CE->getCallee()))
          H.addToken(
               ME->getMemberNameInfo().getCXXOperatorNameRange().getBegin(),
               HighlightingKind::Operator)
              .addModifier(HighlightingModifier::UserDefined);
      }
    }
    return true;
  }

  bool VisitDeclaratorDecl(DeclaratorDecl *D) {
    for (unsigned i = 0; i < D->getNumTemplateParameterLists(); ++i) {
      if (auto *TPL = D->getTemplateParameterList(i))
        H.addAngleBracketTokens(TPL->getLAngleLoc(), TPL->getRAngleLoc());
    }
    auto *AT = D->getType()->getContainedAutoType();
    if (!AT)
      return true;
    auto K =
        kindForType(AT->getDeducedType().getTypePtrOrNull(), H.getResolver());
    if (!K)
      return true;
    auto *TSI = D->getTypeSourceInfo();
    if (!TSI)
      return true;
    SourceLocation StartLoc =
        TSI->getTypeLoc().getContainedAutoTypeLoc().getNameLoc();
    // The AutoType may not have a corresponding token, e.g. in the case of
    // init-captures. In this case, StartLoc overlaps with the location
    // of the decl itself, and producing a token for the type here would result
    // in both it and the token for the decl being dropped due to conflict.
    if (StartLoc == D->getLocation())
      return true;

    auto &Tok =
        H.addToken(StartLoc, *K).addModifier(HighlightingModifier::Deduced);
    const Type *Deduced = AT->getDeducedType().getTypePtrOrNull();
    if (auto Mod = scopeModifier(Deduced))
      Tok.addModifier(*Mod);
    if (isDefaultLibrary(Deduced))
      Tok.addModifier(HighlightingModifier::DefaultLibrary);
    return true;
  }

  // We handle objective-C selectors specially, because one reference can
  // cover several non-contiguous tokens.
  void highlightObjCSelector(const ArrayRef<SourceLocation> &Locs, bool Decl,
                             bool Def, bool Class, bool DefaultLibrary) {
    HighlightingKind Kind =
        Class ? HighlightingKind::StaticMethod : HighlightingKind::Method;
    for (SourceLocation Part : Locs) {
      auto &Tok =
          H.addToken(Part, Kind).addModifier(HighlightingModifier::ClassScope);
      if (Decl)
        Tok.addModifier(HighlightingModifier::Declaration);
      if (Def)
        Tok.addModifier(HighlightingModifier::Definition);
      if (Class)
        Tok.addModifier(HighlightingModifier::Static);
      if (DefaultLibrary)
        Tok.addModifier(HighlightingModifier::DefaultLibrary);
    }
  }

  bool VisitObjCMethodDecl(ObjCMethodDecl *OMD) {
    llvm::SmallVector<SourceLocation> Locs;
    OMD->getSelectorLocs(Locs);
    highlightObjCSelector(Locs, /*Decl=*/true,
                          OMD->isThisDeclarationADefinition(),
                          OMD->isClassMethod(), isDefaultLibrary(OMD));
    return true;
  }

  bool VisitObjCMessageExpr(ObjCMessageExpr *OME) {
    llvm::SmallVector<SourceLocation> Locs;
    OME->getSelectorLocs(Locs);
    bool DefaultLibrary = false;
    if (ObjCMethodDecl *OMD = OME->getMethodDecl())
      DefaultLibrary = isDefaultLibrary(OMD);
    highlightObjCSelector(Locs, /*Decl=*/false, /*Def=*/false,
                          OME->isClassMessage(), DefaultLibrary);
    return true;
  }

  // Objective-C allows you to use property syntax `self.prop` as sugar for
  // `[self prop]` and `[self setProp:]` when there's no explicit `@property`
  // for `prop` as well as for class properties. We treat this like a property
  // even though semantically it's equivalent to a method expression.
  void highlightObjCImplicitPropertyRef(const ObjCMethodDecl *OMD,
                                        SourceLocation Loc) {
    auto &Tok = H.addToken(Loc, HighlightingKind::Field)
                    .addModifier(HighlightingModifier::ClassScope);
    if (OMD->isClassMethod())
      Tok.addModifier(HighlightingModifier::Static);
    if (isDefaultLibrary(OMD))
      Tok.addModifier(HighlightingModifier::DefaultLibrary);
  }

  bool VisitObjCPropertyRefExpr(ObjCPropertyRefExpr *OPRE) {
    // We need to handle implicit properties here since they will appear to
    // reference `ObjCMethodDecl` via an implicit `ObjCMessageExpr`, so normal
    // highlighting will not work.
    if (!OPRE->isImplicitProperty())
      return true;
    // A single property expr can reference both a getter and setter, but we can
    // only provide a single semantic token, so prefer the getter. In most cases
    // the end result should be the same, although it's technically possible
    // that the user defines a setter for a system SDK.
    if (OPRE->isMessagingGetter()) {
      highlightObjCImplicitPropertyRef(OPRE->getImplicitPropertyGetter(),
                                       OPRE->getLocation());
      return true;
    }
    if (OPRE->isMessagingSetter()) {
      highlightObjCImplicitPropertyRef(OPRE->getImplicitPropertySetter(),
                                       OPRE->getLocation());
    }
    return true;
  }

  bool VisitOverloadExpr(OverloadExpr *E) {
    H.addAngleBracketTokens(E->getLAngleLoc(), E->getRAngleLoc());
    if (!E->decls().empty())
      return true; // handled by findExplicitReferences.
    auto &Tok = H.addToken(E->getNameLoc(), HighlightingKind::Unknown)
                    .addModifier(HighlightingModifier::DependentName);
    if (llvm::isa<UnresolvedMemberExpr>(E))
      Tok.addModifier(HighlightingModifier::ClassScope);
    // other case is UnresolvedLookupExpr, scope is unknown.
    return true;
  }

  bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr *E) {
    H.addToken(E->getMemberNameInfo().getLoc(), HighlightingKind::Unknown)
        .addModifier(HighlightingModifier::DependentName)
        .addModifier(HighlightingModifier::ClassScope);
    H.addAngleBracketTokens(E->getLAngleLoc(), E->getRAngleLoc());
    return true;
  }

  bool VisitDependentScopeDeclRefExpr(DependentScopeDeclRefExpr *E) {
    H.addToken(E->getNameInfo().getLoc(), HighlightingKind::Unknown)
        .addModifier(HighlightingModifier::DependentName)
        .addModifier(HighlightingModifier::ClassScope);
    H.addAngleBracketTokens(E->getLAngleLoc(), E->getRAngleLoc());
    return true;
  }

  bool VisitAttr(Attr *A) {
    switch (A->getKind()) {
    case attr::Override:
    case attr::Final:
      H.addToken(A->getLocation(), HighlightingKind::Modifier);
      break;
    default:
      break;
    }
    return true;
  }

  bool VisitDependentNameTypeLoc(DependentNameTypeLoc L) {
    H.addToken(L.getNameLoc(), HighlightingKind::Type)
        .addModifier(HighlightingModifier::DependentName)
        .addModifier(HighlightingModifier::ClassScope);
    return true;
  }

  bool VisitDependentTemplateSpecializationTypeLoc(
      DependentTemplateSpecializationTypeLoc L) {
    H.addToken(L.getTemplateNameLoc(), HighlightingKind::Type)
        .addModifier(HighlightingModifier::DependentName)
        .addModifier(HighlightingModifier::ClassScope);
    H.addAngleBracketTokens(L.getLAngleLoc(), L.getRAngleLoc());
    return true;
  }

  bool TraverseTemplateArgumentLoc(TemplateArgumentLoc L) {
    // Handle template template arguments only (other arguments are handled by
    // their Expr, TypeLoc etc values).
    if (L.getArgument().getKind() != TemplateArgument::Template &&
        L.getArgument().getKind() != TemplateArgument::TemplateExpansion)
      return RecursiveASTVisitor::TraverseTemplateArgumentLoc(L);

    TemplateName N = L.getArgument().getAsTemplateOrTemplatePattern();
    switch (N.getKind()) {
    case TemplateName::OverloadedTemplate:
      // Template template params must always be class templates.
      // Don't bother to try to work out the scope here.
      H.addToken(L.getTemplateNameLoc(), HighlightingKind::Class);
      break;
    case TemplateName::DependentTemplate:
    case TemplateName::AssumedTemplate:
      H.addToken(L.getTemplateNameLoc(), HighlightingKind::Class)
          .addModifier(HighlightingModifier::DependentName);
      break;
    case TemplateName::Template:
    case TemplateName::QualifiedTemplate:
    case TemplateName::SubstTemplateTemplateParm:
    case TemplateName::SubstTemplateTemplateParmPack:
    case TemplateName::UsingTemplate:
    case TemplateName::DeducedTemplate:
      // Names that could be resolved to a TemplateDecl are handled elsewhere.
      break;
    }
    return RecursiveASTVisitor::TraverseTemplateArgumentLoc(L);
  }

  // findExplicitReferences will walk nested-name-specifiers and
  // find anything that can be resolved to a Decl. However, non-leaf
  // components of nested-name-specifiers which are dependent names
  // (kind "Identifier") cannot be resolved to a decl, so we visit
  // them here.
  bool TraverseNestedNameSpecifierLoc(NestedNameSpecifierLoc Q) {
    if (NestedNameSpecifier *NNS = Q.getNestedNameSpecifier()) {
      if (NNS->getKind() == NestedNameSpecifier::Identifier)
        H.addToken(Q.getLocalBeginLoc(), HighlightingKind::Type)
            .addModifier(HighlightingModifier::DependentName)
            .addModifier(HighlightingModifier::ClassScope);
    }
    return RecursiveASTVisitor::TraverseNestedNameSpecifierLoc(Q);
  }

private:
  HighlightingsBuilder &H;
};
} // namespace

std::vector<HighlightingToken>
getSemanticHighlightings(ParsedAST &AST, bool IncludeInactiveRegionTokens) {
  auto &C = AST.getASTContext();
  HighlightingFilter Filter = HighlightingFilter::fromCurrentConfig();
  if (!IncludeInactiveRegionTokens)
    Filter.disableKind(HighlightingKind::InactiveCode);
  // Add highlightings for AST nodes.
  HighlightingsBuilder Builder(AST, Filter);
  // Highlight 'decltype' and 'auto' as their underlying types.
  CollectExtraHighlightings(Builder).TraverseAST(C);
  // Highlight all decls and references coming from the AST.
  findExplicitReferences(
      C,
      [&](ReferenceLoc R) {
        for (const NamedDecl *Decl : R.Targets) {
          if (!canHighlightName(Decl->getDeclName()))
            continue;
          auto Kind = kindForDecl(Decl, AST.getHeuristicResolver());
          if (!Kind)
            continue;
          auto &Tok = Builder.addToken(R.NameLoc, *Kind);

          // The attribute tests don't want to look at the template.
          if (auto *TD = dyn_cast<TemplateDecl>(Decl)) {
            if (auto *Templated = TD->getTemplatedDecl())
              Decl = Templated;
          }
          if (auto Mod = scopeModifier(Decl))
            Tok.addModifier(*Mod);
          if (isConst(Decl))
            Tok.addModifier(HighlightingModifier::Readonly);
          if (isStatic(Decl))
            Tok.addModifier(HighlightingModifier::Static);
          if (isAbstract(Decl))
            Tok.addModifier(HighlightingModifier::Abstract);
          if (isVirtual(Decl))
            Tok.addModifier(HighlightingModifier::Virtual);
          if (isDependent(Decl))
            Tok.addModifier(HighlightingModifier::DependentName);
          if (isDefaultLibrary(Decl))
            Tok.addModifier(HighlightingModifier::DefaultLibrary);
          if (Decl->isDeprecated())
            Tok.addModifier(HighlightingModifier::Deprecated);
          if (isa<CXXConstructorDecl>(Decl))
            Tok.addModifier(HighlightingModifier::ConstructorOrDestructor);
          if (R.IsDecl) {
            // Do not treat an UnresolvedUsingValueDecl as a declaration.
            // It's more common to think of it as a reference to the
            // underlying declaration.
            if (!isa<UnresolvedUsingValueDecl>(Decl))
              Tok.addModifier(HighlightingModifier::Declaration);
            if (isUniqueDefinition(Decl))
              Tok.addModifier(HighlightingModifier::Definition);
          }
        }
      },
      AST.getHeuristicResolver());
  // Add highlightings for macro references.
  auto AddMacro = [&](const MacroOccurrence &M) {
    auto &T = Builder.addToken(M.toRange(C.getSourceManager()),
                               HighlightingKind::Macro);
    T.addModifier(HighlightingModifier::GlobalScope);
    if (M.IsDefinition)
      T.addModifier(HighlightingModifier::Declaration);
  };
  for (const auto &SIDToRefs : AST.getMacros().MacroRefs)
    for (const auto &M : SIDToRefs.second)
      AddMacro(M);
  for (const auto &M : AST.getMacros().UnknownMacros)
    AddMacro(M);

  return std::move(Builder).collect(AST);
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, HighlightingKind K) {
  switch (K) {
  case HighlightingKind::Variable:
    return OS << "Variable";
  case HighlightingKind::LocalVariable:
    return OS << "LocalVariable";
  case HighlightingKind::Parameter:
    return OS << "Parameter";
  case HighlightingKind::Function:
    return OS << "Function";
  case HighlightingKind::Method:
    return OS << "Method";
  case HighlightingKind::StaticMethod:
    return OS << "StaticMethod";
  case HighlightingKind::Field:
    return OS << "Field";
  case HighlightingKind::StaticField:
    return OS << "StaticField";
  case HighlightingKind::Class:
    return OS << "Class";
  case HighlightingKind::Interface:
    return OS << "Interface";
  case HighlightingKind::Enum:
    return OS << "Enum";
  case HighlightingKind::EnumConstant:
    return OS << "EnumConstant";
  case HighlightingKind::Typedef:
    return OS << "Typedef";
  case HighlightingKind::Type:
    return OS << "Type";
  case HighlightingKind::Unknown:
    return OS << "Unknown";
  case HighlightingKind::Namespace:
    return OS << "Namespace";
  case HighlightingKind::TemplateParameter:
    return OS << "TemplateParameter";
  case HighlightingKind::Concept:
    return OS << "Concept";
  case HighlightingKind::Primitive:
    return OS << "Primitive";
  case HighlightingKind::Macro:
    return OS << "Macro";
  case HighlightingKind::Modifier:
    return OS << "Modifier";
  case HighlightingKind::Operator:
    return OS << "Operator";
  case HighlightingKind::Bracket:
    return OS << "Bracket";
  case HighlightingKind::Label:
    return OS << "Label";
  case HighlightingKind::InactiveCode:
    return OS << "InactiveCode";
  }
  llvm_unreachable("invalid HighlightingKind");
}
std::optional<HighlightingKind>
highlightingKindFromString(llvm::StringRef Name) {
  static llvm::StringMap<HighlightingKind> Lookup = {
      {"Variable", HighlightingKind::Variable},
      {"LocalVariable", HighlightingKind::LocalVariable},
      {"Parameter", HighlightingKind::Parameter},
      {"Function", HighlightingKind::Function},
      {"Method", HighlightingKind::Method},
      {"StaticMethod", HighlightingKind::StaticMethod},
      {"Field", HighlightingKind::Field},
      {"StaticField", HighlightingKind::StaticField},
      {"Class", HighlightingKind::Class},
      {"Interface", HighlightingKind::Interface},
      {"Enum", HighlightingKind::Enum},
      {"EnumConstant", HighlightingKind::EnumConstant},
      {"Typedef", HighlightingKind::Typedef},
      {"Type", HighlightingKind::Type},
      {"Unknown", HighlightingKind::Unknown},
      {"Namespace", HighlightingKind::Namespace},
      {"TemplateParameter", HighlightingKind::TemplateParameter},
      {"Concept", HighlightingKind::Concept},
      {"Primitive", HighlightingKind::Primitive},
      {"Macro", HighlightingKind::Macro},
      {"Modifier", HighlightingKind::Modifier},
      {"Operator", HighlightingKind::Operator},
      {"Bracket", HighlightingKind::Bracket},
      {"InactiveCode", HighlightingKind::InactiveCode},
  };

  auto It = Lookup.find(Name);
  return It != Lookup.end() ? std::make_optional(It->getValue()) : std::nullopt;
}
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, HighlightingModifier K) {
  switch (K) {
  case HighlightingModifier::Declaration:
    return OS << "decl"; // abbreviation for common case
  case HighlightingModifier::Definition:
    return OS << "def"; // abbrevation for common case
  case HighlightingModifier::ConstructorOrDestructor:
    return OS << "constrDestr";
  default:
    return OS << toSemanticTokenModifier(K);
  }
}
std::optional<HighlightingModifier>
highlightingModifierFromString(llvm::StringRef Name) {
  static llvm::StringMap<HighlightingModifier> Lookup = {
      {"Declaration", HighlightingModifier::Declaration},
      {"Definition", HighlightingModifier::Definition},
      {"Deprecated", HighlightingModifier::Deprecated},
      {"Deduced", HighlightingModifier::Deduced},
      {"Readonly", HighlightingModifier::Readonly},
      {"Static", HighlightingModifier::Static},
      {"Abstract", HighlightingModifier::Abstract},
      {"Virtual", HighlightingModifier::Virtual},
      {"DependentName", HighlightingModifier::DependentName},
      {"DefaultLibrary", HighlightingModifier::DefaultLibrary},
      {"UsedAsMutableReference", HighlightingModifier::UsedAsMutableReference},
      {"UsedAsMutablePointer", HighlightingModifier::UsedAsMutablePointer},
      {"ConstructorOrDestructor",
       HighlightingModifier::ConstructorOrDestructor},
      {"UserDefined", HighlightingModifier::UserDefined},
      {"FunctionScope", HighlightingModifier::FunctionScope},
      {"ClassScope", HighlightingModifier::ClassScope},
      {"FileScope", HighlightingModifier::FileScope},
      {"GlobalScope", HighlightingModifier::GlobalScope},
  };

  auto It = Lookup.find(Name);
  return It != Lookup.end() ? std::make_optional(It->getValue()) : std::nullopt;
}

bool operator==(const HighlightingToken &L, const HighlightingToken &R) {
  return std::tie(L.R, L.Kind, L.Modifiers) ==
         std::tie(R.R, R.Kind, R.Modifiers);
}
bool operator<(const HighlightingToken &L, const HighlightingToken &R) {
  return std::tie(L.R, L.Kind, L.Modifiers) <
         std::tie(R.R, R.Kind, R.Modifiers);
}

std::vector<SemanticToken>
toSemanticTokens(llvm::ArrayRef<HighlightingToken> Tokens,
                 llvm::StringRef Code) {
  assert(llvm::is_sorted(Tokens));
  std::vector<SemanticToken> Result;
  // In case we split a HighlightingToken into multiple tokens (e.g. because it
  // was spanning multiple lines), this tracks the last one. This prevents
  // having a copy all the time.
  HighlightingToken Scratch;
  const HighlightingToken *Last = nullptr;
  for (const HighlightingToken &Tok : Tokens) {
    Result.emplace_back();
    SemanticToken *Out = &Result.back();
    // deltaStart/deltaLine are relative if possible.
    if (Last) {
      assert(Tok.R.start.line >= Last->R.end.line);
      Out->deltaLine = Tok.R.start.line - Last->R.end.line;
      if (Out->deltaLine == 0) {
        assert(Tok.R.start.character >= Last->R.start.character);
        Out->deltaStart = Tok.R.start.character - Last->R.start.character;
      } else {
        Out->deltaStart = Tok.R.start.character;
      }
    } else {
      Out->deltaLine = Tok.R.start.line;
      Out->deltaStart = Tok.R.start.character;
    }
    Out->tokenType = static_cast<unsigned>(Tok.Kind);
    Out->tokenModifiers = Tok.Modifiers;
    Last = &Tok;

    if (Tok.R.end.line == Tok.R.start.line) {
      Out->length = Tok.R.end.character - Tok.R.start.character;
    } else {
      // If the token spans a line break, split it into multiple pieces for each
      // line.
      // This is slow, but multiline tokens are rare.
      // FIXME: There's a client capability for supporting multiline tokens,
      // respect that.
      auto TokStartOffset = llvm::cantFail(positionToOffset(Code, Tok.R.start));
      // Note that the loop doesn't cover the last line, which has a special
      // length.
      for (int I = Tok.R.start.line; I < Tok.R.end.line; ++I) {
        auto LineEnd = Code.find('\n', TokStartOffset);
        assert(LineEnd != Code.npos);
        Out->length = LineEnd - TokStartOffset;
        // Token continues on next line, right after the line break.
        TokStartOffset = LineEnd + 1;
        Result.emplace_back();
        Out = &Result.back();
        *Out = Result[Result.size() - 2];
        // New token starts at the first column of the next line.
        Out->deltaLine = 1;
        Out->deltaStart = 0;
      }
      // This is the token on last line.
      Out->length = Tok.R.end.character;
      // Update the start location for last token, as that's used in the
      // relative delta calculation for following tokens.
      Scratch = *Last;
      Scratch.R.start.line = Tok.R.end.line;
      Scratch.R.start.character = 0;
      Last = &Scratch;
    }
  }
  return Result;
}
llvm::StringRef toSemanticTokenType(HighlightingKind Kind) {
  switch (Kind) {
  case HighlightingKind::Variable:
  case HighlightingKind::LocalVariable:
  case HighlightingKind::StaticField:
    return "variable";
  case HighlightingKind::Parameter:
    return "parameter";
  case HighlightingKind::Function:
    return "function";
  case HighlightingKind::Method:
    return "method";
  case HighlightingKind::StaticMethod:
    // FIXME: better method with static modifier?
    return "function";
  case HighlightingKind::Field:
    return "property";
  case HighlightingKind::Class:
    return "class";
  case HighlightingKind::Interface:
    return "interface";
  case HighlightingKind::Enum:
    return "enum";
  case HighlightingKind::EnumConstant:
    return "enumMember";
  case HighlightingKind::Typedef:
  case HighlightingKind::Type:
    return "type";
  case HighlightingKind::Unknown:
    return "unknown"; // nonstandard
  case HighlightingKind::Namespace:
    return "namespace";
  case HighlightingKind::TemplateParameter:
    return "typeParameter";
  case HighlightingKind::Concept:
    return "concept"; // nonstandard
  case HighlightingKind::Primitive:
    return "type";
  case HighlightingKind::Macro:
    return "macro";
  case HighlightingKind::Modifier:
    return "modifier";
  case HighlightingKind::Operator:
    return "operator";
  case HighlightingKind::Bracket:
    return "bracket";
  case HighlightingKind::Label:
    return "label";
  case HighlightingKind::InactiveCode:
    return "comment";
  }
  llvm_unreachable("unhandled HighlightingKind");
}

llvm::StringRef toSemanticTokenModifier(HighlightingModifier Modifier) {
  switch (Modifier) {
  case HighlightingModifier::Declaration:
    return "declaration";
  case HighlightingModifier::Definition:
    return "definition";
  case HighlightingModifier::Deprecated:
    return "deprecated";
  case HighlightingModifier::Readonly:
    return "readonly";
  case HighlightingModifier::Static:
    return "static";
  case HighlightingModifier::Deduced:
    return "deduced"; // nonstandard
  case HighlightingModifier::Abstract:
    return "abstract";
  case HighlightingModifier::Virtual:
    return "virtual";
  case HighlightingModifier::DependentName:
    return "dependentName"; // nonstandard
  case HighlightingModifier::DefaultLibrary:
    return "defaultLibrary";
  case HighlightingModifier::UsedAsMutableReference:
    return "usedAsMutableReference"; // nonstandard
  case HighlightingModifier::UsedAsMutablePointer:
    return "usedAsMutablePointer"; // nonstandard
  case HighlightingModifier::ConstructorOrDestructor:
    return "constructorOrDestructor"; // nonstandard
  case HighlightingModifier::UserDefined:
    return "userDefined"; // nonstandard
  case HighlightingModifier::FunctionScope:
    return "functionScope"; // nonstandard
  case HighlightingModifier::ClassScope:
    return "classScope"; // nonstandard
  case HighlightingModifier::FileScope:
    return "fileScope"; // nonstandard
  case HighlightingModifier::GlobalScope:
    return "globalScope"; // nonstandard
  }
  llvm_unreachable("unhandled HighlightingModifier");
}

std::vector<SemanticTokensEdit>
diffTokens(llvm::ArrayRef<SemanticToken> Old,
           llvm::ArrayRef<SemanticToken> New) {
  // For now, just replace everything from the first-last modification.
  // FIXME: use a real diff instead, this is bad with include-insertion.

  unsigned Offset = 0;
  while (!Old.empty() && !New.empty() && Old.front() == New.front()) {
    ++Offset;
    Old = Old.drop_front();
    New = New.drop_front();
  }
  while (!Old.empty() && !New.empty() && Old.back() == New.back()) {
    Old = Old.drop_back();
    New = New.drop_back();
  }

  if (Old.empty() && New.empty())
    return {};
  SemanticTokensEdit Edit;
  Edit.startToken = Offset;
  Edit.deleteTokens = Old.size();
  Edit.tokens = New;
  return {std::move(Edit)};
}

std::vector<Range> getInactiveRegions(ParsedAST &AST) {
  std::vector<Range> SkippedRanges(std::move(AST.getMacros().SkippedRanges));
  const auto &SM = AST.getSourceManager();
  StringRef MainCode = SM.getBufferOrFake(SM.getMainFileID()).getBuffer();
  std::vector<Range> InactiveRegions;
  for (const Range &Skipped : SkippedRanges) {
    Range Inactive = Skipped;
    // Sometimes, SkippedRanges contains a range ending at position 0
    // of a line. Clients that apply whole-line styles will treat that
    // line as inactive which is not desirable, so adjust the ending
    // position to be the end of the previous line.
    if (Inactive.end.character == 0 && Inactive.end.line > 0) {
      --Inactive.end.line;
    }
    // Exclude the directive lines themselves from the range.
    if (Inactive.end.line >= Inactive.start.line + 2) {
      ++Inactive.start.line;
      --Inactive.end.line;
    } else {
      // range would be empty, e.g. #endif on next line after #ifdef
      continue;
    }
    // Since we've adjusted the ending line, we need to recompute the
    // column to reflect the end of that line.
    if (auto EndOfLine = endOfLine(MainCode, Inactive.end.line)) {
      Inactive.end = *EndOfLine;
    } else {
      elog("Failed to determine end of line: {0}", EndOfLine.takeError());
      continue;
    }
    InactiveRegions.push_back(Inactive);
  }
  return InactiveRegions;
}

} // namespace clangd
} // namespace clang
