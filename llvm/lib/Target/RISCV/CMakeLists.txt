add_llvm_component_group(RISCV)

set(LLVM_TARGET_DEFINITIONS RISCV.td)

tablegen(LLVM RISCVGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM RISCVGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM RISCVGenCompressInstEmitter.inc -gen-compress-inst-emitter)
tablegen(LLVM RISCVGenMacroFusion.inc -gen-macro-fusion-pred)
tablegen(LLVM RISCVGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM RISCVGenDisassemblerTables.inc -gen-disassembler
              --specialize-decoders-per-bitwidth)
tablegen(LLVM RISCVGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM RISCVGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM RISCVGenMCPseudoLowering.inc -gen-pseudo-lowering)
tablegen(LLVM RISCVGenRegisterBank.inc -gen-register-bank)
tablegen(LLVM RISCVGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM RISCVGenSearchableTables.inc -gen-searchable-tables)
tablegen(LLVM RISCVGenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM RISCVGenExegesis.inc -gen-exegesis)
tablegen(LLVM RISCVGenSDNodeInfo.inc -gen-sd-node-info)

set(LLVM_TARGET_DEFINITIONS RISCVGISel.td)
tablegen(LLVM RISCVGenGlobalISel.inc -gen-global-isel)
tablegen(LLVM RISCVGenO0PreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="RISCVO0PreLegalizerCombiner")
tablegen(LLVM RISCVGenPreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="RISCVPreLegalizerCombiner")
tablegen(LLVM RISCVGenPostLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="RISCVPostLegalizerCombiner")

add_public_tablegen_target(RISCVCommonTableGen)

add_llvm_target(RISCVCodeGen
  RISCVAsmPrinter.cpp
  RISCVCallingConv.cpp
  RISCVCodeGenPrepare.cpp
  RISCVConstantPoolValue.cpp
  RISCVDeadRegisterDefinitions.cpp
  RISCVExpandAtomicPseudoInsts.cpp
  RISCVExpandPseudoInsts.cpp
  RISCVFoldMemOffset.cpp
  RISCVFrameLowering.cpp
  RISCVGatherScatterLowering.cpp
  RISCVIndirectBranchTracking.cpp
  RISCVInsertReadWriteCSR.cpp
  RISCVInsertVSETVLI.cpp
  RISCVInsertWriteVXRM.cpp
  RISCVInstrInfo.cpp
  RISCVInterleavedAccess.cpp
  RISCVISelDAGToDAG.cpp
  RISCVISelLowering.cpp
  RISCVLandingPadSetup.cpp
  RISCVLateBranchOpt.cpp
  RISCVLoadStoreOptimizer.cpp
  RISCVMachineFunctionInfo.cpp
  RISCVMakeCompressible.cpp
  RISCVMergeBaseOffset.cpp
  RISCVMoveMerger.cpp
  RISCVOptWInstrs.cpp
  RISCVPostRAExpandPseudoInsts.cpp
  RISCVPushPopOptimizer.cpp
  RISCVRedundantCopyElimination.cpp
  RISCVRegisterInfo.cpp
  RISCVRVCRegAllocHints.cpp
  RISCVASPRegAlloc.cpp
  RISCVASPSplit.cpp
  RISCVSelectionDAGInfo.cpp
  RISCVSubtarget.cpp
  RISCVTargetMachine.cpp
  RISCVTargetObjectFile.cpp
  RISCVTargetTransformInfo.cpp
  RISCVVectorMaskDAGMutation.cpp
  RISCVVectorPeephole.cpp
  RISCVVLOptimizer.cpp
  RISCVVMV0Elimination.cpp
  RISCVZacasABIFix.cpp
  GISel/RISCVCallLowering.cpp
  GISel/RISCVInstructionSelector.cpp
  GISel/RISCVLegalizerInfo.cpp
  GISel/RISCVPostLegalizerCombiner.cpp
  GISel/RISCVO0PreLegalizerCombiner.cpp
  GISel/RISCVPreLegalizerCombiner.cpp
  GISel/RISCVRegisterBankInfo.cpp

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  GlobalISel
  IPO
  MC
  Passes
  RISCVDesc
  RISCVInfo
  Scalar
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils
  Vectorize

  ADD_TO_COMPONENT
  RISCV
  )

# The single-phase ASP allocator (RISCVASPRegAlloc.cpp) reuses the CodeGen
# register-allocation driver headers (RegAllocBase.h, AllocationOrder.h), which
# live in lib/CodeGen rather than the public include tree.
target_include_directories(LLVMRISCVCodeGen PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../CodeGen)

# Link Clingo into LLVMRISCVCodeGen when the clingo subproject is enabled, so
# the phase-1 ASP GPRC allocator (RISCVRVCRegAllocHints.cpp) can call libclingo
# in-process. When clingo is disabled the .cpp still compiles but the pass is a
# runtime no-op, keeping the build working without clingo.
if("clingo" IN_LIST LLVM_ENABLE_PROJECTS)
  target_compile_definitions(LLVMRISCVCodeGen PRIVATE LLVM_PBQP_HAVE_CLINGO)
  target_include_directories(LLVMRISCVCodeGen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../clingo/libclingo
  )
  target_link_libraries(LLVMRISCVCodeGen PRIVATE
    ${CMAKE_BINARY_DIR}/lib/libclingo.a
    ${CMAKE_BINARY_DIR}/lib/libgringo.a
    ${CMAKE_BINARY_DIR}/lib/libreify.a
    ${CMAKE_BINARY_DIR}/lib/libclasp.dylib
    ${CMAKE_BINARY_DIR}/lib/libpotassco.dylib
  )
endif()

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(MCA)
add_subdirectory(TargetInfo)
