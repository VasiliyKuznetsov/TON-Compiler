; XFAIL: *
; RUN: llc < %s -march=tvm | FileCheck %s 
target datalayout = "E-S257-i1:257:257-i8:257:257-i16:257:257-i32:257:257-i64:257:257-i257:257:257-p:257:257-a:257:257"  
target triple = "tvm" 

define { i257, i257 } @divmodc (i257 %dvd, i257 %dvr) nounwind {
; CHECK-LABEL: divmodc:
; CHECK: DIVMODC
 %dvd_neg = icmp slt i257 0, %dvd
 %dvr_neg = icmp slt i257 0, %dvr
 %res_neg = xor i1 %dvd_neg, %dvr_neg
 %pr_rem  = srem i257 %dvd, %dvr
 %no_rem  = icmp eq i257 0, %pr_rem
 %n_cor   = or i1 %res_neg, %no_rem
 %cor = select i1 %n_cor, i257 0, i257 1
 %qtnt_pr = sdiv i257 %dvd, %dvr
 %qtnt = add i257 %qtnt_pr, %cor
 
 %qtmul = mul i257 %qtnt, %dvr
 %remf   = sub i257 %dvd, %qtmul

 %agg1 = insertvalue {i257, i257} undef, i257 %remf, 0
 %agg2 = insertvalue {i257, i257} %agg1, i257 %qtnt, 1
 ret { i257, i257 } %agg2
}

