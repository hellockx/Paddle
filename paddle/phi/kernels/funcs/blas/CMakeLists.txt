cc_library(
  blas
  SRCS blas.cc
  DEPS cblas framework_proto phi_backends)
