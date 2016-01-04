/// \file
/// \brief This file handles the whole translation process from the input
///        assembly to LLVM IR.

// Standard includes
#include <cstdint>
#include <sstream>
#include <vector>
#include <fstream>

// LLVM includes
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Scalar.h"

// Local includes
#include "codegenerator.h"
#include "debughelper.h"
#include "instructiontranslator.h"
#include "ir-helpers.h"
#include "jumptargetmanager.h"
#include "ptcinterface.h"
#include "variablemanager.h"

using namespace llvm;

template<typename T, typename... Args>
inline std::array<T, sizeof...(Args)>
make_array(Args&&... args)
{ return { std::forward<Args>(args)... };  }

// Outline the destructor for the sake of privacy in the header
CodeGenerator::~CodeGenerator() = default;

CodeGenerator::CodeGenerator(Architecture& Source,
                             Architecture& Target,
                             std::string Output,
                             std::string Helpers,
                             DebugInfoType DebugInfo,
                             std::string Debug) :
  SourceArchitecture(Source),
  TargetArchitecture(Target),
  Context(getGlobalContext()),
  TheModule((new Module("top", Context))),
  OutputPath(Output),
  Debug(new DebugHelper(Output, Debug, TheModule.get(), DebugInfo))
{
  OriginalInstrMDKind = Context.getMDKindID("oi");
  PTCInstrMDKind = Context.getMDKindID("pi");
  DbgMDKind = Context.getMDKindID("dbg");

  SMDiagnostic Errors;
  HelpersModule = parseIRFile(Helpers, Errors, Context);
}

static BasicBlock *replaceFunction(Function *ToReplace) {
  ToReplace->setLinkage(GlobalValue::InternalLinkage);
  ToReplace->dropAllReferences();

  return BasicBlock::Create(ToReplace->getParent()->getContext(),
                            "",
                            ToReplace);
}

static void replaceFunctionWithRet(Function *ToReplace, uint64_t Result) {
  if (ToReplace == nullptr)
    return;

  BasicBlock *Body = replaceFunction(ToReplace);
  Value *ResultValue;

  if (ToReplace->getReturnType()->isVoidTy()) {
    assert(Result == 0);
    ResultValue = nullptr;
  } else if (ToReplace->getReturnType()->isIntegerTy()) {
    auto *ReturnType = cast<IntegerType>(ToReplace->getReturnType());
    ResultValue = ConstantInt::get(ReturnType, Result, false);
  } else {
    assert("No-op functions can only return void or an integer type");
  }

  ReturnInst::Create(ToReplace->getParent()->getContext(), ResultValue, Body);
}

void CodeGenerator::translate(size_t LoadAddress,
                              ArrayRef<uint8_t> Code,
                              size_t VirtualAddress,
                              std::string Name) {
  const uint8_t *CodePointer = Code.data();
  const uint8_t *CodeEnd = CodePointer + Code.size();

  IRBuilder<> Builder(Context);

  // Create main function
  auto *MainType  = FunctionType::get(Builder.getVoidTy(), false);
  auto *MainFunction = Function::Create(MainType,
                                        Function::ExternalLinkage,
                                        Name,
                                        TheModule.get());

  Debug->newFunction(MainFunction);

  // Create the first basic block and create a placeholder for variable
  // allocations
  BasicBlock *Entry = BasicBlock::Create(Context,
                                         "entrypoint",
                                         MainFunction);
  Builder.SetInsertPoint(Entry);
  Instruction *Delimiter = Builder.CreateUnreachable();

  // Instantiate helpers
  VariableManager Variables(*TheModule,
                            *HelpersModule);

  GlobalVariable *PCReg = Variables.getByEnvOffset(ptc.pc, "pc");

  JumpTargetManager JumpTargets(*TheModule, PCReg, MainFunction);
  std::map<std::string, BasicBlock *> LabeledBasicBlocks;
  std::vector<BasicBlock *> Blocks;

  InstructionTranslator Translator(Builder,
                                   Variables,
                                   JumpTargets,
                                   LabeledBasicBlocks,
                                   Blocks,
                                   *TheModule,
                                   MainFunction,
                                   SourceArchitecture,
                                   TargetArchitecture);

  ptc.mmap(LoadAddress, Code.data(), Code.size());

  while (Entry != nullptr) {
    Builder.SetInsertPoint(Entry);

    LabeledBasicBlocks.clear();

    // TODO: rename this type
    PTCInstructionListPtr InstructionList(new PTCInstructionList);
    size_t ConsumedSize = 0;

    assert(CodeEnd > CodePointer);

    ConsumedSize = ptc.translate(VirtualAddress,
                                 InstructionList.get());
    uint64_t NextPC = VirtualAddress + ConsumedSize;

    dumpTranslation(std::cerr, InstructionList.get());

    Variables.newFunction(Delimiter, InstructionList.get());
    unsigned j = 0;
    MDNode* MDOriginalInstr = nullptr;
    bool StopTranslation = false;

    // Handle the first PTC_INSTRUCTION_op_debug_insn_start
    {
      PTCInstruction *Instruction = &InstructionList->instructions[j];
      auto Result = Translator.newInstruction(Instruction, true);
      std::tie(StopTranslation, MDOriginalInstr) = Result;
      j++;
    }

    for (; j < InstructionList->instruction_count && !StopTranslation; j++) {
      PTCInstruction Instruction = InstructionList->instructions[j];
      PTCOpcode Opcode = Instruction.opc;

      Blocks.clear();
      Blocks.push_back(Builder.GetInsertBlock());

      switch(Opcode) {
      case PTC_INSTRUCTION_op_discard:
        // Instructions we don't even consider
        break;
      case PTC_INSTRUCTION_op_debug_insn_start:
        {
          std::tie(StopTranslation,
                   MDOriginalInstr) = Translator.newInstruction(&Instruction,
                                                                false);
          break;
        }
      case PTC_INSTRUCTION_op_call:
        Translator.translateCall(&Instruction);

        // Sometimes libtinycode terminates a basic block with a call, in this
        // case force a fallthrough
        // TODO: investigate why this happens
        if (j == InstructionList->instruction_count - 1)
          Builder.CreateBr(JumpTargets.getBlockAt(NextPC));

        break;
      default:
        Translator.translate(&Instruction);
      }

      // Create a new metadata referencing the PTC instruction we have just
      // translated
      std::stringstream PTCStringStream;
      dumpInstruction(PTCStringStream, InstructionList.get(), j);
      std::string PTCString = PTCStringStream.str() + "\n";
      MDString *MDPTCString = MDString::get(Context, PTCString);
      MDNode* MDPTCInstr = MDNode::getDistinct(Context, MDPTCString);

      // Set metadata for all the new instructions
      for (BasicBlock *Block : Blocks) {
        BasicBlock::iterator I = Block->end();
        while (I != Block->begin() && !(--I)->hasMetadata()) {
          I->setMetadata(OriginalInstrMDKind, MDOriginalInstr);
          I->setMetadata(PTCInstrMDKind, MDPTCInstr);
        }
      }

    } // End loop over instructions

    Translator.closeLastInstruction(NextPC);

    // Before looking for writes to the PC, give a shot of SROA
    legacy::PassManager PM;
    PM.add(createSROAPass());
    PM.add(Translator.createTranslateDirectBranchesPass());
    PM.run(*TheModule);

    // Obtain a new program counter to translate
    uint64_t NewPC = 0;
    std::tie(NewPC, Entry) = JumpTargets.peekJumpTarget();
    VirtualAddress = NewPC;
    CodePointer = Code.data() + (NewPC - LoadAddress);
  } // End translations loop


  // Handle some specific QEMU functions as no-ops or abort
  auto NoOpFunctionNames = make_array<const char *>("qemu_log_mask",
                                                    "fprintf",
                                                    "cpu_dump_state",
                                                    "mmap_lock",
                                                    "mmap_unlock",
                                                    "pthread_cond_broadcast",
                                                    "pthread_mutex_unlock",
                                                    "pthread_mutex_lock",
                                                    "pthread_cond_wait",
                                                    "pthread_cond_signal",
                                                    "cpu_exit",
                                                    "start_exclusive",
                                                    "process_pending_signals",
                                                    "end_exclusive");
  auto AbortFunctionNames = make_array<const char *>("cpu_restore_state",
                                                     "gdb_handlesig",
                                                     "queue_signal",
                                                     "cpu_mips_exec",
                                                     // syscall.c
                                                     "print_syscall",
                                                     "print_syscall_ret",
                                                     // ARM cpu_loop
                                                     "EmulateAll",
                                                     "cpu_abort",
                                                     "do_arm_semihosting");

  // EmulateAll: requires access to the opcode
  // do_arm_semihosting: we don't care about semihosting

  // From syscall.c
  new GlobalVariable(*TheModule,
                     Type::getInt32Ty(Context),
                     false,
                     GlobalValue::CommonLinkage,
                     ConstantInt::get(Type::getInt32Ty(Context), 0),
                     StringRef("do_strace"));

  for (auto Name : NoOpFunctionNames)
    replaceFunctionWithRet(HelpersModule->getFunction(Name), 0);

  for (auto Name : AbortFunctionNames) {
    Function *TheFunction = HelpersModule->getFunction(Name);
    if (TheFunction != nullptr) {
      assert(HelpersModule->getFunction("abort") != nullptr);
      BasicBlock *NewBody = replaceFunction(TheFunction);
      CallInst::Create(HelpersModule->getFunction("abort"), { }, NewBody);
      new UnreachableInst(Context, NewBody);
    }
  }

  replaceFunctionWithRet(HelpersModule->getFunction("page_check_range"), 1);
  replaceFunctionWithRet(HelpersModule->getFunction("page_get_flags"),
                         0xffffffff);

  Linker TheLinker(TheModule.get());
  bool Result = TheLinker.linkInModule(HelpersModule.get(),
                                       Linker::LinkOnlyNeeded);
  assert(!Result && "Linking failed");

  legacy::PassManager PM;
  PM.add(createSROAPass());
  PM.add(Variables.createCorrectCPUStateUsagePass());
  PM.add(createDeadCodeEliminationPass());
  PM.run(*TheModule);

  // TODO: we have around all the usages of the PC, shall we drop them?
  Delimiter->eraseFromParent();

  JumpTargets.translateIndirectJumps();

  Translator.removeNewPCMarkers();

  Debug->generateDebugInfo();

}

void CodeGenerator::serialize() {
  // Ask the debug handler if it already has a good copy of the IR, if not dump
  // it
  if (!Debug->copySource()) {
    std::ofstream Output(OutputPath);
    Debug->print(Output, false);
  }
}
