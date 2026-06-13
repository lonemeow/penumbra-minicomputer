// REQUIRES: penumbra-registered-target
// RUN: %clang_cc1 -triple penumbra-unknown-none -emit-llvm -o - %s | FileCheck %s

// Pins the slot-based aggregate calling convention from doc/system/abi.md
// ("Argument Passing" / "Return Values"): an aggregate classifies by total
// size alone into one 4-byte slot (i32), two slots ([2 x i32]), or a pointer
// to a caller-owned copy.  Returns mirror the same classes, with larger
// aggregates returned through a hidden sret pointer.
//
// The keystone is that the over-8-byte class is indirect *non-byval*: clang
// materializes the by-value copy in IR and hands the backend a plain pointer,
// so the param carries no `byval` attribute (see the @a16 / call_a16 checks).

struct S3  { char a, b, c; };   // 3 bytes  -> one slot
struct S4  { int a; };          // 4 bytes  -> one slot
struct S6  { short a, b, c; };  // 6 bytes  -> two slots
struct S8  { int a, b; };       // 8 bytes  -> two slots
struct S16 { int a, b, c, d; }; // 16 bytes -> indirect
struct E   { };                 // empty    -> ignored

void sink(void *);

// ---- Argument classification --------------------------------------------

// CHECK-LABEL: define {{.*}}void @a3(i32 %s.coerce)
void a3(struct S3 s) { sink(&s); }

// CHECK-LABEL: define {{.*}}void @a4(i32 %s.coerce)
void a4(struct S4 s) { sink(&s); }

// CHECK-LABEL: define {{.*}}void @a6([2 x i32] %s.coerce)
void a6(struct S6 s) { sink(&s); }

// CHECK-LABEL: define {{.*}}void @a8([2 x i32] %s.coerce)
void a8(struct S8 s) { sink(&s); }

// Indirect, and crucially *not* byval — the pointer is to a caller-made copy.
// CHECK-LABEL: define {{.*}}void @a16(ptr dead_on_return noundef %s)
void a16(struct S16 s) { sink(&s); }

// Empty aggregate carries no argument slot at all.
// CHECK-LABEL: define {{.*}}void @ae()
void ae(struct E s) { (void)s; }

// ---- Return classification ----------------------------------------------

// CHECK-LABEL: define {{.*}}i32 @r3()
struct S3 r3(void) { struct S3 s = {1, 2, 3}; return s; }

// CHECK-LABEL: define {{.*}}i32 @r4()
struct S4 r4(void) { struct S4 s = {4}; return s; }

// CHECK-LABEL: define {{.*}}[2 x i32] @r6()
struct S6 r6(void) { struct S6 s = {5, 6, 7}; return s; }

// CHECK-LABEL: define {{.*}}[2 x i32] @r8()
struct S8 r8(void) { struct S8 s = {8, 9}; return s; }

// CHECK-LABEL: define {{.*}}void @r16(ptr {{.*}}sret(%struct.S16) {{.*}}%agg.result)
struct S16 r16(void) { struct S16 s = {1, 2, 3, 4}; return s; }

// ---- Caller side: the over-8 copy is materialized as an alloca + memcpy ----

// CHECK-LABEL: define {{.*}}void @call_a16(
// CHECK:         %[[TMP:[a-z0-9.-]+]] = alloca %struct.S16
// CHECK:         call void @llvm.memcpy{{.*}}(ptr {{.*}}%[[TMP]], ptr {{.*}}%v,
// CHECK:         call void @a16(ptr {{.*}}%[[TMP]])
void call_a16(struct S16 v) { a16(v); }
