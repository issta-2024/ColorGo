// This file is part of SymCC.
//
// SymCC is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// SymCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// SymCC. If not, see <https://www.gnu.org/licenses/>.

#include "Pass.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/CodeGen/IntrinsicLowering.h>
#include <llvm/CodeGen/TargetLowering.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#if LLVM_VERSION_MAJOR < 14
#include <llvm/Support/TargetRegistry.h>
#else
#include <llvm/MC/TargetRegistry.h>
#endif

#include "Runtime.h"
#include "Symbolizer.h"

using namespace llvm;

static cl::opt<std::string>
    TargetsLineFile("targetsline",
                    cl::desc("Input file containing the target lines of code."),
                    cl::value_desc("targetsline"));
static cl::opt<std::string> TargetsFuncFile(
    "targetsfunc",
    cl::desc("Input file containing the target functions of code."),
    cl::value_desc("targetsfunc"));

#ifndef NDEBUG
#define DEBUG(X)                                                               \
  do {                                                                         \
    X;                                                                         \
  } while (false)
#else
#define DEBUG(X) ((void)0)
#endif

char SymbolizeLegacyPass::ID = 0;

namespace {

static constexpr char kSymCtorName[] = "__sym_ctor";
std::list<std::string> targets;
std::unordered_map<std::string, std::unordered_set<std::string>>
    funcName2TaregetFuncName;
std::unordered_map<std::string, std::unordered_set<BasicBlock *>>
    funcName2TaregetBB;
std::vector<std::string> skip_functions;

// void exploreMap(
//     const std::unordered_map<std::string, std::unordered_set<std::string>>
//         &map) {
//   for (const auto &entry : map) {
//     const std::string &key = entry.first;
//     const std::unordered_set<std::string> &value = entry.second;

//     outs() << key << " calls: ";

//     for (const auto &target : value) {
//       outs() << target << " ";
//     }
//     outs() << '\n';
//   }
// }

void GetTarget() {
  if (TargetsLineFile.empty()) {
    return;
  }
  // outs() << TargetsLineFile << "\n";

  /* 按行读取目标点文件，存储到target中
   */
  std::ifstream targetslinefile(TargetsLineFile);
  std::string line;
  while (std::getline(targetslinefile, line)) {
    targets.push_back(line);
    // outs() << line << "\n";
  }

  targetslinefile.close();

  if (TargetsFuncFile.empty()) {
    return;
  }
  // outs() << TargetsFuncFile << "\n";

  /* 按行读取目标点文件，存储到target中
   */
  std::ifstream targetsfuncfile(TargetsFuncFile);
  while (std::getline(targetsfuncfile, line)) {
    if (line[0] == '*') {
      skip_functions.push_back(line.substr(1));
      continue;
    }
    // outs() << line << "\n";
    std::stringstream ss(line);
    std::string token;
    std::getline(ss, token, ':'); // 解析出主函数名
    std::string callFunction = token;
    std::getline(ss, token); // 解析出调用函数列表
    std::stringstream function_ss(token);

    funcName2TaregetFuncName.insert(
        std::pair<std::string, std::unordered_set<std::string>>(
            callFunction, std::unordered_set<std::string>()));
    while (std::getline(function_ss, token, ',')) {
      if (funcName2TaregetFuncName[callFunction].count(token) == 0) {
        funcName2TaregetFuncName[callFunction].emplace(token);
      }
    }
  }

  targetsfuncfile.close();
}

static void getDebugLoc(const Instruction *I, std::string &Filename,
                        unsigned &Line) {
  DebugLoc Loc = I->getDebugLoc();
  if (bool(Loc)) {
    Line = Loc.getLine();
    Filename = Loc.get()->getFilename();
  }
  // outs() << Filename << ":" << Line << "\n";
}

void PrintFuncName2TargetBB(std::string funcName) {
  outs() << funcName << ':';
  for (auto targetBBItem : funcName2TaregetBB[funcName]) {
    targetBBItem->printAsOperand(outs(), false);
    outs() << ' ';
  }
  outs() << '\n';
}

bool instrumentModule(Module &M) {
  DEBUG(errs() << "Symbolizer module instrumentation\n");
  GetTarget();
  // Redirect calls to external functions to the corresponding wrappers and
  // rename internal functions.
  for (auto &function : M.functions()) {
    auto name = function.getName();
    if (isInterceptedFunction(function))
      function.setName(name + "_symbolized");
  }

  // Insert a constructor that initializes the runtime and any globals.
  Function *ctor;
  std::tie(ctor, std::ignore) = createSanitizerCtorAndInitFunctions(
      M, kSymCtorName, "_sym_initialize", {}, {});
  appendToGlobalCtors(M, ctor, 0);

  return true;
}

bool canLower(const CallInst *CI) {
  const Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;

  switch (Callee->getIntrinsicID()) {
  case Intrinsic::expect:
  case Intrinsic::ctpop:
  case Intrinsic::ctlz:
  case Intrinsic::cttz:
  case Intrinsic::prefetch:
  case Intrinsic::pcmarker:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_label:
  case Intrinsic::eh_typeid_for:
  case Intrinsic::annotation:
  case Intrinsic::ptr_annotation:
  case Intrinsic::assume:
#if LLVM_VERSION_MAJOR > 11
  case Intrinsic::experimental_noalias_scope_decl:
#endif
  case Intrinsic::var_annotation:
  case Intrinsic::sqrt:
  case Intrinsic::log:
  case Intrinsic::log2:
  case Intrinsic::log10:
  case Intrinsic::exp:
  case Intrinsic::exp2:
  case Intrinsic::pow:
  case Intrinsic::sin:
  case Intrinsic::cos:
  case Intrinsic::floor:
  case Intrinsic::ceil:
  case Intrinsic::trunc:
  case Intrinsic::round:
#if LLVM_VERSION_MAJOR > 10
  case Intrinsic::roundeven:
#endif
  case Intrinsic::copysign:
#if LLVM_VERSION_MAJOR < 16
  case Intrinsic::flt_rounds:
#else
  case Intrinsic::get_rounding:
#endif
  case Intrinsic::invariant_start:
  case Intrinsic::lifetime_start:
  case Intrinsic::invariant_end:
  case Intrinsic::lifetime_end:
    return true;
  default:
    return false;
  }

  llvm_unreachable("Control cannot reach here");
}

void liftInlineAssembly(CallInst *CI) {
  // TODO When we don't have to worry about the old pass manager anymore,
  // move the initialization to the pass constructor. (Currently there are
  // two passes, but only if we're on a recent enough LLVM...)

  Function *F = CI->getFunction();
  Module *M = F->getParent();
  auto triple = M->getTargetTriple();

  std::string error;
  auto target = TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    errs() << "Warning: can't get target info to lift inline assembly\n";
    return;
  }

  auto cpu = F->getFnAttribute("target-cpu").getValueAsString();
  auto features = F->getFnAttribute("target-features").getValueAsString();

  std::unique_ptr<TargetMachine> TM(
      target->createTargetMachine(triple, cpu, features, TargetOptions(), {}));
  auto subTarget = TM->getSubtargetImpl(*F);
  if (subTarget == nullptr)
    return;

  auto targetLowering = subTarget->getTargetLowering();
  if (targetLowering == nullptr)
    return;

  targetLowering->ExpandInlineAsm(CI);
}
void getAllTargetBB(BasicBlock *BB,
                    std::unordered_set<BasicBlock *> &allTargetBBs) {
  if (BB != nullptr && allTargetBBs.count(BB) == 0) {
    allTargetBBs.emplace(BB);
    for (llvm::BasicBlock *SuccBB : predecessors(BB)) {
      getAllTargetBB(SuccBB, allTargetBBs);
    }
  }
}
std::unordered_set<BasicBlock *> getFuncTargetBB(Function &F) {
  std::unordered_set<BasicBlock *> allTargetBBs;
  std::string funcName = F.getName().str();
  // outs() << funcName << '\n';
  for (auto &BB : F) {
    std::string bb_name("");
    std::string filename;
    unsigned line;
    bool isTargetBB = false;
    // bool isTargetBBLine = false;

    for (auto &I : BB) {
      if (isTargetBB)
        break;
      if (CallInst *inst = dyn_cast<CallInst>(
              &I)) { // 使用dyn_cast函数来判断指令是callInst还是invokeInst,当是这两个指令的时候，解析这两个指令，通过getCalledFunction()函数来获得所调用的函数，
        Function *called = inst->getCalledFunction();
        if (called != nullptr) {
          std::string calledName = called->getName();
          if (funcName2TaregetFuncName[funcName].count(calledName) > 0) {
            isTargetBB = true;
          }
        }
      }
      if (InvokeInst *inst = dyn_cast<InvokeInst>(
              &I)) { // 使用dyn_cast函数来判断指令是callInst还是invokeInst,当是这两个指令的时候，解析这两个指令，通过getCalledFunction()函数来获得所调用的函数，
        Function *called = inst->getCalledFunction();
        if (called != nullptr) {
          std::string calledName = called->getName();
          if (funcName2TaregetFuncName[funcName].count(calledName) > 0) {
            isTargetBB = true;
          }
        }
      }

      getDebugLoc(&I, filename, line);

      if (bb_name == "")
        bb_name = filename + ":" + std::to_string(line);

      /* Don't worry about external libs */
      /* 去除掉一些外部库，以及没有位置信息的指令
       */
      static const std::string Xlibs("/usr/");
      if (filename.empty() || line == 0 ||
          !filename.compare(0, Xlibs.size(), Xlibs))
        continue;

      // std::size_t i = 0;
      for (auto &target : targets) {
        std::size_t found = filename.find_last_of("/\\");
        if (found != std::string::npos)
          filename = filename.substr(found + 1);

        std::size_t pos = target.find_last_of(":");
        std::string target_file = target.substr(0, pos);
        unsigned int target_line = atoi(target.substr(pos + 1).c_str());

        if (!target_file.compare(filename) && target_line == line) {
          isTargetBB = true;
          // if (i == targets.size() - 1)
          //   isTargetBBLine = true;
        }
        // i++;
      }
    }
    // if (isTargetBBLine) {
    //   IRBuilder<> builder(&BB);
    //   // 在当前基本块之后插入一个新的基本块
    //   BasicBlock *newBlock = BasicBlock::Create(F.getContext(), "newBlock",
    //   &F); builder.CreateBr(newBlock);

    //   // 在新的基本块中添加指令
    //   builder.SetInsertPoint(newBlock);
    //   // 获取或创建fopen函数的类型
    //   FunctionType *FopenFuncType = FunctionType::get(
    //       builder.getInt8PtrTy(),
    //       {builder.getInt8PtrTy(), builder.getInt8PtrTy()}, false);
    //   FunctionCallee FopenFunc =
    //       BB.getParent()->getParent()->getOrInsertFunction("fopen",
    //                                                        FopenFuncType);
    //   Value *FileName = builder.CreateGlobalStringPtr("target");
    //   Value *Mode = builder.CreateGlobalStringPtr("w");
    //   builder.CreateCall(FopenFunc, {FileName, Mode}, "filePtr");
    // }
    if (!bb_name.empty()) {
      /* 这里是设置基本块名字，但是因为有一些基本块在源码中位置在同一行，
         故同名的设置会失效，所以当发现名字没有设置上时，为其创建一个allocator，
         则会自动在名字后生成随机数，避免了同名问题
      */
      BB.setName(bb_name + ":");
      if (!BB.hasName()) {
        std::string newname = bb_name + ":";
        Twine t(newname);
        SmallString<256> NameData;
        StringRef NameRef = t.toStringRef(NameData);
        MallocAllocator Allocator;
        BB.setValueName(ValueName::Create(NameRef, Allocator));
      }
    }
    if (isTargetBB) {
      errs() << "targetBB:";
      BB.printAsOperand(errs(), false);
      errs() << "\n";
      if (funcName2TaregetBB.find(F.getName()) == funcName2TaregetBB.end()) {
        funcName2TaregetBB.insert(
            std::pair<std::string, std::unordered_set<BasicBlock *>>(
                F.getName(), std::unordered_set<BasicBlock *>()));
      }
      if (funcName2TaregetBB[F.getName()].count(&BB) == 0) {
        funcName2TaregetBB[F.getName()].emplace(&BB);
        if (!std::count(skip_functions.begin(), skip_functions.end(),
                        funcName)) {
          getAllTargetBB(&BB, allTargetBBs);
        }else{
          allTargetBBs.emplace(&BB);
        }
      }
    }
  }

  return allTargetBBs;
}

bool instrumentFunction(Function &F) {
  auto functionName = F.getName();
  if (functionName == kSymCtorName)
    return false;

  Symbolizer symbolizer(*F.getParent());
  if (std::count(skip_functions.begin(), skip_functions.end(), functionName)) {
    symbolizer.setSkipTerminate(true);
  } else {
    symbolizer.setSkipTerminate(false);
  }
  std::unordered_set<BasicBlock *> allTargetBBs = getFuncTargetBB(F);
  if (allTargetBBs.size()) {
    symbolizer.setTargetBB(allTargetBBs);
    PrintFuncName2TargetBB(functionName);
    errs() << "alltargetBBs: ";
    for (auto targetBBItem : allTargetBBs) {
      targetBBItem->printAsOperand(errs(), false);
      errs() << ' ' << targetBBItem->getName() << ' ';
    }
    errs() << '\n';
  }

  DEBUG(errs() << "Symbolizing function ");
  DEBUG(errs().write_escaped(functionName) << '\n');

  SmallVector<Instruction *, 0> allInstructions;
  allInstructions.reserve(F.getInstructionCount());
  for (auto &I : instructions(F))
    allInstructions.push_back(&I);

  IntrinsicLowering IL(F.getParent()->getDataLayout());
  for (auto *I : allInstructions) {
    if (auto *CI = dyn_cast<CallInst>(I)) {
      if (canLower(CI)) {
        IL.LowerIntrinsicCall(CI);
      } else if (isa<InlineAsm>(CI->getCalledOperand())) {
        liftInlineAssembly(CI);
      }
    }
  }
  allInstructions.clear();
  for (auto &I : instructions(F))
    allInstructions.push_back(&I);

  symbolizer.symbolizeFunctionArguments(F);

  for (auto &basicBlock : F)
    symbolizer.insertBasicBlockNotification(basicBlock);

  for (auto *instPtr : allInstructions) {
    symbolizer.visit(instPtr);
  }

  symbolizer.finalizePHINodes();
  symbolizer.shortCircuitExpressionUses();
  if (functionName == "_bfd_dwarf2_find_nearest_line")
    DEBUG(errs() << F << '\n');
  assert(!verifyFunction(F, &errs()) &&
         "SymbolizePass produced invalid bitcode");

  return true;
}

} // namespace

bool SymbolizeLegacyPass::doInitialization(Module &M) {
  return instrumentModule(M);
}

bool SymbolizeLegacyPass::runOnFunction(Function &F) {
  return instrumentFunction(F);
}

#if LLVM_VERSION_MAJOR >= 13

PreservedAnalyses SymbolizePass::run(Function &F, FunctionAnalysisManager &) {
  return instrumentFunction(F) ? PreservedAnalyses::none()
                               : PreservedAnalyses::all();
}

PreservedAnalyses SymbolizePass::run(Module &M, ModuleAnalysisManager &) {
  return instrumentModule(M) ? PreservedAnalyses::none()
                             : PreservedAnalyses::all();
}
#endif
