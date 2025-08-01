; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 4
; RUN: opt -S -passes=loop-vectorize -mcpu=skylake-avx512 -mtriple=x86_64-apple-macosx -S %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

; Test cases based on https://github.com/llvm/llvm-project/issues/91883.
define void @iv.4_used_as_vector_and_first_lane(ptr %src, ptr noalias %dst) {
; CHECK-LABEL: define void @iv.4_used_as_vector_and_first_lane(
; CHECK-SAME: ptr [[SRC:%.*]], ptr noalias [[DST:%.*]]) #[[ATTR0:[0-9]+]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[VEC_IND:%.*]] = phi <4 x i64> [ <i64 0, i64 1, i64 2, i64 3>, [[VECTOR_PH]] ], [ [[VEC_IND_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[STEP_ADD:%.*]] = add <4 x i64> [[VEC_IND]], splat (i64 4)
; CHECK-NEXT:    [[STEP_ADD_2:%.*]] = add <4 x i64> [[STEP_ADD]], splat (i64 4)
; CHECK-NEXT:    [[STEP_ADD_3:%.*]] = add <4 x i64> [[STEP_ADD_2]], splat (i64 4)
; CHECK-NEXT:    [[TMP4:%.*]] = getelementptr inbounds i64, ptr [[SRC]], i64 [[INDEX]]
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr inbounds i64, ptr [[TMP4]], i32 4
; CHECK-NEXT:    [[TMP10:%.*]] = getelementptr inbounds i64, ptr [[TMP4]], i32 8
; CHECK-NEXT:    [[TMP11:%.*]] = getelementptr inbounds i64, ptr [[TMP4]], i32 12
; CHECK-NEXT:    [[WIDE_LOAD:%.*]] = load <4 x i64>, ptr [[TMP4]], align 8
; CHECK-NEXT:    [[WIDE_LOAD4:%.*]] = load <4 x i64>, ptr [[TMP9]], align 8
; CHECK-NEXT:    [[WIDE_LOAD5:%.*]] = load <4 x i64>, ptr [[TMP10]], align 8
; CHECK-NEXT:    [[WIDE_LOAD6:%.*]] = load <4 x i64>, ptr [[TMP11]], align 8
; CHECK-NEXT:    [[TMP12:%.*]] = add <4 x i64> [[VEC_IND]], splat (i64 4)
; CHECK-NEXT:    [[TMP13:%.*]] = add <4 x i64> [[STEP_ADD]], splat (i64 4)
; CHECK-NEXT:    [[TMP14:%.*]] = add <4 x i64> [[STEP_ADD_2]], splat (i64 4)
; CHECK-NEXT:    [[TMP15:%.*]] = add <4 x i64> [[STEP_ADD_3]], splat (i64 4)
; CHECK-NEXT:    [[TMP16:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD]], splat (i64 128)
; CHECK-NEXT:    [[TMP17:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD4]], splat (i64 128)
; CHECK-NEXT:    [[TMP18:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD5]], splat (i64 128)
; CHECK-NEXT:    [[TMP19:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD6]], splat (i64 128)
; CHECK-NEXT:    [[TMP26:%.*]] = extractelement <4 x i64> [[TMP12]], i32 0
; CHECK-NEXT:    [[TMP27:%.*]] = add i64 [[TMP26]], 1
; CHECK-NEXT:    [[TMP28:%.*]] = getelementptr i64, ptr [[DST]], i64 [[TMP27]]
; CHECK-NEXT:    [[TMP33:%.*]] = getelementptr i64, ptr [[TMP28]], i32 4
; CHECK-NEXT:    [[TMP34:%.*]] = getelementptr i64, ptr [[TMP28]], i32 8
; CHECK-NEXT:    [[TMP35:%.*]] = getelementptr i64, ptr [[TMP28]], i32 12
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[TMP12]], ptr [[TMP28]], i32 4, <4 x i1> [[TMP16]])
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[TMP13]], ptr [[TMP33]], i32 4, <4 x i1> [[TMP17]])
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[TMP14]], ptr [[TMP34]], i32 4, <4 x i1> [[TMP18]])
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[TMP15]], ptr [[TMP35]], i32 4, <4 x i1> [[TMP19]])
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 16
; CHECK-NEXT:    [[VEC_IND_NEXT]] = add <4 x i64> [[STEP_ADD_3]], splat (i64 4)
; CHECK-NEXT:    [[TMP36:%.*]] = icmp eq i64 [[INDEX_NEXT]], 32
; CHECK-NEXT:    br i1 [[TMP36]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br label [[EXIT:%.*]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ 0, [[ENTRY:%.*]] ]
; CHECK-NEXT:    br label [[LOOP_HEADER:%.*]]
; CHECK:       loop.header:
; CHECK-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[LOOP_LATCH:%.*]] ]
; CHECK-NEXT:    [[G_SRC:%.*]] = getelementptr inbounds i64, ptr [[SRC]], i64 [[IV]]
; CHECK-NEXT:    [[L:%.*]] = load i64, ptr [[G_SRC]], align 8
; CHECK-NEXT:    [[IV_4:%.*]] = add nuw nsw i64 [[IV]], 4
; CHECK-NEXT:    [[C:%.*]] = icmp ule i64 [[L]], 128
; CHECK-NEXT:    br i1 [[C]], label [[LOOP_THEN:%.*]], label [[LOOP_LATCH]]
; CHECK:       loop.then:
; CHECK-NEXT:    [[OR:%.*]] = or disjoint i64 [[IV_4]], 1
; CHECK-NEXT:    [[G_DST:%.*]] = getelementptr inbounds i64, ptr [[DST]], i64 [[OR]]
; CHECK-NEXT:    store i64 [[IV_4]], ptr [[G_DST]], align 4
; CHECK-NEXT:    br label [[LOOP_LATCH]]
; CHECK:       loop.latch:
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i64 [[IV_NEXT]], 32
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[EXIT]], label [[LOOP_HEADER]], !llvm.loop [[LOOP3:![0-9]+]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %g.src = getelementptr inbounds i64, ptr %src, i64 %iv
  %l = load i64, ptr %g.src
  %iv.4 = add nuw nsw i64 %iv, 4
  %c = icmp ule i64 %l, 128
  br i1 %c, label %loop.then, label %loop.latch

loop.then:
  %or = or disjoint i64 %iv.4, 1
  %g.dst = getelementptr inbounds i64, ptr %dst, i64 %or
  store i64 %iv.4, ptr %g.dst, align 4
  br label %loop.latch

loop.latch:
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond = icmp eq i64 %iv.next, 32
  br i1 %exitcond, label %exit, label %loop.header

exit:
  ret void
}

define void @iv.4_used_as_first_lane(ptr %src, ptr noalias %dst) {
; CHECK-LABEL: define void @iv.4_used_as_first_lane(
; CHECK-SAME: ptr [[SRC:%.*]], ptr noalias [[DST:%.*]]) #[[ATTR0]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[TMP4:%.*]] = getelementptr inbounds i64, ptr [[SRC]], i64 [[INDEX]]
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr inbounds i64, ptr [[TMP4]], i32 4
; CHECK-NEXT:    [[TMP10:%.*]] = getelementptr inbounds i64, ptr [[TMP4]], i32 8
; CHECK-NEXT:    [[TMP11:%.*]] = getelementptr inbounds i64, ptr [[TMP4]], i32 12
; CHECK-NEXT:    [[WIDE_LOAD:%.*]] = load <4 x i64>, ptr [[TMP4]], align 8
; CHECK-NEXT:    [[WIDE_LOAD1:%.*]] = load <4 x i64>, ptr [[TMP9]], align 8
; CHECK-NEXT:    [[WIDE_LOAD2:%.*]] = load <4 x i64>, ptr [[TMP10]], align 8
; CHECK-NEXT:    [[WIDE_LOAD3:%.*]] = load <4 x i64>, ptr [[TMP11]], align 8
; CHECK-NEXT:    [[TMP15:%.*]] = add i64 [[INDEX]], 4
; CHECK-NEXT:    [[TMP16:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD]], splat (i64 128)
; CHECK-NEXT:    [[TMP17:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD1]], splat (i64 128)
; CHECK-NEXT:    [[TMP18:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD2]], splat (i64 128)
; CHECK-NEXT:    [[TMP19:%.*]] = icmp ule <4 x i64> [[WIDE_LOAD3]], splat (i64 128)
; CHECK-NEXT:    [[TMP23:%.*]] = add i64 [[TMP15]], 1
; CHECK-NEXT:    [[TMP24:%.*]] = getelementptr i64, ptr [[DST]], i64 [[TMP23]]
; CHECK-NEXT:    [[TMP29:%.*]] = getelementptr i64, ptr [[TMP24]], i32 4
; CHECK-NEXT:    [[TMP30:%.*]] = getelementptr i64, ptr [[TMP24]], i32 8
; CHECK-NEXT:    [[TMP31:%.*]] = getelementptr i64, ptr [[TMP24]], i32 12
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[WIDE_LOAD]], ptr [[TMP24]], i32 4, <4 x i1> [[TMP16]])
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[WIDE_LOAD1]], ptr [[TMP29]], i32 4, <4 x i1> [[TMP17]])
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[WIDE_LOAD2]], ptr [[TMP30]], i32 4, <4 x i1> [[TMP18]])
; CHECK-NEXT:    call void @llvm.masked.store.v4i64.p0(<4 x i64> [[WIDE_LOAD3]], ptr [[TMP31]], i32 4, <4 x i1> [[TMP19]])
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 16
; CHECK-NEXT:    [[TMP32:%.*]] = icmp eq i64 [[INDEX_NEXT]], 32
; CHECK-NEXT:    br i1 [[TMP32]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP4:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br label [[EXIT:%.*]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ 0, [[ENTRY:%.*]] ]
; CHECK-NEXT:    br label [[LOOP_HEADER:%.*]]
; CHECK:       loop.header:
; CHECK-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[LOOP_LATCH:%.*]] ]
; CHECK-NEXT:    [[G_SRC:%.*]] = getelementptr inbounds i64, ptr [[SRC]], i64 [[IV]]
; CHECK-NEXT:    [[L:%.*]] = load i64, ptr [[G_SRC]], align 8
; CHECK-NEXT:    [[IV_4:%.*]] = add nuw nsw i64 [[IV]], 4
; CHECK-NEXT:    [[C:%.*]] = icmp ule i64 [[L]], 128
; CHECK-NEXT:    br i1 [[C]], label [[LOOP_THEN:%.*]], label [[LOOP_LATCH]]
; CHECK:       loop.then:
; CHECK-NEXT:    [[OR:%.*]] = or disjoint i64 [[IV_4]], 1
; CHECK-NEXT:    [[G_DST:%.*]] = getelementptr inbounds i64, ptr [[DST]], i64 [[OR]]
; CHECK-NEXT:    store i64 [[L]], ptr [[G_DST]], align 4
; CHECK-NEXT:    br label [[LOOP_LATCH]]
; CHECK:       loop.latch:
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i64 [[IV_NEXT]], 32
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[EXIT]], label [[LOOP_HEADER]], !llvm.loop [[LOOP5:![0-9]+]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %g.src = getelementptr inbounds i64, ptr %src, i64 %iv
  %l = load i64, ptr %g.src
  %iv.4 = add nuw nsw i64 %iv, 4
  %c = icmp ule i64 %l, 128
  br i1 %c, label %loop.then, label %loop.latch

loop.then:
  %or = or disjoint i64 %iv.4, 1
  %g.dst = getelementptr inbounds i64, ptr %dst, i64 %or
  store i64 %l, ptr %g.dst, align 4
  br label %loop.latch

loop.latch:
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond = icmp eq i64 %iv.next, 32
  br i1 %exitcond, label %exit, label %loop.header

exit:
  ret void
}
;.
; CHECK: [[LOOP0]] = distinct !{[[LOOP0]], [[META1:![0-9]+]], [[META2:![0-9]+]]}
; CHECK: [[META1]] = !{!"llvm.loop.isvectorized", i32 1}
; CHECK: [[META2]] = !{!"llvm.loop.unroll.runtime.disable"}
; CHECK: [[LOOP3]] = distinct !{[[LOOP3]], [[META2]], [[META1]]}
; CHECK: [[LOOP4]] = distinct !{[[LOOP4]], [[META1]], [[META2]]}
; CHECK: [[LOOP5]] = distinct !{[[LOOP5]], [[META2]], [[META1]]}
;.
