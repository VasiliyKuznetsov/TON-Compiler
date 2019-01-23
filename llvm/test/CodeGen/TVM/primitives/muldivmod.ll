; XFAIL: *
; RUN: llc < %s -march=tvm | FileCheck %s 
target datalayout = "E-S1024-i256:256:256" 
target triple = "tvm" 

define { i64, i64 } @muldivmod (i64 %dvd1, i64 %dvd2, i64 %dvr) nounwind {
; CHECK-LABEL: muldivmod:
; CHECK: MULDIVMOD
 %dvd = mul i64 %dvd1, %dvd2
 %dvds    = icmp slt i64 0, %dvd
 %dvrs    = icmp slt i64 0, %dvr
 %qtnt    = sdiv i64 %dvd, %dvr
 %rem     = srem i64 %dvd, %dvr
 %rnz     = icmp ne i64 0, %rem
 %qtntneg = xor i1 %dvds, %dvrs
 %docor   = and i1 %qtntneg, %rnz
 %cor     = select i1 %docor, i64 -1, i64 0
 %qtntfin   = add i64 %qtnt, %cor
 
 %qmuldd1 = mul i64 %qtntfin, %dvr
 %remfin  = sub i64 %dvd, %qmuldd1
 
 %agg1 = insertvalue {i64, i64} undef, i64 %remfin, 0
 %agg2 = insertvalue {i64, i64} %agg1, i64 %qtntfin, 1
 ret { i64, i64 } %agg2
}