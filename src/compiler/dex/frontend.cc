/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <llvm/Support/Threading.h>

#include "compiler/driver/compiler_driver.h"
#include "compiler_internals.h"
#include "dataflow_iterator.h"
#if defined(ART_USE_PORTABLE_COMPILER)
#include "compiler/llvm/llvm_compilation_unit.h"
#endif
#include "leb128.h"
#include "mirror/object.h"
#include "runtime.h"
#include "quick/codegen_util.h"
#include "portable/mir_to_gbc.h"
#include "quick/mir_to_lir.h"

namespace {
#if !defined(ART_USE_PORTABLE_COMPILER)
  pthread_once_t llvm_multi_init = PTHREAD_ONCE_INIT;
#endif
  void InitializeLLVMForQuick() {
    ::llvm::llvm_start_multithreaded();
  }
}

namespace art {
namespace llvm {
::llvm::Module* makeLLVMModuleContents(::llvm::Module* module);
}

LLVMInfo::LLVMInfo() {
#if !defined(ART_USE_PORTABLE_COMPILER)
  pthread_once(&llvm_multi_init, InitializeLLVMForQuick);
#endif
  // Create context, module, intrinsic helper & ir builder
  llvm_context_.reset(new ::llvm::LLVMContext());
  llvm_module_ = new ::llvm::Module("art", *llvm_context_);
  ::llvm::StructType::create(*llvm_context_, "JavaObject");
  art::llvm::makeLLVMModuleContents(llvm_module_);
  intrinsic_helper_.reset( new art::llvm::IntrinsicHelper(*llvm_context_, *llvm_module_));
  ir_builder_.reset(new art::llvm::IRBuilder(*llvm_context_, *llvm_module_, *intrinsic_helper_));
}

LLVMInfo::~LLVMInfo() {
}

extern "C" void ArtInitQuickCompilerContext(art::CompilerDriver& compiler) {
  CHECK(compiler.GetCompilerContext() == NULL);
  LLVMInfo* llvm_info = new LLVMInfo();
  compiler.SetCompilerContext(llvm_info);
}

extern "C" void ArtUnInitQuickCompilerContext(art::CompilerDriver& compiler) {
  delete reinterpret_cast<LLVMInfo*>(compiler.GetCompilerContext());
  compiler.SetCompilerContext(NULL);
}

/* Default optimizer/debug setting for the compiler. */
static uint32_t kCompilerOptimizerDisableFlags = 0 | // Disable specific optimizations
  (1 << kLoadStoreElimination) |
  //(1 << kLoadHoisting) |
  //(1 << kSuppressLoads) |
  //(1 << kNullCheckElimination) |
  //(1 << kPromoteRegs) |
  //(1 << kTrackLiveTemps) |
  //(1 << kSafeOptimizations) |
  //(1 << kBBOpt) |
  //(1 << kMatch) |
  //(1 << kPromoteCompilerTemps) |
  0;

static uint32_t kCompilerDebugFlags = 0 |     // Enable debug/testing modes
  //(1 << kDebugDisplayMissingTargets) |
  //(1 << kDebugVerbose) |
  //(1 << kDebugDumpCFG) |
  //(1 << kDebugSlowFieldPath) |
  //(1 << kDebugSlowInvokePath) |
  //(1 << kDebugSlowStringPath) |
  //(1 << kDebugSlowestFieldPath) |
  //(1 << kDebugSlowestStringPath) |
  //(1 << kDebugExerciseResolveMethod) |
  //(1 << kDebugVerifyDataflow) |
  //(1 << kDebugShowMemoryUsage) |
  //(1 << kDebugShowNops) |
  //(1 << kDebugCountOpcodes) |
  //(1 << kDebugDumpCheckStats) |
  //(1 << kDebugDumpBitcodeFile) |
  //(1 << kDebugVerifyBitcode) |
  0;

static CompiledMethod* CompileMethod(CompilerDriver& compiler,
                                     const CompilerBackend compiler_backend,
                                     const DexFile::CodeItem* code_item,
                                     uint32_t access_flags, InvokeType invoke_type,
                                     uint32_t class_def_idx, uint32_t method_idx,
                                     jobject class_loader, const DexFile& dex_file
#if defined(ART_USE_PORTABLE_COMPILER)
                                     , llvm::LlvmCompilationUnit* llvm_compilation_unit
#endif
)
{
  VLOG(compiler) << "Compiling " << PrettyMethod(method_idx, dex_file) << "...";

  // FIXME: now we detect this in MIRGraph.
  SpecialCaseHandler special_case = kNoHandler;

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  UniquePtr<CompilationUnit> cu(new CompilationUnit);

  if (!HeapInit(cu.get())) {
    LOG(FATAL) << "Failed to initialize compiler heap";
  }

  cu->compiler_driver = &compiler;
  cu->class_linker = class_linker;
  cu->instruction_set = compiler.GetInstructionSet();
  DCHECK((cu->instruction_set == kThumb2) ||
         (cu->instruction_set == kX86) ||
         (cu->instruction_set == kMips));

  cu->gen_bitcode = (compiler_backend == kPortable);

#if defined(ART_USE_PORTABLE_COMPILER)
  cu->llvm_compilation_unit = llvm_compilation_unit;
  cu->llvm_info = llvm_compilation_unit->GetQuickContext();
  cu->symbol = llvm_compilation_unit->GetDexCompilationUnit()->GetSymbol();
#endif
  /* Adjust this value accordingly once inlining is performed */
  cu->num_dalvik_registers = code_item->registers_size_;
  // TODO: set this from command line
  cu->compiler_flip_match = false;
  bool use_match = !cu->compiler_method_match.empty();
  bool match = use_match && (cu->compiler_flip_match ^
      (PrettyMethod(method_idx, dex_file).find(cu->compiler_method_match) !=
       std::string::npos));
  if (!use_match || match) {
    cu->disable_opt = kCompilerOptimizerDisableFlags;
    cu->enable_debug = kCompilerDebugFlags;
    cu->verbose = VLOG_IS_ON(compiler) ||
        (cu->enable_debug & (1 << kDebugVerbose));
  }

  // If debug build, always verify bitcode.
  if (kIsDebugBuild && cu->gen_bitcode) {
    cu->enable_debug |= (1 << kDebugVerifyBitcode);
  }

  if (cu->instruction_set == kMips) {
    // Disable some optimizations for mips for now
    cu->disable_opt |= (
        (1 << kLoadStoreElimination) |
        (1 << kLoadHoisting) |
        (1 << kSuppressLoads) |
        (1 << kNullCheckElimination) |
        (1 << kPromoteRegs) |
        (1 << kTrackLiveTemps) |
        (1 << kSafeOptimizations) |
        (1 << kBBOpt) |
        (1 << kMatch) |
        (1 << kPromoteCompilerTemps));
  }

  /* Assume leaf */
  cu->attributes = METHOD_IS_LEAF;

  cu->mir_graph.reset(new MIRGraph(cu.get()));

  /* Gathering opcode stats? */
  if (kCompilerDebugFlags & (1 << kDebugCountOpcodes)) {
    cu->mir_graph->EnableOpcodeCounting();
  }

  /* Build the raw MIR graph */
  cu->mir_graph->InlineMethod(code_item, access_flags, invoke_type, class_def_idx, method_idx,
                              class_loader, dex_file);

  /* Do a code layout pass */
  cu->mir_graph->CodeLayout();

  if (cu->enable_debug & (1 << kDebugVerifyDataflow)) {
    cu->mir_graph->VerifyDataflow();
  }

  /* Perform SSA transformation for the whole method */
  cu->mir_graph->SSATransformation();

  /* Do constant propagation */
  cu->mir_graph->PropagateConstants();

  /* Count uses */
  cu->mir_graph->MethodUseCount();

  /* Perform null check elimination */
  cu->mir_graph->NullCheckElimination();

  /* Combine basic blocks where possible */
  cu->mir_graph->BasicBlockCombine();

  /* Do some basic block optimizations */
  cu->mir_graph->BasicBlockOptimization();

  if (cu->enable_debug & (1 << kDebugDumpCheckStats)) {
    cu->mir_graph->DumpCheckStats();
  }

  /* Set up regLocation[] array to describe values - one for each ssa_name. */
  cu->mir_graph->BuildRegLocations();

#if defined(ART_USE_PORTABLE_COMPILER)
  /* Go the LLVM path? */
  if (cu->gen_bitcode) {
    // MIR->Bitcode
    MethodMIR2Bitcode(cu.get());
    if (compiler_backend == kPortable) {
      // all done
      ArenaReset(cu.get());
      return NULL;
    }
  } else
#endif
  {
    switch (compiler.GetInstructionSet()) {
      case kThumb2:
        InitArmCodegen(cu.get()); break;
      case kMips:
        InitMipsCodegen(cu.get()); break;
      case kX86:
        InitX86Codegen(cu.get()); break;
      default:
        LOG(FATAL) << "Unexpected instruction set: " << compiler.GetInstructionSet();
    }

// ** MOVE ALL OF THIS TO Codegen.materialize()

  /* Initialize the switch_tables list */                       // TO CODEGEN
  CompilerInitGrowableList(cu.get(), &cu->switch_tables, 4,
                      kListSwitchTables);

  /* Intialize the fill_array_data list */                     // TO CODEGEN
  CompilerInitGrowableList(cu.get(), &cu->fill_array_data, 4,
                      kListFillArrayData);

  /* Intialize the throw_launchpads list, estimate size based on insns_size */ // TO CODEGEN
  CompilerInitGrowableList(cu.get(), &cu->throw_launchpads, code_item->insns_size_in_code_units_,
                      kListThrowLaunchPads);

  /* Intialize the instrinsic_launchpads list */  // TO_CODEGEN
  CompilerInitGrowableList(cu.get(), &cu->intrinsic_launchpads, 4,
                      kListMisc);


  /* Intialize the suspend_launchpads list */ // TO_CODEGEN
  CompilerInitGrowableList(cu.get(), &cu->suspend_launchpads, 2048,
                      kListSuspendLaunchPads);

    // TODO: Push these to codegen
    cu.get()->cg->CompilerInitializeRegAlloc(cu.get());  // Needs to happen after SSA naming

    /* Allocate Registers using simple local allocation scheme */
    cu.get()->cg->SimpleRegAlloc(cu.get());

    if (special_case != kNoHandler) {
      /*
       * Custom codegen for special cases.  If for any reason the
       * special codegen doesn't succeed, cu->first_lir_insn will
       * set to NULL;
       */
      SpecialMIR2LIR(cu.get(), special_case);
    }

    /* Convert MIR to LIR, etc. */
    if (cu->first_lir_insn == NULL) {
      MethodMIR2LIR(cu.get());
    }
  }

  /* Method is not empty */
  if (cu->first_lir_insn) {

    // mark the targets of switch statement case labels
    ProcessSwitchTables(cu.get());

    /* Convert LIR into machine code. */
    AssembleLIR(cu.get());

    if (cu->verbose) {
      CodegenDump(cu.get());
    }

  }

  if (kCompilerDebugFlags & (1 << kDebugCountOpcodes)) {
    cu->mir_graph->ShowOpcodeStats();
  }

  // Combine vmap tables - core regs, then fp regs - into vmap_table
  std::vector<uint16_t> vmap_table;
  // Core regs may have been inserted out of order - sort first
  std::sort(cu->core_vmap_table.begin(), cu->core_vmap_table.end());
  for (size_t i = 0 ; i < cu->core_vmap_table.size(); i++) {
    // Copy, stripping out the phys register sort key
    vmap_table.push_back(~(-1 << VREG_NUM_WIDTH) & cu->core_vmap_table[i]);
  }
  // If we have a frame, push a marker to take place of lr
  if (cu->frame_size > 0) {
    vmap_table.push_back(INVALID_VREG);
  } else {
    DCHECK_EQ(__builtin_popcount(cu->core_spill_mask), 0);
    DCHECK_EQ(__builtin_popcount(cu->fp_spill_mask), 0);
  }
  // Combine vmap tables - core regs, then fp regs. fp regs already sorted
  for (uint32_t i = 0; i < cu->fp_vmap_table.size(); i++) {
    vmap_table.push_back(cu->fp_vmap_table[i]);
  }
  CompiledMethod* result =
      new CompiledMethod(cu->instruction_set, cu->code_buffer,
                         cu->frame_size, cu->core_spill_mask, cu->fp_spill_mask,
                         cu->combined_mapping_table, vmap_table, cu->native_gc_map);

  VLOG(compiler) << "Compiled " << PrettyMethod(method_idx, dex_file)
     << " (" << (cu->code_buffer.size() * sizeof(cu->code_buffer[0]))
     << " bytes)";

#ifdef WITH_MEMSTATS
  if (cu->enable_debug & (1 << kDebugShowMemoryUsage)) {
    DumpMemStats(cu.get());
  }
#endif

  ArenaReset(cu.get());

  return result;
}

CompiledMethod* CompileOneMethod(CompilerDriver& compiler,
                                 const CompilerBackend backend,
                                 const DexFile::CodeItem* code_item,
                                 uint32_t access_flags,
                                 InvokeType invoke_type,
                                 uint32_t class_def_idx,
                                 uint32_t method_idx,
                                 jobject class_loader,
                                 const DexFile& dex_file,
                                 llvm::LlvmCompilationUnit* llvm_compilation_unit)
{
  return CompileMethod(compiler, backend, code_item, access_flags, invoke_type, class_def_idx,
                       method_idx, class_loader, dex_file
#if defined(ART_USE_PORTABLE_COMPILER)
                       , llvm_compilation_unit
#endif
                       );
}

}  // namespace art

extern "C" art::CompiledMethod*
    ArtQuickCompileMethod(art::CompilerDriver& compiler,
                          const art::DexFile::CodeItem* code_item,
                          uint32_t access_flags, art::InvokeType invoke_type,
                          uint32_t class_def_idx, uint32_t method_idx, jobject class_loader,
                          const art::DexFile& dex_file)
{
  // TODO: check method fingerprint here to determine appropriate backend type.  Until then, use build default
  art::CompilerBackend backend = compiler.GetCompilerBackend();
  return art::CompileOneMethod(compiler, backend, code_item, access_flags, invoke_type,
                               class_def_idx, method_idx, class_loader, dex_file,
                               NULL /* use thread llvm_info */);
}
