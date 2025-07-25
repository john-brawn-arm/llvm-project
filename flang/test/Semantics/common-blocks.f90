! RUN: %python %S/test_errors.py %s %flang_fc1 -pedantic

! Test check that enforce that a common block is initialized
! only once in a file.

subroutine init_1
  common x, y
  common /a/ xa, ya
  common /b/ xb, yb
  !WARNING: Blank COMMON object 'x' in a DATA statement is not standard [-Wdata-stmt-extensions]
  data x /42./, xa /42./, yb/42./
end subroutine

subroutine init_conflict
  !ERROR: Multiple initialization of COMMON block //
  common x, y
  !ERROR: Multiple initialization of COMMON block /a/
  common /a/ xa, ya
  common /b/ xb, yb
  equivalence (yb, yb_eq)
  !WARNING: Blank COMMON object 'x' in a DATA statement is not standard [-Wdata-stmt-extensions]
  !ERROR: Multiple initialization of COMMON block /b/
  data x /66./, xa /66./, yb_eq /66./
end subroutine
