; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -passes=loop-vectorize -mcpu=corei7-avx -S -vectorizer-min-trip-count=21 | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux"

;
; The source code for the test:
;
; void foo(ptr restrict A, ptr restrict B)
; {
;     for (int i = 0; i < 20; ++i) A[i] += B[i];
; }
;

;
; This loop will be vectorized, although the trip count is below the threshold, but
; vectorization is explicitly forced in metadata. The trip count of 4 is chosen as
; it more nicely divides the loop count of 20, produce a lower total cost.
;
define void @vectorized(ptr noalias nocapture %A, ptr noalias nocapture readonly %B) {
; CHECK-LABEL: @vectorized(
; CHECK-NEXT:  iter.check:
; CHECK-NEXT:    br i1 false, label [[VEC_EPILOG_SCALAR_PH:%.*]], label [[VECTOR_MAIN_LOOP_ITER_CHECK:%.*]]
; CHECK:       vector.main.loop.iter.check:
; CHECK-NEXT:    br i1 false, label [[VEC_EPILOG_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[TMP1:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 [[INDEX]]
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr inbounds float, ptr [[TMP1]], i32 4
; CHECK-NEXT:    [[TMP4:%.*]] = getelementptr inbounds float, ptr [[TMP1]], i32 8
; CHECK-NEXT:    [[TMP5:%.*]] = getelementptr inbounds float, ptr [[TMP1]], i32 12
; CHECK-NEXT:    [[WIDE_LOAD:%.*]] = load <4 x float>, ptr [[TMP1]], align 4, !llvm.access.group [[ACC_GRP0:![0-9]+]]
; CHECK-NEXT:    [[WIDE_LOAD1:%.*]] = load <4 x float>, ptr [[TMP3]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[WIDE_LOAD2:%.*]] = load <4 x float>, ptr [[TMP4]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[WIDE_LOAD3:%.*]] = load <4 x float>, ptr [[TMP5]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr inbounds float, ptr [[A:%.*]], i64 [[INDEX]]
; CHECK-NEXT:    [[TMP8:%.*]] = getelementptr inbounds float, ptr [[TMP6]], i32 4
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr inbounds float, ptr [[TMP6]], i32 8
; CHECK-NEXT:    [[TMP10:%.*]] = getelementptr inbounds float, ptr [[TMP6]], i32 12
; CHECK-NEXT:    [[WIDE_LOAD4:%.*]] = load <4 x float>, ptr [[TMP6]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[WIDE_LOAD5:%.*]] = load <4 x float>, ptr [[TMP8]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[WIDE_LOAD6:%.*]] = load <4 x float>, ptr [[TMP9]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[WIDE_LOAD7:%.*]] = load <4 x float>, ptr [[TMP10]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[TMP11:%.*]] = fadd fast <4 x float> [[WIDE_LOAD]], [[WIDE_LOAD4]]
; CHECK-NEXT:    [[TMP12:%.*]] = fadd fast <4 x float> [[WIDE_LOAD1]], [[WIDE_LOAD5]]
; CHECK-NEXT:    [[TMP13:%.*]] = fadd fast <4 x float> [[WIDE_LOAD2]], [[WIDE_LOAD6]]
; CHECK-NEXT:    [[TMP14:%.*]] = fadd fast <4 x float> [[WIDE_LOAD3]], [[WIDE_LOAD7]]
; CHECK-NEXT:    store <4 x float> [[TMP11]], ptr [[TMP6]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    store <4 x float> [[TMP12]], ptr [[TMP8]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    store <4 x float> [[TMP13]], ptr [[TMP9]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    store <4 x float> [[TMP14]], ptr [[TMP10]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 16
; CHECK-NEXT:    [[TMP15:%.*]] = icmp eq i64 [[INDEX_NEXT]], 16
; CHECK-NEXT:    br i1 [[TMP15]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP1:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br i1 false, label [[FOR_END:%.*]], label [[VEC_EPILOG_ITER_CHECK:%.*]]
; CHECK:       vec.epilog.iter.check:
; CHECK-NEXT:    br i1 false, label [[VEC_EPILOG_SCALAR_PH]], label [[VEC_EPILOG_PH]]
; CHECK:       vec.epilog.ph:
; CHECK-NEXT:    [[VEC_EPILOG_RESUME_VAL:%.*]] = phi i64 [ 16, [[VEC_EPILOG_ITER_CHECK]] ], [ 0, [[VECTOR_MAIN_LOOP_ITER_CHECK]] ]
; CHECK-NEXT:    br label [[VEC_EPILOG_VECTOR_BODY:%.*]]
; CHECK:       vec.epilog.vector.body:
; CHECK-NEXT:    [[INDEX8:%.*]] = phi i64 [ [[VEC_EPILOG_RESUME_VAL]], [[VEC_EPILOG_PH]] ], [ [[INDEX_NEXT11:%.*]], [[VEC_EPILOG_VECTOR_BODY]] ]
; CHECK-NEXT:    [[TMP17:%.*]] = getelementptr inbounds float, ptr [[B]], i64 [[INDEX8]]
; CHECK-NEXT:    [[WIDE_LOAD9:%.*]] = load <4 x float>, ptr [[TMP17]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[TMP19:%.*]] = getelementptr inbounds float, ptr [[A]], i64 [[INDEX8]]
; CHECK-NEXT:    [[WIDE_LOAD10:%.*]] = load <4 x float>, ptr [[TMP19]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[TMP21:%.*]] = fadd fast <4 x float> [[WIDE_LOAD9]], [[WIDE_LOAD10]]
; CHECK-NEXT:    store <4 x float> [[TMP21]], ptr [[TMP19]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[INDEX_NEXT11]] = add nuw i64 [[INDEX8]], 4
; CHECK-NEXT:    [[TMP22:%.*]] = icmp eq i64 [[INDEX_NEXT11]], 20
; CHECK-NEXT:    br i1 [[TMP22]], label [[VEC_EPILOG_MIDDLE_BLOCK:%.*]], label [[VEC_EPILOG_VECTOR_BODY]], !llvm.loop [[LOOP5:![0-9]+]]
; CHECK:       vec.epilog.middle.block:
; CHECK-NEXT:    br i1 true, label [[FOR_END]], label [[VEC_EPILOG_SCALAR_PH]]
; CHECK:       vec.epilog.scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ 20, [[VEC_EPILOG_MIDDLE_BLOCK]] ], [ 16, [[VEC_EPILOG_ITER_CHECK]] ], [ 0, [[ITER_CHECK:%.*]] ]
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[INDVARS_IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[VEC_EPILOG_SCALAR_PH]] ], [ [[INDVARS_IV_NEXT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds float, ptr [[B]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    [[TMP23:%.*]] = load float, ptr [[ARRAYIDX]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[ARRAYIDX2:%.*]] = getelementptr inbounds float, ptr [[A]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    [[TMP24:%.*]] = load float, ptr [[ARRAYIDX2]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[ADD:%.*]] = fadd fast float [[TMP23]], [[TMP24]]
; CHECK-NEXT:    store float [[ADD]], ptr [[ARRAYIDX2]], align 4, !llvm.access.group [[ACC_GRP0]]
; CHECK-NEXT:    [[INDVARS_IV_NEXT]] = add nuw nsw i64 [[INDVARS_IV]], 1
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i64 [[INDVARS_IV_NEXT]], 20
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP6:![0-9]+]]
; CHECK:       for.end:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  %arrayidx = getelementptr inbounds float, ptr %B, i64 %indvars.iv
  %0 = load float, ptr %arrayidx, align 4, !llvm.access.group !11
  %arrayidx2 = getelementptr inbounds float, ptr %A, i64 %indvars.iv
  %1 = load float, ptr %arrayidx2, align 4, !llvm.access.group !11
  %add = fadd fast float %0, %1
  store float %add, ptr %arrayidx2, align 4, !llvm.access.group !11
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond = icmp eq i64 %indvars.iv.next, 20
  br i1 %exitcond, label %for.end, label %for.body, !llvm.loop !1

for.end:
  ret void
}

!1 = !{!1, !2, !{!"llvm.loop.parallel_accesses", !11}}
!2 = !{!"llvm.loop.vectorize.enable", i1 true}
!11 = distinct !{}

;
; This loop will be vectorized as the trip count is below the threshold but no
; scalar iterations are needed thanks to folding its tail.
;
define void @vectorized1(ptr noalias nocapture %A, ptr noalias nocapture readonly %B) {
; CHECK-LABEL: @vectorized1(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[BROADCAST_SPLATINSERT:%.*]] = insertelement <8 x i64> poison, i64 [[INDEX]], i64 0
; CHECK-NEXT:    [[BROADCAST_SPLAT:%.*]] = shufflevector <8 x i64> [[BROADCAST_SPLATINSERT]], <8 x i64> poison, <8 x i32> zeroinitializer
; CHECK-NEXT:    [[VEC_IV:%.*]] = add <8 x i64> [[BROADCAST_SPLAT]], <i64 0, i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7>
; CHECK-NEXT:    [[TMP1:%.*]] = icmp ule <8 x i64> [[VEC_IV]], splat (i64 19)
; CHECK-NEXT:    [[TMP2:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 [[INDEX]]
; CHECK-NEXT:    [[WIDE_MASKED_LOAD:%.*]] = call <8 x float> @llvm.masked.load.v8f32.p0(ptr [[TMP2]], i32 4, <8 x i1> [[TMP1]], <8 x float> poison), !llvm.access.group [[ACC_GRP7:![0-9]+]]
; CHECK-NEXT:    [[TMP4:%.*]] = getelementptr inbounds float, ptr [[A:%.*]], i64 [[INDEX]]
; CHECK-NEXT:    [[WIDE_MASKED_LOAD1:%.*]] = call <8 x float> @llvm.masked.load.v8f32.p0(ptr [[TMP4]], i32 4, <8 x i1> [[TMP1]], <8 x float> poison), !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[TMP6:%.*]] = fadd fast <8 x float> [[WIDE_MASKED_LOAD]], [[WIDE_MASKED_LOAD1]]
; CHECK-NEXT:    call void @llvm.masked.store.v8f32.p0(<8 x float> [[TMP6]], ptr [[TMP4]], i32 4, <8 x i1> [[TMP1]]), !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 8
; CHECK-NEXT:    [[TMP7:%.*]] = icmp eq i64 [[INDEX_NEXT]], 24
; CHECK-NEXT:    br i1 [[TMP7]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP8:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br label [[FOR_END:%.*]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ 0, [[ENTRY:%.*]] ]
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[INDVARS_IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[INDVARS_IV_NEXT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds float, ptr [[B]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    [[TMP8:%.*]] = load float, ptr [[ARRAYIDX]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[ARRAYIDX2:%.*]] = getelementptr inbounds float, ptr [[A]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    [[TMP9:%.*]] = load float, ptr [[ARRAYIDX2]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[ADD:%.*]] = fadd fast float [[TMP8]], [[TMP9]]
; CHECK-NEXT:    store float [[ADD]], ptr [[ARRAYIDX2]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[INDVARS_IV_NEXT]] = add nuw nsw i64 [[INDVARS_IV]], 1
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i64 [[INDVARS_IV_NEXT]], 20
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP10:![0-9]+]]
; CHECK:       for.end:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  %arrayidx = getelementptr inbounds float, ptr %B, i64 %indvars.iv
  %0 = load float, ptr %arrayidx, align 4, !llvm.access.group !13
  %arrayidx2 = getelementptr inbounds float, ptr %A, i64 %indvars.iv
  %1 = load float, ptr %arrayidx2, align 4, !llvm.access.group !13
  %add = fadd fast float %0, %1
  store float %add, ptr %arrayidx2, align 4, !llvm.access.group !13
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond = icmp eq i64 %indvars.iv.next, 20
  br i1 %exitcond, label %for.end, label %for.body, !llvm.loop !3

for.end:
  ret void
}

!3 = !{!3, !{!"llvm.loop.parallel_accesses", !13}}
!13 = distinct !{}

;
; This loop will be vectorized as the trip count is below the threshold but no
; scalar iterations are needed.
;
define void @vectorized2(ptr noalias nocapture %A, ptr noalias nocapture readonly %B) {
; CHECK-LABEL: @vectorized2(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[TMP1:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 [[INDEX]]
; CHECK-NEXT:    [[WIDE_LOAD:%.*]] = load <8 x float>, ptr [[TMP1]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr inbounds float, ptr [[A:%.*]], i64 [[INDEX]]
; CHECK-NEXT:    [[WIDE_LOAD1:%.*]] = load <8 x float>, ptr [[TMP3]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[TMP5:%.*]] = fadd fast <8 x float> [[WIDE_LOAD]], [[WIDE_LOAD1]]
; CHECK-NEXT:    store <8 x float> [[TMP5]], ptr [[TMP3]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 8
; CHECK-NEXT:    [[TMP6:%.*]] = icmp eq i64 [[INDEX_NEXT]], 16
; CHECK-NEXT:    br i1 [[TMP6]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP11:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br label [[FOR_END:%.*]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ 0, [[ENTRY:%.*]] ]
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[INDVARS_IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[INDVARS_IV_NEXT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds float, ptr [[B]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    [[TMP7:%.*]] = load float, ptr [[ARRAYIDX]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[ARRAYIDX2:%.*]] = getelementptr inbounds float, ptr [[A]], i64 [[INDVARS_IV]]
; CHECK-NEXT:    [[TMP8:%.*]] = load float, ptr [[ARRAYIDX2]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[ADD:%.*]] = fadd fast float [[TMP7]], [[TMP8]]
; CHECK-NEXT:    store float [[ADD]], ptr [[ARRAYIDX2]], align 4, !llvm.access.group [[ACC_GRP7]]
; CHECK-NEXT:    [[INDVARS_IV_NEXT]] = add nuw nsw i64 [[INDVARS_IV]], 1
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i64 [[INDVARS_IV_NEXT]], 16
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP12:![0-9]+]]
; CHECK:       for.end:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  %arrayidx = getelementptr inbounds float, ptr %B, i64 %indvars.iv
  %0 = load float, ptr %arrayidx, align 4, !llvm.access.group !13
  %arrayidx2 = getelementptr inbounds float, ptr %A, i64 %indvars.iv
  %1 = load float, ptr %arrayidx2, align 4, !llvm.access.group !13
  %add = fadd fast float %0, %1
  store float %add, ptr %arrayidx2, align 4, !llvm.access.group !13
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond = icmp eq i64 %indvars.iv.next, 16
  br i1 %exitcond, label %for.end, label %for.body, !llvm.loop !4

for.end:
  ret void
}

!4 = !{!4}

