; ModuleID = 'programs/repro.c'
source_filename = "programs/repro.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx26.0.0"

@A = internal global [16 x i8] zeroinitializer, align 16
@B = internal global [16 x i8] zeroinitializer, align 16

; Function Attrs: nofree noinline norecurse nosync nounwind ssp memory(argmem: readwrite) uwtable
define void @bytecopy(ptr noundef writeonly captures(none) %d, ptr noundef readonly captures(none) %s, i32 noundef %n) local_unnamed_addr #0 {
entry:
  %cmp6 = icmp sgt i32 %n, 0
  br i1 %cmp6, label %iter.check, label %for.cond.cleanup

iter.check:                                       ; preds = %entry
  %d9 = ptrtoaddr ptr %d to i64
  %s10 = ptrtoaddr ptr %s to i64
  %wide.trip.count = zext nneg i32 %n to i64
  %min.iters.check = icmp ult i32 %n, 4
  %0 = sub i64 %d9, %s10
  %diff.check = icmp ult i64 %0, 32
  %or.cond = or i1 %min.iters.check, %diff.check
  br i1 %or.cond, label %for.body.preheader, label %vector.main.loop.iter.check

vector.main.loop.iter.check:                      ; preds = %iter.check
  %min.iters.check11 = icmp ult i32 %n, 32
  br i1 %min.iters.check11, label %vec.epilog.ph, label %vector.ph

vector.ph:                                        ; preds = %vector.main.loop.iter.check
  %n.mod.vf = and i64 %wide.trip.count, 28
  %n.vec = and i64 %wide.trip.count, 2147483616
  br label %vector.body

vector.body:                                      ; preds = %vector.body, %vector.ph
  %index = phi i64 [ 0, %vector.ph ], [ %index.next, %vector.body ]
  %1 = getelementptr inbounds nuw i8, ptr %s, i64 %index
  %2 = getelementptr inbounds nuw i8, ptr %1, i64 16
  %wide.load = load <16 x i8>, ptr %1, align 1, !tbaa !8
  %wide.load12 = load <16 x i8>, ptr %2, align 1, !tbaa !8
  %3 = getelementptr inbounds nuw i8, ptr %d, i64 %index
  %4 = getelementptr inbounds nuw i8, ptr %3, i64 16
  store <16 x i8> %wide.load, ptr %3, align 1, !tbaa !8
  store <16 x i8> %wide.load12, ptr %4, align 1, !tbaa !8
  %index.next = add nuw i64 %index, 32
  %5 = icmp eq i64 %index.next, %n.vec
  br i1 %5, label %middle.block, label %vector.body, !llvm.loop !9

middle.block:                                     ; preds = %vector.body
  %cmp.n = icmp eq i64 %n.vec, %wide.trip.count
  br i1 %cmp.n, label %for.cond.cleanup, label %vec.epilog.iter.check

vec.epilog.iter.check:                            ; preds = %middle.block
  %min.epilog.iters.check = icmp eq i64 %n.mod.vf, 0
  br i1 %min.epilog.iters.check, label %for.body.preheader, label %vec.epilog.ph, !prof !13

vec.epilog.ph:                                    ; preds = %vector.main.loop.iter.check, %vec.epilog.iter.check
  %vec.epilog.resume.val = phi i64 [ %n.vec, %vec.epilog.iter.check ], [ 0, %vector.main.loop.iter.check ]
  %n.vec14 = and i64 %wide.trip.count, 2147483644
  br label %vec.epilog.vector.body

vec.epilog.vector.body:                           ; preds = %vec.epilog.vector.body, %vec.epilog.ph
  %index15 = phi i64 [ %vec.epilog.resume.val, %vec.epilog.ph ], [ %index.next17, %vec.epilog.vector.body ]
  %6 = getelementptr inbounds nuw i8, ptr %s, i64 %index15
  %wide.load16 = load <4 x i8>, ptr %6, align 1, !tbaa !8
  %7 = getelementptr inbounds nuw i8, ptr %d, i64 %index15
  store <4 x i8> %wide.load16, ptr %7, align 1, !tbaa !8
  %index.next17 = add nuw i64 %index15, 4
  %8 = icmp eq i64 %index.next17, %n.vec14
  br i1 %8, label %vec.epilog.middle.block, label %vec.epilog.vector.body, !llvm.loop !14

vec.epilog.middle.block:                          ; preds = %vec.epilog.vector.body
  %cmp.n18 = icmp eq i64 %n.vec14, %wide.trip.count
  br i1 %cmp.n18, label %for.cond.cleanup, label %for.body.preheader

for.body.preheader:                               ; preds = %iter.check, %vec.epilog.iter.check, %vec.epilog.middle.block
  %indvars.iv.ph = phi i64 [ 0, %iter.check ], [ %n.vec, %vec.epilog.iter.check ], [ %n.vec14, %vec.epilog.middle.block ]
  %xtraiter = and i64 %wide.trip.count, 3
  %lcmp.mod.not = icmp eq i64 %xtraiter, 0
  br i1 %lcmp.mod.not, label %for.body.prol.loopexit, label %for.body.prol

for.body.prol:                                    ; preds = %for.body.preheader, %for.body.prol
  %indvars.iv.prol = phi i64 [ %indvars.iv.next.prol, %for.body.prol ], [ %indvars.iv.ph, %for.body.preheader ]
  %prol.iter = phi i64 [ %prol.iter.next, %for.body.prol ], [ 0, %for.body.preheader ]
  %arrayidx.prol = getelementptr inbounds nuw i8, ptr %s, i64 %indvars.iv.prol
  %9 = load i8, ptr %arrayidx.prol, align 1, !tbaa !8
  %arrayidx2.prol = getelementptr inbounds nuw i8, ptr %d, i64 %indvars.iv.prol
  store i8 %9, ptr %arrayidx2.prol, align 1, !tbaa !8
  %indvars.iv.next.prol = add nuw nsw i64 %indvars.iv.prol, 1
  %prol.iter.next = add i64 %prol.iter, 1
  %prol.iter.cmp.not = icmp eq i64 %prol.iter.next, %xtraiter
  br i1 %prol.iter.cmp.not, label %for.body.prol.loopexit, label %for.body.prol, !llvm.loop !15

for.body.prol.loopexit:                           ; preds = %for.body.prol, %for.body.preheader
  %indvars.iv.unr = phi i64 [ %indvars.iv.ph, %for.body.preheader ], [ %indvars.iv.next.prol, %for.body.prol ]
  %10 = sub nsw i64 %indvars.iv.ph, %wide.trip.count
  %11 = icmp ugt i64 %10, -4
  br i1 %11, label %for.cond.cleanup, label %for.body

for.cond.cleanup:                                 ; preds = %for.body.prol.loopexit, %for.body, %middle.block, %vec.epilog.middle.block, %entry
  ret void

for.body:                                         ; preds = %for.body.prol.loopexit, %for.body
  %indvars.iv = phi i64 [ %indvars.iv.next.3, %for.body ], [ %indvars.iv.unr, %for.body.prol.loopexit ]
  %arrayidx = getelementptr inbounds nuw i8, ptr %s, i64 %indvars.iv
  %12 = load i8, ptr %arrayidx, align 1, !tbaa !8
  %arrayidx2 = getelementptr inbounds nuw i8, ptr %d, i64 %indvars.iv
  store i8 %12, ptr %arrayidx2, align 1, !tbaa !8
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %arrayidx.1 = getelementptr inbounds nuw i8, ptr %s, i64 %indvars.iv.next
  %13 = load i8, ptr %arrayidx.1, align 1, !tbaa !8
  %arrayidx2.1 = getelementptr inbounds nuw i8, ptr %d, i64 %indvars.iv.next
  store i8 %13, ptr %arrayidx2.1, align 1, !tbaa !8
  %indvars.iv.next.1 = add nuw nsw i64 %indvars.iv, 2
  %arrayidx.2 = getelementptr inbounds nuw i8, ptr %s, i64 %indvars.iv.next.1
  %14 = load i8, ptr %arrayidx.2, align 1, !tbaa !8
  %arrayidx2.2 = getelementptr inbounds nuw i8, ptr %d, i64 %indvars.iv.next.1
  store i8 %14, ptr %arrayidx2.2, align 1, !tbaa !8
  %indvars.iv.next.2 = add nuw nsw i64 %indvars.iv, 3
  %arrayidx.3 = getelementptr inbounds nuw i8, ptr %s, i64 %indvars.iv.next.2
  %15 = load i8, ptr %arrayidx.3, align 1, !tbaa !8
  %arrayidx2.3 = getelementptr inbounds nuw i8, ptr %d, i64 %indvars.iv.next.2
  store i8 %15, ptr %arrayidx2.3, align 1, !tbaa !8
  %indvars.iv.next.3 = add nuw nsw i64 %indvars.iv, 4
  %exitcond.not.3 = icmp eq i64 %indvars.iv.next.3, %wide.trip.count
  br i1 %exitcond.not.3, label %for.cond.cleanup, label %for.body, !llvm.loop !17
}

; Function Attrs: nofree norecurse nosync nounwind ssp memory(readwrite, argmem: none, inaccessiblemem: none, target_mem: none) uwtable
define range(i32 0, 256) i32 @main() local_unnamed_addr #1 {
entry:
  tail call void @bytecopy(ptr noundef nonnull @A, ptr noundef nonnull @B, i32 noundef 0)
  %0 = load i8, ptr @A, align 16, !tbaa !8
  %conv = zext i8 %0 to i32
  ret i32 %conv
}

attributes #0 = { nofree noinline norecurse nosync nounwind ssp memory(argmem: readwrite) uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cmov,+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "tune-cpu"="generic" }
attributes #1 = { nofree norecurse nosync nounwind ssp memory(readwrite, argmem: none, inaccessiblemem: none, target_mem: none) uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cmov,+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "tune-cpu"="generic" }

!llvm.module.flags = !{!0, !1, !2}
!llvm.ident = !{!3}
!llvm.errno.tbaa = !{!4}

!0 = !{i32 8, !"PIC Level", i32 2}
!1 = !{i32 7, !"uwtable", i32 2}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{!"clang version 23.0.0git (https://github.com/grahamjonesgs/klausscpu-llvm.git 2fa6958fe09425038e3a874dcc79450a100d0a3b)"}
!4 = !{!5, !5, i64 0}
!5 = !{!"int", !6, i64 0}
!6 = !{!"omnipotent char", !7, i64 0}
!7 = !{!"Simple C/C++ TBAA"}
!8 = !{!6, !6, i64 0}
!9 = distinct !{!9, !10, !11, !12}
!10 = !{!"llvm.loop.mustprogress"}
!11 = !{!"llvm.loop.isvectorized", i32 1}
!12 = !{!"llvm.loop.unroll.runtime.disable"}
!13 = !{!"branch_weights", i32 4, i32 28}
!14 = distinct !{!14, !10, !11, !12}
!15 = distinct !{!15, !16}
!16 = !{!"llvm.loop.unroll.disable"}
!17 = distinct !{!17, !10, !11}
