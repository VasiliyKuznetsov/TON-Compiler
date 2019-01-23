; XFAIL: *
; RUN: llc < %s -march=tvm | FileCheck %s 
target datalayout = "E-S1024-i256:256:256" 
target triple = "tvm" 

define i64 @modpow2 (i64 %dvd, i64 %tt) nounwind {
; CHECK-LABEL: modpow2:
; CHECK: MODPOW2
 %1 = add i64 %tt, 1
 %dvr = shl i64 2, %1
 
 %dvds    = icmp slt i64 0, %dvd
 %dvrs    = icmp slt i64 0, %dvr
 %qtnt    = sdiv i64 %dvd, %dvr
 %rem     = srem i64 %dvd, %dvr
 %rnz     = icmp ne i64 0, %rem
 %qtntneg = xor i1 %dvds, %dvrs
 %docor   = and i1 %qtntneg, %rnz
 %cor     = select i1 %docor, i64 -1, i64 0
 %qtntf   = add i64 %qtnt, %cor
 %fqmuldd = mul i64 %dvr, %qtntf 
 %rem     = sub i64 %dvd, %fqmuldd
 
 ret i64 %rem
}