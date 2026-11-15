; Handwritten AIR: out[thread_position_in_grid.x] = x * 7 + 3.
; Exact dispatch bounds and untouched buffer sentinels are checked by the CPU.
source_filename = "venus_compute"
target triple = "air64-apple-ios16.5.0"
define void @venus_compute(ptr addrspace(1) %out, <3 x i32> %tid) {
  %x = extractelement <3 x i32> %tid, i32 0
  %product = mul i32 %x, 7
  %value = add i32 %product, 3
  %index = zext i32 %x to i64
  %p = getelementptr i32, ptr addrspace(1) %out, i64 %index
  store i32 %value, ptr addrspace(1) %p, align 4
  ret void
}
!air.kernel = !{!2}
!air.version = !{!0}
!air.language_version = !{!1}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @venus_compute, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"tid"}
