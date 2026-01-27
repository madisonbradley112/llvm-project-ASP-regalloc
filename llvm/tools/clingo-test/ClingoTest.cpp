#include "clingo.h"
#include "llvm/Support/raw_ostream.h"

int main(int argc, char *argv[]) {
  llvm::outs() << "=== Clingo C API Test ===\n";

  // Create a clingo control object
  clingo_control_t *ctl = nullptr;
  bool success = clingo_control_new(nullptr, 0, nullptr, nullptr, 0, &ctl);
  
  if (!success) {
    llvm::errs() << "Failed to create clingo control object\n";
    return 1;
  }

  llvm::outs() << "✓ Created clingo control object\n";

  // Add a simple logic program
  const char *program = "a. b.";

  if (!clingo_control_add(ctl, "base", nullptr, 0, program)) {
    llvm::errs() << "Failed to add program\n";
    clingo_control_free(ctl);
    return 1;
  }

  llvm::outs() << "✓ Added logic program\n";

  // Ground the program
  clingo_part_t parts[] = {{"base", nullptr, 0}};
  if (!clingo_control_ground(ctl, parts, 1, nullptr, nullptr)) {
    llvm::errs() << "Failed to ground program\n";
    clingo_control_free(ctl);
    return 1;
  }

  llvm::outs() << "✓ Grounded program\n";

  // Clean up
  clingo_control_free(ctl);

  llvm::outs() << "\n=== Test Complete ===\n";
  llvm::outs() << "Clingo C API integration with LLVM successful!\n";

  return 0;
}
