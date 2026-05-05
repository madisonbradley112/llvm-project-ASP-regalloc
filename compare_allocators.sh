#!/bin/bash

# compare_allocators.sh - Automate PBQP vs ASP PBQP register allocation comparison
# Usage: ./compare_allocators.sh

set -e

WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLC="${WORKSPACE_ROOT}/build/bin/llc"
CLANG="${WORKSPACE_ROOT}/build/bin/clang"
TEST_C_FILE="${WORKSPACE_ROOT}/tests/high_register_pressure.c"
TEST_LL_FILE="${WORKSPACE_ROOT}/tests/high_register_pressure.ll"

# Output files (relative to workspace root)
DEFAULT_JSON="${WORKSPACE_ROOT}/pbqp_default_run.json"
DEFAULT_TXT="${WORKSPACE_ROOT}/pbqp_default_run.txt"
ASP_JSON="${WORKSPACE_ROOT}/pbqp_asp_run.json"
ASP_TXT="${WORKSPACE_ROOT}/pbqp_asp_run.txt"

# Temporary assembly output files
DEFAULT_ASM="/tmp/high_default.s"
ASP_ASM="/tmp/high_asp.s"

echo "================================================"
echo "PBQP Register Allocation Comparison"
echo "================================================"
echo ""
echo "Workspace root: ${WORKSPACE_ROOT}"
echo "Clang compiler: ${CLANG}"
echo "LLVM compiler: ${LLC}"
echo "Test input (C): ${TEST_C_FILE}"
echo "Test input (LL): ${TEST_LL_FILE}"
echo ""

# Validate prerequisites
if [ ! -f "${LLC}" ]; then
    echo "ERROR: llc binary not found at ${LLC}"
    echo "Please build the LLVM project first."
    exit 1
fi

if [ ! -f "${CLANG}" ]; then
    echo "ERROR: clang binary not found at ${CLANG}"
    echo "Please build the LLVM project first."
    exit 1
fi

if [ ! -f "${TEST_C_FILE}" ]; then
    echo "ERROR: Test file not found at ${TEST_C_FILE}"
    exit 1
fi

# Compile C to LLVM IR
echo "[0/3] Compiling C to LLVM IR..."
"${CLANG}" -emit-llvm -S -O0 -target aarch64-linux-gnu "${TEST_C_FILE}" -o "${TEST_LL_FILE}"
if [ ! -f "${TEST_LL_FILE}" ]; then
    echo "ERROR: Failed to compile C to LLVM IR"
    exit 1
fi
echo "✓ Generated LLVM IR: ${TEST_LL_FILE}"
echo ""

TEST_FILE="${TEST_LL_FILE}"

# Run default PBQP allocator
echo "[1/3] Running default PBQP allocator..."
echo "Command: ${LLC} ${TEST_FILE} -o ${DEFAULT_ASM} -regalloc=pbqp -pbqp-export-results=${DEFAULT_JSON} -pbqp-export-results-text=${DEFAULT_TXT}"
"${LLC}" "${TEST_FILE}" -o "${DEFAULT_ASM}" \
    -regalloc=pbqp \
    -pbqp-export-results="${DEFAULT_JSON}" \
    -pbqp-export-results-text="${DEFAULT_TXT}"

if [ ! -f "${DEFAULT_JSON}" ]; then
    echo "ERROR: Default PBQP allocator did not produce ${DEFAULT_JSON}"
    exit 1
fi

echo "✓ Default PBQP export created: $(wc -c < "${DEFAULT_JSON}") bytes"
echo "  JSON: ${DEFAULT_JSON}"
echo "  Text: ${DEFAULT_TXT}"
echo ""

# Run ASP PBQP allocator
echo "[2/3] Running ASP PBQP allocator..."
echo "Command: ${LLC} ${TEST_FILE} -o ${ASP_ASM} -regalloc=pbqp-asp -pbqp-export-results=${ASP_JSON} -pbqp-export-results-text=${ASP_TXT}"
"${LLC}" "${TEST_FILE}" -o "${ASP_ASM}" \
    -regalloc=pbqp-asp \
    -pbqp-export-results="${ASP_JSON}" \
    -pbqp-export-results-text="${ASP_TXT}"

if [ ! -f "${ASP_JSON}" ]; then
    echo "ERROR: ASP PBQP allocator did not produce ${ASP_JSON}"
    echo "Note: Check if the ASP solver is properly integrated and compiled."
    exit 1
fi

echo "✓ ASP PBQP export created: $(wc -c < "${ASP_JSON}") bytes"
echo "  JSON: ${ASP_JSON}"
echo "  Text: ${ASP_TXT}"
echo ""

# Summary
echo "================================================"
echo "Comparison Ready"
echo "================================================"
echo ""
echo "Export files ready for comparison:"
echo "  Default PBQP:"
echo "    - ${DEFAULT_JSON}"
echo "    - ${DEFAULT_TXT}"
echo ""
echo "  ASP PBQP:"
echo "    - ${ASP_JSON}"
echo "    - ${ASP_TXT}"
echo ""
echo "Next: Load these files into the PBQPComparison"
echo "      framework to generate detailed comparison report."
echo ""
