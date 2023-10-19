/*
  Copyright 2015 Google LLC All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.
*/

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <unordered_map>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

using namespace llvm;

#include "afl-untouch.h"

using namespace std;

namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;
      AFLCoverage() : ModulePass(ID) { }

      bool runOnModule(Module &M) override;

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }

  };

}


char AFLCoverage::ID = 0;


bool AFLCoverage::runOnModule(Module &M) {

  LLVMContext &C = M.getContext();

  IntegerType *Int8Ty  = IntegerType::getInt8Ty(C);
  IntegerType *Int32Ty = IntegerType::getInt32Ty(C);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  GlobalVariable *AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

  GlobalVariable *AFLPrevLoc = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  GlobalVariable* AFLUntouchPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                        GlobalValue::ExternalLinkage, 0, "__afl_untouch_ptr");

  GlobalVariable* AFLBBId = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_bb_ids",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */

  int inst_blocks = 0;

  for (auto &F : M) {

    CFGraph cfg;
    cfg.bb_num = 0;
    cfg.edge_num = 0;
    int bb_idx = 0;

    for (auto& BB : F) {

        bb_idx = insert_bb(&cfg, &BB, 0);

        BBNode* bb = &cfg.list[bb_idx];
        StringRef bb_name = bb->bb->getName();

        if (bb_name.empty()) {
            string Str;
            string temp;
            raw_string_ostream OS(Str);
            bb->bb->printAsOperand(OS, false);
            string func_name = bb->bb->getParent()->getName().str();
            temp = func_name + OS.str();
            bb->bb_name = temp;
            bb->bb->setName(StringRef(temp));
            
        } 

        BranchInst* BI = dyn_cast<BranchInst>(BB.getTerminator());

        if (!BI) continue; // It is impossible because we split switches to if...then

        if (!BI->getNumSuccessors()) continue;  // RET basicblock

        // we calculate the sum of the functions called in this basicblock!
        for (auto& INST : BB) {

            if (CallInst* CI = dyn_cast<CallInst>(&INST)) {

                if (Function* CF = CI->getCalledFunction()) {

                    StringRef F_name = CF->getName();
                    if (!F_name.find("llvm.")) continue;
                    cfg.list[bb_idx].calledFunNum++;

                }
            }
        }

        // CFG buildinig ...

        for (auto SI = succ_begin(&BB), E = succ_end(&BB); SI != E; SI++) {

            BasicBlock* SuccBB = *SI;

            int suc_bb_idx = insert_bb(&cfg, SuccBB, 0);

            insert_edge(&cfg, SuccBB, bb_idx, suc_bb_idx);

        }

    } // end building CFG

    loop_detect(&cfg);

    for (auto &BB : F) {

      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(&(*IP));

      if (AFL_R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int cur_loc = AFL_R(MAP_SIZE);

      // store bb_id

      int bb_idx = search_bb(&cfg, &BB);
      if (bb_idx == -1) {
          // impossible?!
          errs() << "No find?!" << "\n";
          exit(-1);
      }
      cfg.list[bb_idx].cur_loc = cur_loc;

      ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

      if (&BB != &F.front()) {

          /* Load __afl_prev_loc */

          LoadInst* PrevLoc = IRB.CreateLoad(AFLPrevLoc);
          PrevLoc->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt32Ty());

          /* Load __afl_bb_ids */

          LoadInst* BBId = IRB.CreateLoad(AFLBBId);
          BBId->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* BBIdCasted = IRB.CreateZExt(BBId, IRB.getInt32Ty());

          /* Update __afl_untouch_ptr */

          Value* CurEdgeId = IRB.CreateXor(PrevLocCasted, CurLoc);

          Value* UntouchID = IRB.CreateXor(CurEdgeId, BBIdCasted);

          LoadInst* UntouchPtr = IRB.CreateLoad(AFLUntouchPtr);
          UntouchPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* UntouchPtrIdx =
              IRB.CreateGEP(UntouchPtr, UntouchID);

          IRB.CreateStore(ConstantInt::get(Int8Ty, 1), UntouchPtrIdx)
              ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

          /* Load SHM pointer */

          LoadInst* MapPtr = IRB.CreateLoad(AFLMapPtr);
          MapPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* MapPtrIdx =
              IRB.CreateGEP(MapPtr, CurEdgeId);

          /* Update bitmap */

          LoadInst* Counter = IRB.CreateLoad(MapPtrIdx);
          Counter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
          IRB.CreateStore(Incr, MapPtrIdx)
              ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

          /* Set prev_loc to cur_loc >> 1 */

          StoreInst* Store =
              IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
          Store->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

          inst_blocks++;

      }
      else {

          // In first BB, we don't need to collect untouch information!

          /* Load prev_loc */

          LoadInst* PrevLoc = IRB.CreateLoad(AFLPrevLoc);
          PrevLoc->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt32Ty());

          /* Load SHM pointer */

          LoadInst* MapPtr = IRB.CreateLoad(AFLMapPtr);
          MapPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* MapPtrIdx =
              IRB.CreateGEP(MapPtr, IRB.CreateXor(PrevLocCasted, CurLoc));

          /* Update bitmap */

          LoadInst* Counter = IRB.CreateLoad(MapPtrIdx);
          Counter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
          Value* Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
          IRB.CreateStore(Incr, MapPtrIdx)
              ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

          /* Set prev_loc to cur_loc >> 1 */

          StoreInst* Store =
              IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
          Store->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

          inst_blocks++;

      }

    }

    for (auto& BB : F) {

        BasicBlock::iterator IP = BB.getFirstInsertionPt();
        IRBuilder<> IRB(&(*IP));

        int bb_idx = search_bb(&cfg, &BB);

        if (bb_idx == -1) {
            errs() << "Really???" << "\n";
            exit(-1);
        }

        BranchInst* BI = dyn_cast<BranchInst>(BB.getTerminator());

        if (!BI) {
            if (!cfg.list[bb_idx].outdegree) {
                continue;
            }
            else {
                errs() << BB << "\n";
                errs() << "Impossible?! Here is a switch inst?\n";
                exit(0);
            }
        }

        if (BI->isUnconditional() || BI->getNumSuccessors() < 2) {

            IRBuilder<> IRB_last(BB.getTerminator());

            IRB_last.CreateStore(ConstantInt::get(Int32Ty, 0), AFLBBId)
                ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

            continue;

        }

        BasicBlock* bb_left = dyn_cast<BasicBlock>(BI->getOperand(1));
        BasicBlock* bb_right = dyn_cast<BasicBlock>(BI->getOperand(2));

        int bb_idx_l = search_bb(&cfg, bb_left);
        int bb_idx_r = search_bb(&cfg, bb_right);
        if (bb_idx_l == -1 || bb_idx_r == -1) {
            errs() << "Really???" << "\n";
            exit(-1);
        }

        IRBuilder<> IRB_last(BB.getTerminator());

        /* Update __afl_bb_ids */

        IRB_last.CreateStore(ConstantInt::get(Int32Ty, cfg.list[bb_idx_l].cur_loc ^ cfg.list[bb_idx_r].cur_loc), AFLBBId)
            ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

    }
  }
  /* Say something nice. */

  if (!be_quiet) {

    if (!inst_blocks) WARNF("No instrumentation targets found.");
    else OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks, getenv("AFL_HARDEN") ? "hardened" :
             ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
              "ASAN/MSAN" : "non-hardened"), inst_ratio);

  }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}


static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
