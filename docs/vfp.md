# VFP 실행

Kernarium 정수 코어는 VFP·Advanced SIMD를 실행하지 않아서, `Runtime::step`이 A32 워드를 먼저 `atlan::vfp::execute_a32`에 보낸다. 스칼라 VFPv2/v3(단정밀·배정밀)만 구현한다.

지원: `VADD/VSUB/VMUL/VDIV/VMLA/VMLS/VNMLA/VNMLS/VNMUL`, `VMOV`(레지스터, 즉치, 코어↔S, 코어 쌍↔S 쌍/D), `VABS/VNEG/VSQRT`, `VCMP/VCMPE`(레지스터·0), `VCVT`(정수↔부동소수, f32↔f64, 정수 변환은 포화), `VLDR/VSTR/VLDM/VSTM/VPUSH/VPOP`, FPSCR의 `VMRS/VMSR`. 레지스터 파일은 `CpuState::vectors`의 바이트 배열이고 S, D 레지스터가 같은 저장소를 공유한다(D0-D31).

제한(명시적으로 `unsupported_instruction`과 `Result::detail`로 보고):
- Advanced SIMD, 고정소수점 `VCVT`, half-precision, `VCVTR` 외 반올림 변형, 코어↔스칼라 요소 `VMOV`, FPSCR 외 레지스터 `VMRS`/`VMSR`, `FLDMX/FSTMX`.
- FPSCR의 반올림 모드, flush-to-zero, default-NaN, vector length/stride가 설정되면 연산이 거부한다.
- Thumb 상태의 VFP는 아직 실행하지 않는다.
- 누적 예외 플래그는 `IOC`(비교의 unordered)만 기록한다. NaN 전파는 호스트 부동소수점 동작을 따른다.
- 4바이트로 정렬되지 않은 VFP 메모리 접근은 정렬 fault로 보고한다.
- `VMLA`류는 곱을 먼저 반올림하는 비융합 연산이며, 이를 위해 `engine/src/vfp.cpp`는 `-ffp-contract=off`로 컴파일한다.
