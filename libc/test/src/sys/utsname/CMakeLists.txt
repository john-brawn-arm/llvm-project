add_custom_target(libc_sys_utsname_unittests)

add_libc_unittest(
  uname_test
  SUITE
    libc_sys_utsname_unittests
  SRCS
    uname_test.cpp
  DEPENDS
    libc.include.sys_utsname
    libc.src.errno.errno
    libc.src.__support.common
    libc.src.sys.utsname.uname
    libc.test.UnitTest.ErrnoSetterMatcher
)
