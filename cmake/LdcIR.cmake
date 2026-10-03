# ldc_compile_ir(<src> <out-var>): compile a C++ file to LLVM IR the way ldc expects it
# (clang++ from the SVF image, -O0 without optnone, debug info for names, then mem2reg).
# -flto -fwhole-program-vtables only add type tests at virtual calls: the static class of
# the receiver, used as DeclTypeOf(r) (see Builder::declaredClass).

get_filename_component(_ldc_llvm_bin "${LLVM_DIR}/../../../bin" ABSOLUTE)
find_program(LDC_CLANGXX clang++ HINTS "${_ldc_llvm_bin}" REQUIRED)
find_program(LDC_OPT opt HINTS "${_ldc_llvm_bin}" REQUIRED)

function(ldc_compile_ir src out_var)
  get_filename_component(name "${src}" NAME_WE)
  set(raw "${CMAKE_CURRENT_BINARY_DIR}/${name}.raw.ll")
  set(ll  "${CMAKE_CURRENT_BINARY_DIR}/${name}.ll")
  add_custom_command(
    OUTPUT "${ll}"
    COMMAND "${LDC_CLANGXX}" -S -g -fno-discard-value-names
            -Xclang -disable-O0-optnone -flto -fwhole-program-vtables -emit-llvm "${src}" -o "${raw}"
    COMMAND "${LDC_OPT}" -S -p=mem2reg "${raw}" -o "${ll}"
    DEPENDS "${src}"
    COMMENT "IR: ${name}"
    VERBATIM)
  set(${out_var} "${ll}" PARENT_SCOPE)
endfunction()
