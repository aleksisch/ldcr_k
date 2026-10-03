# ldc_compile_ir(<src> <out-var>): compile a C++ file to LLVM IR the way ldc expects it
# (clang++ from the selected LLVM installation, -O0 without optnone, debug info for names, then mem2reg).

# Avoid silently using unrelated LLVM tools from PATH. Explicit cache overrides
# are available for installations with a custom tool layout.
find_program(LDC_CLANGXX clang++ HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH REQUIRED)
find_program(LDC_OPT opt HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH REQUIRED)

function(ldc_compile_ir src out_var)
  get_filename_component(name "${src}" NAME_WE)
  set(raw "${CMAKE_CURRENT_BINARY_DIR}/${name}.raw.ll")
  set(ll  "${CMAKE_CURRENT_BINARY_DIR}/${name}.ll")
  add_custom_command(
    OUTPUT "${ll}"
    COMMAND "${LDC_CLANGXX}" -S -g -fno-discard-value-names
            -Xclang -disable-O0-optnone -emit-llvm "${src}" -o "${raw}"
    COMMAND "${LDC_OPT}" -S -p=mem2reg "${raw}" -o "${ll}"
    DEPENDS "${src}"
    COMMENT "IR: ${name}"
    VERBATIM)
  set(${out_var} "${ll}" PARENT_SCOPE)
endfunction()
