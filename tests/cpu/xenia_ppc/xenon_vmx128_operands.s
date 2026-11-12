# Xenon's own VMX128 operand-wiring cases, in the xenia PPC test format, for
# encodings the xenia hardware corpus does not reach: vperm128 with VC > 3
# and VA >= 64, vsel128 (old vD is the mask), the halfword VMX128 unpacks
# (vupkhsb128/vupklsb128 with VA128 = 0x60), VSCR.NJ flushing, NaN in
# vctsxs, and the single-precision fres/frsqrte estimates. Expected values
# follow the Xbox 360 VMX128 encoding (rexglue-sdk docs/ppc/vmx128.txt,
# binutils VC128 {7, 6}) and xenia's semantics. The VMX128 words are
# hand-encoded: the bundled assembler rejects VC > 3 and vector registers
# above v7 in these forms. Generated into xenon_vmx128_operands.inc.

test_vperm128_vc6_va70:
  #_ REGISTER_IN v70 [00010203, 04050607, 08090A0B, 0C0D0E0F]
  #_ REGISTER_IN v2 [10111213, 14151617, 18191A1B, 1C1D1E1F]
  #_ REGISTER_IN v6 [1F1E1D1C, 03020100, 13121110, 0F0E0D0C]
  #_ REGISTER_IN v5 [00000000, 00000000, 00000000, 00000000]
  .long 0x14661580  # vperm128 v3, v70, v2, v6
  blr
  #_ REGISTER_OUT v3 [1F1E1D1C, 03020100, 13121110, 0F0E0D0C]

test_vperm128_vc7:
  #_ REGISTER_IN v1 [00010203, 04050607, 08090A0B, 0C0D0E0F]
  #_ REGISTER_IN v2 [10111213, 14151617, 18191A1B, 1C1D1E1F]
  #_ REGISTER_IN v7 [00100111, 02120313, 04140515, 06160717]
  .long 0x146111C0  # vperm128 v3, v1, v2, v7
  blr
  #_ REGISTER_OUT v3 [00100111, 02120313, 04140515, 06160717]

test_vsel128_mask_is_old_vd:
  #_ REGISTER_IN v1 [11111111, 11111111, 11111111, 11111111]
  #_ REGISTER_IN v2 [22222222, 22222222, 22222222, 22222222]
  #_ REGISTER_IN v3 [FFFFFFFF, 00000000, FFFF0000, 0000FFFF]
  .long 0x14611350  # vsel128 v3, v1, v2, v3
  blr
  #_ REGISTER_OUT v3 [22222222, 11111111, 22221111, 11112222]

test_vsel128_high_registers:
  #_ REGISTER_IN v65 [AAAAAAAA, AAAAAAAA, AAAAAAAA, AAAAAAAA]
  #_ REGISTER_IN v98 [55555555, 55555555, 55555555, 55555555]
  #_ REGISTER_IN v100 [F0F0F0F0, 0F0F0F0F, 00000000, FFFFFFFF]
  .long 0x1481175F  # vsel128 v100, v65, v98, v100
  blr
  #_ REGISTER_OUT v100 [5A5A5A5A, A5A5A5A5, AAAAAAAA, 55555555]

test_vupkhsh128:
  #_ REGISTER_IN v2 [80017FFF, 0001FFFE, 12348000, 0000FFFF]
  .long 0x186017A0  # vupkhsh128 v3, v2
  blr
  #_ REGISTER_OUT v3 [FFFF8001, 00007FFF, 00000001, FFFFFFFE]

test_vupklsh128:
  #_ REGISTER_IN v2 [80017FFF, 0001FFFE, 12348000, 0000FFFF]
  .long 0x186017E0  # vupklsh128 v3, v2
  blr
  #_ REGISTER_OUT v3 [00001234, FFFF8000, 00000000, FFFFFFFF]

test_vupkhsh128_high_registers:
  #_ REGISTER_IN v66 [80017FFF, 0001FFFE, 12348000, 0000FFFF]
  .long 0x186017AE  # vupkhsh128 v99, v66
  blr
  #_ REGISTER_OUT v99 [FFFF8001, 00007FFF, 00000001, FFFFFFFE]

test_vaddfp_flushes_denormals:
  #_ REGISTER_IN v1 [00000001, 3F800000, 007FFFFF, 80000001]
  #_ REGISTER_IN v2 [00000000, 00000000, 00000001, 3F800000]
  vaddfp v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [00000000, 3F800000, 00000000, 3F800000]

test_vctsxs_nan_is_zero:
  #_ REGISTER_IN v1 [7FC00000, FFC00000, 3F800000, 4F000000]
  vctsxs v3, v1, 0
  blr
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000001, 7FFFFFFF]

test_fres_is_single:
  #_ REGISTER_IN f2 3.0
  fres f1, f2
  blr
  #_ REGISTER_OUT f1 0.3333333432674408

test_frsqrte_is_single:
  #_ REGISTER_IN f2 3.0
  frsqrte f1, f2
  blr
  #_ REGISTER_OUT f1 0.5773502588272095
