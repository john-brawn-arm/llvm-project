; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 5
; RUN: opt -p instcombine -S %s | FileCheck %s

define <4 x i16> @ext_identity_mask_first_vector_first_half_4xi16(<8 x i8> %x) {
; CHECK-LABEL: define <4 x i16> @ext_identity_mask_first_vector_first_half_4xi16(
; CHECK-SAME: <8 x i8> [[X:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[TMP0:%.*]] = shufflevector <8 x i8> [[X]], <8 x i8> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
; CHECK-NEXT:    [[SHUFFLE:%.*]] = zext <4 x i8> [[TMP0]] to <4 x i16>
; CHECK-NEXT:    ret <4 x i16> [[SHUFFLE]]
;
entry:
  %e.1 = zext <8 x i8> %x to <8 x i16>
  %shuffle = shufflevector <8 x i16> %e.1, <8 x i16> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  ret <4 x i16> %shuffle
}

define <3 x i32> @ext_identity_mask_first_vector_first_half_3xi32(<4 x i16> %x) {
; CHECK-LABEL: define <3 x i32> @ext_identity_mask_first_vector_first_half_3xi32(
; CHECK-SAME: <4 x i16> [[X:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[TMP0:%.*]] = shufflevector <4 x i16> [[X]], <4 x i16> poison, <3 x i32> <i32 0, i32 1, i32 2>
; CHECK-NEXT:    [[SHUFFLE:%.*]] = zext <3 x i16> [[TMP0]] to <3 x i32>
; CHECK-NEXT:    ret <3 x i32> [[SHUFFLE]]
;
entry:
  %e.1 = zext <4 x i16> %x to <4 x i32>
  %shuffle = shufflevector <4 x i32> %e.1, <4 x i32> poison, <3 x i32> <i32 0, i32 1, i32 2>
  ret <3 x i32> %shuffle
}

define <4 x i16> @ext_no_identity_mask1(<8 x i8> %in) {
; CHECK-LABEL: define <4 x i16> @ext_no_identity_mask1(
; CHECK-SAME: <8 x i8> [[IN:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[E_1:%.*]] = zext <8 x i8> [[IN]] to <8 x i16>
; CHECK-NEXT:    [[SHUFFLE:%.*]] = shufflevector <8 x i16> [[E_1]], <8 x i16> poison, <4 x i32> <i32 1, i32 2, i32 3, i32 4>
; CHECK-NEXT:    ret <4 x i16> [[SHUFFLE]]
;
entry:
  %e.1 = zext <8 x i8> %in to <8 x i16>
  %shuffle = shufflevector <8 x i16> %e.1, <8 x i16> poison, <4 x i32> <i32 1, i32 2, i32 3, i32 4>
  ret <4 x i16> %shuffle
}

define <4 x i16> @ext_no_identity_mask2(<8 x i8> %x, <8 x i16> %y) {
; CHECK-LABEL: define <4 x i16> @ext_no_identity_mask2(
; CHECK-SAME: <8 x i8> [[X:%.*]], <8 x i16> [[Y:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[E_1:%.*]] = zext <8 x i8> [[X]] to <8 x i16>
; CHECK-NEXT:    [[SHUFFLE:%.*]] = shufflevector <8 x i16> [[E_1]], <8 x i16> poison, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
; CHECK-NEXT:    ret <4 x i16> [[SHUFFLE]]
;
entry:
  %e.1 = zext <8 x i8> %x to <8 x i16>
  %shuffle = shufflevector <8 x i16> %e.1, <8 x i16> %y, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
  ret <4 x i16> %shuffle
}

define <5 x i32> @ext_identity_mask_first_vector_first_half_5xi32(<4 x i16> %x) {
; CHECK-LABEL: define <5 x i32> @ext_identity_mask_first_vector_first_half_5xi32(
; CHECK-SAME: <4 x i16> [[X:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[E_1:%.*]] = zext <4 x i16> [[X]] to <4 x i32>
; CHECK-NEXT:    [[SHUFFLE:%.*]] = shufflevector <4 x i32> [[E_1]], <4 x i32> poison, <5 x i32> <i32 0, i32 1, i32 2, i32 3, i32 0>
; CHECK-NEXT:    ret <5 x i32> [[SHUFFLE]]
;
entry:
  %e.1 = zext <4 x i16> %x to <4 x i32>
  %shuffle = shufflevector <4 x i32> %e.1, <4 x i32> %e.1, <5 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4>
  ret <5 x i32> %shuffle
}

define <4 x i16> @ext_no_identity_mask_first_vector_second_half(<8 x i8> %x, <8 x i16> %y) {
; CHECK-LABEL: define <4 x i16> @ext_no_identity_mask_first_vector_second_half(
; CHECK-SAME: <8 x i8> [[X:%.*]], <8 x i16> [[Y:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[E_1:%.*]] = zext <8 x i8> [[X]] to <8 x i16>
; CHECK-NEXT:    [[SHUFFLE:%.*]] = shufflevector <8 x i16> [[E_1]], <8 x i16> [[Y]], <4 x i32> <i32 0, i32 9, i32 1, i32 10>
; CHECK-NEXT:    ret <4 x i16> [[SHUFFLE]]
;
entry:
  %e.1 = zext <8 x i8> %x to <8 x i16>
  %shuffle = shufflevector <8 x i16> %e.1, <8 x i16> %y, <4 x i32> <i32 0, i32 9, i32 1, i32 10>
  ret <4 x i16> %shuffle
}

define <4 x i16> @select_second_op(<8 x i8> %x, <8 x i16> %y) {
; CHECK-LABEL: define <4 x i16> @select_second_op(
; CHECK-SAME: <8 x i8> [[X:%.*]], <8 x i16> [[Y:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[SHUFFLE:%.*]] = shufflevector <8 x i16> [[Y]], <8 x i16> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
; CHECK-NEXT:    ret <4 x i16> [[SHUFFLE]]
;
entry:
  %e.1 = zext <8 x i8> %x to <8 x i16>
  %shuffle = shufflevector <8 x i16> %e.1, <8 x i16> %y, <4 x i32> <i32 8, i32 9, i32 10, i32 11>
  ret <4 x i16> %shuffle
}

define <4 x i32> @load_i32_zext_to_v4i32(ptr %di) {
; CHECK-LABEL: define <4 x i32> @load_i32_zext_to_v4i32(
; CHECK-SAME: ptr [[DI:%.*]]) {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[L1:%.*]] = load <4 x i8>, ptr [[DI]], align 4
; CHECK-NEXT:    [[EXT_2:%.*]] = zext <4 x i8> [[L1]] to <4 x i32>
; CHECK-NEXT:    ret <4 x i32> [[EXT_2]]
;
entry:
  %l = load i32, ptr %di
  %vec.ins = insertelement <2 x i32> <i32 poison, i32 0>, i32 %l, i64 0
  %vec.bc = bitcast <2 x i32> %vec.ins to <8 x i8>
  %e.1 = zext <8 x i8> %vec.bc to <8 x i16>
  %vec.shuffle = shufflevector <8 x i16> %e.1, <8 x i16> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %ext.2 = zext nneg <4 x i16> %vec.shuffle to <4 x i32>
  ret <4 x i32> %ext.2
}
