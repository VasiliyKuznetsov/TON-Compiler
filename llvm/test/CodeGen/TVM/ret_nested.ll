; RUN: llc -O3 < %s -march=tvm | FileCheck %s
target datalayout = "E-S257-i1:257:257-i8:257:257-i16:257:257-i32:257:257-i64:257:257-i257:257:257-p:257:257-a:257:257"
target triple = "tvm"

define i257 @g(i257 %N) nounwind {
  ret i257 %N
}

; CHECK-LABEL: f:
define i257 @f(i257 %N) nounwind {
; CHECK: PUSH c0
; CHECK: PUSHCONT
; CHECK: PUSHINT 1
; CHECK: PUSHINT $g$
; CHECK: CALL 1
; CHECK: SWAP
; CHECK: POP	c0
; CHECK: }
; CHECK: PUSHCONT
; CHECK: IFELSE

  %status = icmp sgt i257 %N, 0
  br i1 %status, label %then, label %else

else:
  %1 = call i257 @g(i257 1)
  ret i257 %1

then:
  %2 = call i257 @g(i257 2)
  ret i257 %2
}

; the part below has been added only for runtime testing
define void @test() nounwind {
  %1 = call i257 @f(i257 0)
  %2 = sub i257 %1, 2
  call void @llvm.tvm.throwif(i257 %2, i257 13)
  ret void
}

declare void @llvm.tvm.throwif(i257 %cond, i257 %exception)

declare i257 @undefined_f(i257)

; CHECK-LABEL: l:
define i257 @l(i257 %x) {
; CHECK: PUSH c0
; CHECK: PUSHCONT
; CHECK: {
; CHECK:   PUSHCONT
; CHECK:   {
; CHECK:     POP c0
; CHECK:   }
; CHECK:   PUSHCONT
; CHECK:   {
; CHECK:     POP c0
; CHECK:   }
; CHECK:   IFELSE
; CHECK: }
; CHECK: PUSHCONT
; CHECK: {
; CHECK:   POP c0
; CHECK: }
; CHECK: IFELSE
entry:
  %0 = icmp eq i257 %x, 0
  br i1 %0, label %if.else, label %if.then

if.then:
  %1 = tail call i257 @undefined_f(i257 %x)
  %2 = icmp ne i257 %1, 0
  br i1 %2, label %if.then2, label %if.else2

if.then2:
  %3 = tail call i257 @undefined_f(i257 %x)
  ret i257 %3

if.else2:
  ret i257 4

if.else:
  ret i257 0
}

; CHECK-LABEL: k:
define i257 @k(i257 %x) {
entry:
; CHECK-NOT: PUSH c0
; CHECK: ADDCONST 3
; CHECK-NOT: POP c0
  %add = add nsw i257 %x, 3
  ret i257 %add
}
