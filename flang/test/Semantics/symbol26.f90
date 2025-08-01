! RUN: %python %S/test_symbols.py %s %flang_fc1
! Regression test for https://github.com/llvm/llvm-project/issues/62598
! Ensure that implicitly typed names in module NAMELIST groups receive
! the module's default accessibility attribute.
!DEF: /m Module
module m
 !DEF: /m/a PUBLIC Namelist
 !DEF: /m/j PUBLIC (Implicit, InNamelist) ObjectEntity INTEGER(4)
 namelist/a/j
end module m
!DEF: /MAIN MainProgram
program MAIN
 !DEF: /MAIN/j (Implicit) ObjectEntity INTEGER(4)
 j = 1
contains
 !DEF: /MAIN/inner (Subroutine) Subprogram
 subroutine inner
  !REF: /m
  use :: m
  !DEF: /MAIN/inner/j (Implicit, InNamelist) Use INTEGER(4)
  j = 2
 end subroutine
end program
