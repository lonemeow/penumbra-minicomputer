; RUN: opt -passes="print<cost-model>" -cost-kind=throughput -disable-output \
; RUN:     -mtriple=penumbra-unknown-none < %s 2>&1 | FileCheck %s \
; RUN:     --check-prefixes=THRU,THRU-FASTMUL
; RUN: opt -passes="print<cost-model>" -cost-kind=throughput -disable-output \
; RUN:     -mtriple=penumbra-unknown-none -mcpu=penumbra1 < %s 2>&1 \
; RUN:     | FileCheck %s --check-prefixes=THRU,THRU-SLOWMUL
; RUN: opt -passes="print<cost-model>" -cost-kind=code-size -disable-output \
; RUN:     -mtriple=penumbra-unknown-none < %s 2>&1 | FileCheck %s \
; RUN:     --check-prefix=SIZE

; MUL and DIV run on the sequential divmul peer unit rather than in the
; ALU, so throughput charges them in ALU operations: the cycles the tuned
; core's scheduling model states for them over an ALU operation's, rounded
; up.  The default tuning multiplies in far fewer cycles than it divides;
; penumbra1 takes as long for either, and its ALU operations are slower too.
; Left at the BasicTTIImpl default both price as 1, the same as an ADD,
; and the middle end prefers multiply-shaped canonical forms.

define void @arith(i32 %a, i32 %b) {
; THRU-LABEL: function 'arith'
; THRU: cost of 1 for instruction: %add = add i32
; THRU: cost of 1 for instruction: %sub = sub i32
; THRU: cost of 1 for instruction: %and = and i32
; THRU: cost of 1 for instruction: %shl = shl i32
; THRU-FASTMUL: cost of 10 for instruction: %mul = mul i32
; THRU-SLOWMUL: cost of 19 for instruction: %mul = mul i32
; THRU-FASTMUL: cost of 34 for instruction: %sdiv = sdiv i32
; THRU-SLOWMUL: cost of 19 for instruction: %sdiv = sdiv i32
; THRU-FASTMUL: cost of 34 for instruction: %udiv = udiv i32
; THRU-SLOWMUL: cost of 19 for instruction: %udiv = udiv i32
; THRU-FASTMUL: cost of 34 for instruction: %srem = srem i32
; THRU-SLOWMUL: cost of 19 for instruction: %srem = srem i32
; THRU-FASTMUL: cost of 34 for instruction: %urem = urem i32
; THRU-SLOWMUL: cost of 19 for instruction: %urem = urem i32
;
; Code size is left to the base model: one instruction is emitted
; however long it runs, so a slow operation must not look bulky to the
; inliner or the unroller.
; SIZE-LABEL: function 'arith'
; SIZE: cost of 1 for instruction: %mul = mul i32
; SIZE: cost of 4 for instruction: %sdiv = sdiv i32
  %add = add i32 %a, %b
  %sub = sub i32 %a, %b
  %and = and i32 %a, %b
  %shl = shl i32 %a, %b
  %mul = mul i32 %a, %b
  %sdiv = sdiv i32 %a, %b
  %udiv = udiv i32 %a, %b
  %srem = srem i32 %a, %b
  %urem = urem i32 %a, %b
  ret void
}

; A type that legalizes into several native operations is charged for
; each of them rather than once.
define void @arith_i64(i64 %a, i64 %b) {
; THRU-LABEL: function 'arith_i64'
; THRU-FASTMUL: cost of 20 for instruction: %mul = mul i64
; THRU-SLOWMUL: cost of 38 for instruction: %mul = mul i64
  %mul = mul i64 %a, %b
  ret void
}
