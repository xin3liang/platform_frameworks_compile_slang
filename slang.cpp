/*
 * Copyright 2010, The Android Open Source Project
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

#include "slang.h"

#include <stdlib.h>

#include <string>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"

#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"

#include "clang/Frontend/CodeGenOptions.h"
#include "clang/Frontend/DependencyOutputOptions.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"

#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"

#include "clang/Parse/ParseAST.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"

#include "llvm/Bitcode/ReaderWriter.h"

// More force linking
#include "llvm/Linker/Linker.h"

// Force linking all passes/vmcore stuffs to libslang.so
#include "llvm/LinkAllIR.h"
#include "llvm/LinkAllPasses.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"

#include "slang_assert.h"
#include "slang_backend.h"
#include "slang_utils.h"

namespace {

static const char *kRSTriple32 = "armv7-none-linux-gnueabi";
static const char *kRSTriple64 = "aarch64-none-linux-gnueabi";

struct ForceSlangLinking {
  ForceSlangLinking() {
    // We must reference the functions in such a way that compilers will not
    // delete it all as dead code, even with whole program optimization,
    // yet is effectively a NO-OP. As the compiler isn't smart enough
    // to know that getenv() never returns -1, this will do the job.
    if (std::getenv("bar") != reinterpret_cast<char*>(-1))
      return;

    // llvm-rs-link needs following functions existing in libslang.
    llvm::parseBitcodeFile(nullptr, llvm::getGlobalContext());
    llvm::Linker::LinkModules(nullptr, nullptr, 0, nullptr);

    // llvm-rs-cc need this.
    new clang::TextDiagnosticPrinter(llvm::errs(),
                                     new clang::DiagnosticOptions());
  }
} ForceSlangLinking;

}  // namespace

namespace slang {

bool Slang::GlobalInitialized = false;

// Language option (define the language feature for compiler such as C99)
clang::LangOptions Slang::LangOpts;

// Code generation option for the compiler
clang::CodeGenOptions Slang::CodeGenOpts;

// The named of metadata node that pragma resides (should be synced with
// bcc.cpp)
const llvm::StringRef Slang::PragmaMetadataName = "#pragma";

static inline llvm::tool_output_file *
OpenOutputFile(const char *OutputFile,
               llvm::sys::fs::OpenFlags Flags,
               std::string* Error,
               clang::DiagnosticsEngine *DiagEngine) {
  slangAssert((OutputFile != nullptr) && (Error != nullptr) &&
              (DiagEngine != nullptr) && "Invalid parameter!");

  if (SlangUtils::CreateDirectoryWithParents(
                        llvm::sys::path::parent_path(OutputFile), Error)) {
    llvm::tool_output_file *F =
          new llvm::tool_output_file(OutputFile, *Error, Flags);
    if (F != nullptr)
      return F;
  }

  // Report error here.
  DiagEngine->Report(clang::diag::err_fe_error_opening)
    << OutputFile << *Error;

  return nullptr;
}

void Slang::GlobalInitialization() {
  if (!GlobalInitialized) {
    // We only support x86, x64 and ARM target

    // For ARM
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTarget();
    LLVMInitializeARMAsmPrinter();

    // For x86 and x64
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86AsmPrinter();

    // Please refer to include/clang/Basic/LangOptions.h to setup
    // the options.
    LangOpts.RTTI = 0;  // Turn off the RTTI information support
    LangOpts.C99 = 1;
    LangOpts.Renderscript = 1;
    LangOpts.LaxVectorConversions = 0;  // Do not bitcast vectors!
    LangOpts.CharIsSigned = 1;  // Signed char is our default.

    CodeGenOpts.OptimizationLevel = 3;

    GlobalInitialized = true;
  }
}

void Slang::LLVMErrorHandler(void *UserData, const std::string &Message,
                             bool GenCrashDialog) {
  clang::DiagnosticsEngine* DiagEngine =
    static_cast<clang::DiagnosticsEngine *>(UserData);

  DiagEngine->Report(clang::diag::err_fe_error_backend) << Message;
  exit(1);
}

void Slang::createTarget(uint32_t BitWidth) {
  std::vector<std::string> features;

  if (BitWidth == 64) {
    mTargetOpts->Triple = kRSTriple64;
  } else {
    mTargetOpts->Triple = kRSTriple32;
    // Treat long as a 64-bit type for our 32-bit RS code.
    features.push_back("+long64");
    mTargetOpts->FeaturesAsWritten = features;
  }

  mTarget.reset(clang::TargetInfo::CreateTargetInfo(*mDiagEngine,
                                                    mTargetOpts));
}

void Slang::createFileManager() {
  mFileSysOpt.reset(new clang::FileSystemOptions());
  mFileMgr.reset(new clang::FileManager(*mFileSysOpt));
}

void Slang::createSourceManager() {
  mSourceMgr.reset(new clang::SourceManager(*mDiagEngine, *mFileMgr));
}

void Slang::createPreprocessor() {
  // Default only search header file in current dir
  llvm::IntrusiveRefCntPtr<clang::HeaderSearchOptions> HSOpts =
      new clang::HeaderSearchOptions();
  clang::HeaderSearch *HeaderInfo = new clang::HeaderSearch(HSOpts,
                                                            *mSourceMgr,
                                                            *mDiagEngine,
                                                            LangOpts,
                                                            mTarget.get());

  llvm::IntrusiveRefCntPtr<clang::PreprocessorOptions> PPOpts =
      new clang::PreprocessorOptions();
  mPP.reset(new clang::Preprocessor(PPOpts,
                                    *mDiagEngine,
                                    LangOpts,
                                    *mSourceMgr,
                                    *HeaderInfo,
                                    *this,
                                    nullptr,
                                    /* OwnsHeaderSearch = */true));
  // Initialize the preprocessor
  mPP->Initialize(getTargetInfo());
  clang::FrontendOptions FEOpts;
  clang::InitializePreprocessor(*mPP, *PPOpts, FEOpts);

  mPragmas.clear();
  mPP->AddPragmaHandler(new PragmaRecorder(&mPragmas));

  std::vector<clang::DirectoryLookup> SearchList;
  for (unsigned i = 0, e = mIncludePaths.size(); i != e; i++) {
    if (const clang::DirectoryEntry *DE =
            mFileMgr->getDirectory(mIncludePaths[i])) {
      SearchList.push_back(clang::DirectoryLookup(DE,
                                                  clang::SrcMgr::C_System,
                                                  false));
    }
  }

  HeaderInfo->SetSearchPaths(SearchList,
                             /* angledDirIdx = */1,
                             /* systemDixIdx = */1,
                             /* noCurDirSearch = */false);

  initPreprocessor();
}

void Slang::createASTContext() {
  mASTContext.reset(new clang::ASTContext(LangOpts,
                                          *mSourceMgr,
                                          mPP->getIdentifierTable(),
                                          mPP->getSelectorTable(),
                                          mPP->getBuiltinInfo()));
  mASTContext->InitBuiltinTypes(getTargetInfo());
  initASTContext();
}

clang::ASTConsumer *
Slang::createBackend(const clang::CodeGenOptions& CodeGenOpts,
                     llvm::raw_ostream *OS, OutputType OT) {
  return new Backend(mDiagEngine, CodeGenOpts, getTargetOptions(),
                     &mPragmas, OS, OT);
}

Slang::Slang() : mInitialized(false), mDiagClient(nullptr),
  mTargetOpts(new clang::TargetOptions()), mOT(OT_Default) {
  GlobalInitialization();
}

void Slang::init(uint32_t BitWidth, clang::DiagnosticsEngine *DiagEngine,
                 DiagnosticBuffer *DiagClient) {
  if (mInitialized)
    return;

  mDiagEngine = DiagEngine;
  mDiagClient = DiagClient;
  mDiag.reset(new clang::Diagnostic(mDiagEngine));
  initDiagnostic();
  llvm::install_fatal_error_handler(LLVMErrorHandler, mDiagEngine);

  createTarget(BitWidth);
  createFileManager();
  createSourceManager();

  mInitialized = true;
}

clang::ModuleLoadResult Slang::loadModule(
    clang::SourceLocation ImportLoc,
    clang::ModuleIdPath Path,
    clang::Module::NameVisibilityKind Visibility,
    bool IsInclusionDirective) {
  slangAssert(0 && "Not implemented");
  return clang::ModuleLoadResult();
}

bool Slang::setInputSource(llvm::StringRef InputFile,
                           const char *Text,
                           size_t TextLength) {
  mInputFileName = InputFile.str();

  // Reset the ID tables if we are reusing the SourceManager
  mSourceMgr->clearIDTables();

  // Load the source
  llvm::MemoryBuffer *SB =
      llvm::MemoryBuffer::getMemBuffer(Text, Text + TextLength);
  mSourceMgr->setMainFileID(mSourceMgr->createFileID(SB));

  if (mSourceMgr->getMainFileID().isInvalid()) {
    mDiagEngine->Report(clang::diag::err_fe_error_reading) << InputFile;
    return false;
  }
  return true;
}

bool Slang::setInputSource(llvm::StringRef InputFile) {
  mInputFileName = InputFile.str();

  mSourceMgr->clearIDTables();

  const clang::FileEntry *File = mFileMgr->getFile(InputFile);
  if (File) {
    mSourceMgr->setMainFileID(mSourceMgr->createFileID(File,
        clang::SourceLocation(), clang::SrcMgr::C_User));
  }

  if (mSourceMgr->getMainFileID().isInvalid()) {
    mDiagEngine->Report(clang::diag::err_fe_error_reading) << InputFile;
    return false;
  }

  return true;
}

bool Slang::setOutput(const char *OutputFile) {
  std::string Error;
  llvm::tool_output_file *OS = nullptr;

  switch (mOT) {
    case OT_Dependency:
    case OT_Assembly:
    case OT_LLVMAssembly: {
      OS = OpenOutputFile(OutputFile, llvm::sys::fs::F_Text, &Error,
          mDiagEngine);
      break;
    }
    case OT_Nothing: {
      break;
    }
    case OT_Object:
    case OT_Bitcode: {
      OS = OpenOutputFile(OutputFile, llvm::sys::fs::F_None,
                          &Error, mDiagEngine);
      break;
    }
    default: {
      llvm_unreachable("Unknown compiler output type");
    }
  }

  if (!Error.empty())
    return false;

  mOS.reset(OS);

  mOutputFileName = OutputFile;

  return true;
}

bool Slang::setDepOutput(const char *OutputFile) {
  std::string Error;

  mDOS.reset(
      OpenOutputFile(OutputFile, llvm::sys::fs::F_Text, &Error, mDiagEngine));
  if (!Error.empty() || (mDOS.get() == nullptr))
    return false;

  mDepOutputFileName = OutputFile;

  return true;
}

int Slang::generateDepFile() {
  if (mDiagEngine->hasErrorOccurred())
    return 1;
  if (mDOS.get() == nullptr)
    return 1;

  // Initialize options for generating dependency file
  clang::DependencyOutputOptions DepOpts;
  DepOpts.IncludeSystemHeaders = 1;
  DepOpts.OutputFile = mDepOutputFileName;
  DepOpts.Targets = mAdditionalDepTargets;
  DepOpts.Targets.push_back(mDepTargetBCFileName);
  for (std::vector<std::string>::const_iterator
           I = mGeneratedFileNames.begin(), E = mGeneratedFileNames.end();
       I != E;
       I++) {
    DepOpts.Targets.push_back(*I);
  }
  mGeneratedFileNames.clear();

  // Per-compilation needed initialization
  createPreprocessor();
  clang::DependencyFileGenerator::CreateAndAttachToPreprocessor(*mPP.get(), DepOpts);

  // Inform the diagnostic client we are processing a source file
  mDiagClient->BeginSourceFile(LangOpts, mPP.get());

  // Go through the source file (no operations necessary)
  clang::Token Tok;
  mPP->EnterMainSourceFile();
  do {
    mPP->Lex(Tok);
  } while (Tok.isNot(clang::tok::eof));

  mPP->EndSourceFile();

  // Declare success if no error
  if (!mDiagEngine->hasErrorOccurred())
    mDOS->keep();

  // Clean up after compilation
  mPP.reset();
  mDOS.reset();

  return mDiagEngine->hasErrorOccurred() ? 1 : 0;
}

int Slang::compile() {
  if (mDiagEngine->hasErrorOccurred())
    return 1;
  if (mOS.get() == nullptr)
    return 1;

  // Here is per-compilation needed initialization
  createPreprocessor();
  createASTContext();

  mBackend.reset(createBackend(CodeGenOpts, &mOS->os(), mOT));

  // Inform the diagnostic client we are processing a source file
  mDiagClient->BeginSourceFile(LangOpts, mPP.get());

  // The core of the slang compiler
  ParseAST(*mPP, mBackend.get(), *mASTContext);

  // Inform the diagnostic client we are done with previous source file
  mDiagClient->EndSourceFile();

  // Declare success if no error
  if (!mDiagEngine->hasErrorOccurred())
    mOS->keep();

  // The compilation ended, clear
  mBackend.reset();
  mASTContext.reset();
  mPP.reset();
  mOS.reset();

  return mDiagEngine->hasErrorOccurred() ? 1 : 0;
}

void Slang::setDebugMetadataEmission(bool EmitDebug) {
  if (EmitDebug)
    CodeGenOpts.setDebugInfo(clang::CodeGenOptions::FullDebugInfo);
  else
    CodeGenOpts.setDebugInfo(clang::CodeGenOptions::NoDebugInfo);
}

void Slang::setOptimizationLevel(llvm::CodeGenOpt::Level OptimizationLevel) {
  CodeGenOpts.OptimizationLevel = OptimizationLevel;
}

void Slang::reset(bool SuppressWarnings) {
  // Always print diagnostics if we had an error occur, but don't print
  // warnings if we suppressed them (i.e. we are doing the 64-bit compile after
  // an existing 32-bit compile).
  //
  // TODO: This should really be removing duplicate identical warnings between
  // the 32-bit and 64-bit compiles, but that is a more substantial feature.
  // Bug: 17052573
  if (!SuppressWarnings || mDiagEngine->hasErrorOccurred()) {
    llvm::errs() << mDiagClient->str();
  }
  mDiagEngine->Reset();
  mDiagClient->reset();
}

Slang::~Slang() {
}

}  // namespace slang
