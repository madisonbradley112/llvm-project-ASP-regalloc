# Clingo Integration Guide for LLVM

Since clingo has been added as a git submodule to the LLVM monorepo, it can now be built as part of the LLVM build process with all its bundled dependencies. This guide explains how to configure and use clingo in LLVM components.

## Project Structure

Clingo is located at the root of the monorepo and includes bundled dependencies:
```
llvm-project-ASP-regalloc/
├── llvm/
├── clang/
├── mlir/
├── clingo/              # <- Clingo submodule with bundled dependencies
│   ├── libclingo/       # Clingo C/C++ API
│   ├── libgringo/       # Gringo (logic programming grounder)
│   ├── libpotassco/     # Potassco libraries (bundled in third_party)
│   ├── clasp/           # Clasp SAT solver (git submodule within clingo)
│   ├── libreify/        # Reifier library
│   ├── third_party/     # Contains potassco and other dependencies
│   └── ...
├── ...other projects...
```

## Understanding Clingo's Dependencies

Clingo includes bundled versions of all its dependencies:
- **libgringo**: The logic program grounder
- **libpotassco**: Potassco utility library
- **clasp**: The Clasp SAT solver (in `clingo/clasp/` as a git submodule)
- **libreify**: Program reification utilities

These are all built automatically when you build clingo with the default `CLINGO_USE_LIB=OFF` setting.

## Building LLVM with Clingo

### Step 1: Initialize Clingo's Submodules

Clingo has its own git submodules (particularly clasp). Initialize them:

```bash
cd llvm-project-ASP-regalloc/clingo
git submodule update --init --recursive
```

This ensures clasp and other submodules are available.

### Step 2: Configure LLVM Build with Clingo

Create or navigate to your LLVM build directory and configure with clingo as an enabled project:

```bash
cd llvm-project-ASP-regalloc
mkdir -p build
cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clingo;clang;mlir" \
  -GNinja
```

The following options are automatically configured by the LLVM build system (in `llvm/CMakeLists.txt`) via FORCE CACHE variables and will be applied during configuration:
- `-DCLINGO_BUILD_STATIC=ON` - Build clingo as static library
- `-DCLASP_BUILD_STATIC=ON` - Build clasp as static library
- `-DCLASP_BUILD_APP=OFF` - Don't build clasp applications
- `-DCLINGO_BUILD_APPS=OFF` - Don't build clingo applications
- `-DCLINGO_BUILD_TESTS=OFF` - Don't build clingo tests
- `-DCLINGO_BUILD_WITH_PYTHON=OFF` - Disable Python bindings
- `-DCLINGO_BUILD_WITH_LUA=OFF` - Disable Lua bindings

These are automatically set, so you only need to specify the basic configuration above.

### Step 3: Build LLVM

```bash
cd llvm-project-ASP-regalloc/build
ninja
```

Or with make (if not using Ninja):

```bash
make -j$(nproc)
```

This will:
1. Build all clingo dependencies (clasp, libgringo, libpotassco, libreify)
2. Build the clingo libraries as static archives
3. Build LLVM and enabled projects with clingo available

## Using Clingo in LLVM Components

Once clingo is built as part of LLVM, you can use it in any LLVM component.

### In Component CMakeLists.txt

In any LLVM component's `CMakeLists.txt` file, link against clingo by explicitly specifying all required libraries:

```cmake
add_llvm_tool(your-tool
  main.cpp
  # ... source files ...
)

target_include_directories(your-tool
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../projects/clingo/clingo/libclingo
)

target_link_libraries(your-tool
  PRIVATE
  ${CMAKE_BINARY_DIR}/lib/libclingo.a
  ${CMAKE_BINARY_DIR}/lib/libgringo.a
  ${CMAKE_BINARY_DIR}/lib/libreify.a
  ${CMAKE_BINARY_DIR}/lib/libclasp.dylib
  ${CMAKE_BINARY_DIR}/lib/libpotassco.dylib
  LLVMSupport
)
```

**Important Notes:**
- When clingo is built as part of LLVM, all libraries are in `${CMAKE_BINARY_DIR}/lib/`
- You must explicitly link against all dependencies in the correct order:
  - `libclingo.a` - The clingo C/C++ API
  - `libgringo.a` - Required by libclingo for logic program grounding
  - `libreify.a` - Required by libgringo for reification
  - `libclasp.dylib` - The SAT solver (dynamic library)
  - `libpotassco.dylib` - Potassco utilities (dynamic library)
- The include path points to the clingo source tree headers in the projects directory
- Adjust the relative path `../../projects/clingo/...` based on your tool's location

### In C++ Code

Include clingo headers and use the C API:

```cpp
#include "clingo.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

// Example: Create a clingo control object
int setupClingo() {
  clingo_control_t *ctl = NULL;
  
  // Create control object with 0 threads
  clingo_error_code_t ret = clingo_control_new(
    nullptr,           // logger callback
    nullptr,           // logger context
    nullptr,           // scripts (not using scripts)
    nullptr,           // script context
    0,                 // threads (0 = auto)
    &ctl               // output control object
  );
  
  if (ret != clingo_error_success) {
    llvm::errs() << "Failed to create clingo control\n";
    return 1;
  }
  
  // ... use clingo API ...
  
  // Clean up
  clingo_control_free(ctl);
  return 0;
}
```

## CMake Module: Findclingo.cmake

A CMake module at `cmake/Modules/Findclingo.cmake` provides the `clingo::clingo` target for linking. When clingo is built as part of LLVM (via `LLVM_ENABLE_PROJECTS`), this target is automatically available.

## Advanced Configuration

### Clingo Build Options When Building with LLVM

You can customize the clingo build when configuring LLVM:

```bash
cmake .. \
  -DLLVM_ENABLE_PROJECTS="clingo;clang" \
  -DCLINGO_BUILD_WITH_PYTHON=OFF \   # Disable Python bindings
  -DCLINGO_BUILD_WITH_LUA=OFF \      # Disable Lua bindings
  -DCLINGO_BUILD_APPS=OFF \          # Don't build gringo, clasp, etc. apps
  -DCLINGO_BUILD_TESTS=OFF \         # Don't build tests
  -DCLINGO_BUILD_EXAMPLES=OFF        # Don't build examples
```

### Understanding CLINGO_USE_LIB

The `CLINGO_USE_LIB` variable controls whether clingo uses bundled or external dependencies:

- `CLINGO_USE_LIB=OFF` (default): Uses bundled clasp, libgringo, libpotassco, libreify
- `CLINGO_USE_LIB=ON`: Links against externally-provided libraries (advanced usage)

For the standard bundled approach, keep the default `CLINGO_USE_LIB=OFF`.

## Verifying Integration

### 1. Check CMake Configuration Output

When configuring LLVM with clingo enabled, you should see:

```
-- clingo project is enabled
-- Configuring done
```

### 2. Check Build Progress

During the build, clingo and its dependencies should build:

```bash
cmake --build . --config Release -j$(nproc)
# You should see output for:
# - Building clasp
# - Building libpotassco
# - Building libgringo
# - Building libreify
# - Building libclingo
```

### 3. Verify Clingo Libraries Were Built

After a successful build, check:

```bash
ls -la build/lib/libclingo*
# Should show libclingo.a or libclingo.dylib (depending on CLINGO_BUILD_SHARED)

ls -la build/lib/libclasp*
# Should show clasp libraries
```

### 3. Test with a Simple Tool (clingo-test)

A test tool has been created at `llvm/tools/clingo-test/` to verify the integration works. It demonstrates proper linking and use of the clingo C API.

**`llvm/tools/clingo-test/CMakeLists.txt`:**

```cmake
add_llvm_tool(clingo-test
  ClingoTest.cpp
)

target_include_directories(clingo-test
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../projects/clingo/clingo/libclingo
)

target_link_libraries(clingo-test
  PRIVATE
  ${CMAKE_BINARY_DIR}/lib/libclingo.a
  ${CMAKE_BINARY_DIR}/lib/libgringo.a
  ${CMAKE_BINARY_DIR}/lib/libreify.a
  ${CMAKE_BINARY_DIR}/lib/libclasp.dylib
  ${CMAKE_BINARY_DIR}/lib/libpotassco.dylib
  LLVMSupport
)
```

**`llvm/tools/clingo-test/ClingoTest.cpp`:**

```cpp
#include "clingo.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

int main() {
  std::cout << "=== Clingo C API Test ===\n";
  
  clingo_control_t *ctl = NULL;
  
  // Create a clingo control object
  if (!clingo_control_new(nullptr, 0, nullptr, nullptr, 0, &ctl)) {
    std::cerr << "Failed to create control object\n";
    return 1;
  }
  std::cout << "✓ Created clingo control object\n";
  
  // Add a simple logic program
  const char *program = "a. b. c.";
  if (!clingo_control_add(ctl, "base", nullptr, 0, program)) {
    std::cerr << "Failed to add program\n";
    clingo_control_free(ctl);
    return 1;
  }
  std::cout << "✓ Added logic program\n";
  
  // Ground the program
  clingo_part_t parts[] = {{{\"base\", 4}, nullptr, 0}};
  if (!clingo_control_ground(ctl, parts, 1, nullptr, nullptr)) {
    std::cerr << "Failed to ground program\n";
    clingo_control_free(ctl);
    return 1;
  }
  std::cout << "✓ Grounded program\n";
  
  // Clean up
  clingo_control_free(ctl);
  
  std::cout << "\n=== Test Complete ===\n";
  std::cout << "Clingo C API integration with LLVM successful!\n";
  return 0;
}
```

Build and test:

```bash
cd llvm-project-ASP-regalloc/build
ninja clingo-test
./bin/clingo-test
```

Expected output:

```
=== Clingo C API Test ===
✓ Created clingo control object
✓ Added logic program
✓ Grounded program

=== Test Complete ===
Clingo C API integration with LLVM successful!
```

## Troubleshooting

### CMake says clingo project is disabled

Make sure you have:
1. Clingo is registered in `LLVM_EXTRA_PROJECTS` in `llvm/CMakeLists.txt` (✓ already configured)
2. Passed `-DLLVM_ENABLE_PROJECTS="clingo;..."` when configuring LLVM
3. Initialized clingo's submodules: `cd clingo && git submodule update --init --recursive`

### Linker errors about undefined symbols

**From Gringo or Reify:** These symbols come from `libgringo.a` and `libreify.a`. Ensure your `target_link_libraries` includes them in the correct order:

```cmake
target_link_libraries(your-target
  PRIVATE
  ${CMAKE_BINARY_DIR}/lib/libclingo.a      # Must be first
  ${CMAKE_BINARY_DIR}/lib/libgringo.a      # Must come after libclingo.a
  ${CMAKE_BINARY_DIR}/lib/libreify.a       # Must come after libgringo.a
  ${CMAKE_BINARY_DIR}/lib/libclasp.dylib
  ${CMAKE_BINARY_DIR}/lib/libpotassco.dylib
)
```

**Important:** The order of static libraries matters. libclingo.a references symbols from libgringo.a, which references symbols from libreify.a. Linkers process libraries left-to-right, so order is critical.

### Build fails in clasp/gringo compilation

Common issues:
- Ensure clingo's submodules are initialized: `cd clingo && git submodule status` (all should show hashes, not minus signs)
- Ensure you have a modern C++ compiler (LLVM requires C++17, which includes C++14 support needed by clasp)
- Try a clean rebuild: `rm -rf build && mkdir build && cd build && cmake .. -DLLVM_ENABLE_PROJECTS="clingo;..." -GNinja && ninja`
- For verbose output: `ninja -v` to see actual compilation commands

### Include path errors for clingo.h

When clingo is built as part of LLVM, headers are in the clingo source tree. Use the correct relative path:

```cmake
target_include_directories(your-target
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../projects/clingo/clingo/libclingo
)
```

Adjust the relative path based on your tool's location. For tools in `llvm/tools/your-tool/`, the path above is correct. For components in `llvm/lib/YourComponent/`, use `../../projects/clingo/...` or similar.

### "clingo isn't a known project"

This means clingo isn't registered as an LLVM project. Verify:
1. `clingo` is in `LLVM_EXTRA_PROJECTS` in `llvm/CMakeLists.txt`
2. The clingo source exists at `projects/clingo/`
3. Reconfigure CMake: `cmake .. -DLLVM_ENABLE_PROJECTS="clingo;..."`

### Linking against "clingo" target fails

If you try to link against a `clingo` target and get "unknown target" errors, this is expected. When clingo is built as part of LLVM's monorepo build system, it doesn't create a standard CMake target. Instead, link directly against the built libraries using their full paths in `${CMAKE_BINARY_DIR}/lib/`.

## Next Steps

1. **Add clingo support to your component:**
   - Modify your component's `CMakeLists.txt` to link against clingo
   - Include `clingo.h` in your source files

2. **Implement clingo-based optimizations:**
   - Use the clingo C API to encode optimization problems
   - Solve them and apply results to LLVM IR

3. **Test thoroughly:**
   - Add unit tests for clingo integration
   - Verify correctness on real LLVM optimization passes

## Resources

- [Clingo Documentation](https://potassco.org/clingo/)
- [Clingo C API Reference](https://potassco.org/clingo/python-api/)
- [LLVM CMake Documentation](https://llvm.org/docs/CMake.html)
- Clingo submodule: `llvm-project-ASP-regalloc/clingo/`
