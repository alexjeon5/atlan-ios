# 추출 범위

`upstream.json`에 고정한 Kernarium 커밋의 `darwin/cpu/arm64/aarch32` 중 디코더와 정수·분기·메모리 의미론만 포함한다. 파일별 SPDX `GPL-2.0-or-later`와 원본 주석을 보존했다. 프로젝트 전체 라이선스 본문은 루트 `LICENSE`에 있다.

원본 대비 변경:

- `semantics.hpp`, `semantics.cpp`, `semantics_internal.hpp`, `semantics_memory.cpp`: 커널의 메모리 접근 enum과 CPU 상태를 `atlan`의 독립 인터페이스로 교체했다.
- `semantics_table.cpp`, `semantics.cpp`: 가져오지 않은 벡터 핸들러를 연결하지 않는다. 벡터 메모리 명령도 지원 범위에서 제외한다.
- `semantics_control.cpp`: 직접 ARM64 타이머 레지스터를 읽는 대신 1 GHz 단위의 `steady_clock` 기반 가상 카운터를 사용한다.
- `semantics_integer.cpp`: Clang 전용 bit-reverse builtin을 동일한 비트 연산으로 교체했다.
- `guest_key.hpp`: SME 헤더 의존성을 제거했다. 사용하지 않는 A64 key 헬퍼를 위한 마스크 상수만 남겼다.

다른 가져온 소스는 원본과 동일하다. 원본의 전체 의미론 커버리지 주장은 이 부분 추출에 적용되지 않는다. 실행 지원 여부는 축소된 `op_info` 테이블이 결정한다. 레거시 ARMv5 명령 중 Armv8 EL0 프로파일에서 빠진 SWP 같은 명령은 현재 undefined로 처리한다.
