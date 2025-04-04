// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=spirv64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm-bc %s -o %t-ppc-host.bc
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple nvptx64-nvidia-cuda -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - | FileCheck %s --check-prefix=BOOL_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple spirv64 -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=spirv64 -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - | FileCheck %s --check-prefix=SPIRV_BOOL_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple amdgcn-amd-amdhsa -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - | FileCheck %s --check-prefix=AMD_BOOL_RETURN_SAFE
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc -ffast-math -ffp-contract=fast
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=spirv64 -emit-llvm-bc %s -o %t-ppc-host.bc -ffast-math -ffp-contract=fast
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm-bc %s -o %t-ppc-host.bc -ffast-math -ffp-contract=fast
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple nvptx64-nvidia-cuda -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -ffast-math -ffp-contract=fast | FileCheck %s --check-prefix=BOOL_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple spirv64 -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=spirv64 -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -ffast-math -ffp-contract=fast | FileCheck %s --check-prefix=SPIRV_BOOL_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple amdgcn-amd-amdhsa -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -ffast-math -ffp-contract=fast | FileCheck %s --check-prefix=AMD_BOOL_RETURN_FAST
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc -DUSE_ISNAN_WITH_INT_RETURN
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=spirv64 -emit-llvm-bc %s -o %t-ppc-host.bc -DUSE_ISNAN_WITH_INT_RETURN
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm-bc %s -o %t-ppc-host.bc -DUSE_ISNAN_WITH_INT_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple nvptx64-nvidia-cuda -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -DUSE_ISNAN_WITH_INT_RETURN | FileCheck %s --check-prefix=INT_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple spirv64 -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=spirv64 -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -DUSE_ISNAN_WITH_INT_RETURN | FileCheck %s --check-prefix=SPIRV_INT_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple amdgcn-amd-amdhsa -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -DUSE_ISNAN_WITH_INT_RETURN | FileCheck %s --check-prefix=AMD_INT_RETURN_SAFE
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc -ffast-math -ffp-contract=fast -DUSE_ISNAN_WITH_INT_RETURN
// RUN: %clang_cc1 -x c++ -internal-isystem %S/Inputs/include -fopenmp -triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm-bc %s -o %t-ppc-host.bc -ffast-math -ffp-contract=fast -DUSE_ISNAN_WITH_INT_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple nvptx64-nvidia-cuda -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -ffast-math -ffp-contract=fast -DUSE_ISNAN_WITH_INT_RETURN | FileCheck %s --check-prefix=INT_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple spirv64 -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=spirv64 -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -ffast-math -ffp-contract=fast -DUSE_ISNAN_WITH_INT_RETURN | FileCheck %s --check-prefix=SPIRV_INT_RETURN
// RUN: %clang_cc1 -x c++ -include __clang_openmp_device_functions.h -internal-isystem %S/../../lib/Headers/openmp_wrappers -internal-isystem %S/Inputs/include -fopenmp -triple amdgcn-amd-amdhsa -aux-triple powerpc64le-unknown-unknown -fopenmp-targets=amdgcn-amd-amdhsa -emit-llvm %s -fopenmp-is-target-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - -ffast-math -ffp-contract=fast -DUSE_ISNAN_WITH_INT_RETURN | FileCheck %s --check-prefix=AMD_INT_RETURN_FAST
// expected-no-diagnostics

#include <cmath>

double math(float f, double d) {
  double r = 0;
  // INT_RETURN: call noundef i32 @__nv_isnanf(float
  // AMD_INT_RETURN_SAFE: call i1 @llvm.is.fpclass.f32(float{{.*}}, i32 3)
  // AMD_INT_RETURN_FAST: sitofp i32 {{.*}} to double
  // SPIRV_INT_RETURN: call spir_func noundef i32 @_Z5isnanf(float
  // BOOL_RETURN: call noundef i32 @__nv_isnanf(float
  // SPIRV_BOOL_RETURN: call spir_func noundef zeroext i1 @_Z5isnanf(float 
  // AMD_BOOL_RETURN_SAFE: call i1 @llvm.is.fpclass.f32(float{{.*}}, i32 3)
  // AMD_BOOL_RETURN_FAST: icmp ne i32 {{.*}}, 0
  r += std::isnan(f);
  // INT_RETURN: call noundef i32 @__nv_isnand(double
  // SPIRV_INT_RETURN: call spir_func noundef i32 @_Z5isnand(double
  // AMD_INT_RETURN_SAFE: call i1 @llvm.is.fpclass.f64(double{{.*}}, i32 3)
  // AMD_INT_RETURN_FAST: sitofp i32 {{.*}} to double
  // BOOL_RETURN: call noundef i32 @__nv_isnand(double
  // SPIRV_BOOL_RETURN: call spir_func noundef zeroext i1 @_Z5isnand(double
  // AMD_BOOL_RETURN_SAFE: call i1 @llvm.is.fpclass.f64(double{{.*}}, i32 3)
  // AMD_BOOL_RETURN_FAST: icmp ne i32 {{.*}}, 0
  r += std::isnan(d);
  return r;
}

long double foo(float f, double d, long double ld) {
  double r = ld;
  r += math(f, d);
#pragma omp target map(r)
  { r += math(f, d); }
  return r;
}
