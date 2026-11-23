# Kernarium 구성요소 분석

참조 구현은 [alexjeon5/Kernarium](https://github.com/alexjeon5/Kernarium/tree/5aa987dbca2ee1af8ab57986aa221e8df8db18e5)이며 원래 upstream은 [imxieyi/Kernarium](https://github.com/imxieyi/Kernarium)이다. 참조 저장소는 수정하지 않는다.

| 구성요소 | 의존성 | 결정 |
| --- | --- | --- |
| `darwin/cpu/arm64/aarch32` 디코더 | 생성된 인코딩·피연산자 테이블, C++ 표준 라이브러리 | 그대로 분리. ARM/T16/T32를 공통 인터페이스로 해독한다. |
| 정수·분기·메모리 의미론 | CpuState, A32Memory, guest key, 4 KiB 게스트 페이지 규약 | 커널 ABI를 작은 상태·메모리 인터페이스로 교체해 재사용한다. |
| VFP/NEON 의미론 | 부동소수점 상태와 ARM64 보조 코드 | 초기 PoC에서 제외한다. 디코더의 인식과 실제 실행 지원을 구별한다. |
| ARM64 Gadget | 사전 컴파일된 어셈블리, TLB, 블록 구성·캐시, CpuState 레이아웃 | 후속 비 JIT 가속 후보. 원본 executor는 주소 공간·런타임 계약과 결합되어 있어 그대로 가져오지 않는다. |
| ARM64 JIT | 실행 메모리 arena, 캐시, 플랫폼의 코드 게시·무효화 계약 | 기본 iOS 경로에서 제외한다. AOT 연구의 lowering 참고 자료로만 둔다. |
| 메모리·CPU 상태 | 커널 bridge ABI, Apple 플랫폼·SME·태스크 상태 | 전체 모듈 대신 현재 의미론에 필요한 상태와 소프트웨어 페이지 매핑을 구현한다. |

추출 파일별 원본 SHA-256, 경로, 고정 커밋은 [upstream.json](../third_party/kernarium/upstream.json)에 있다. 생성된 디코더 테이블은 수정하지 않았다. 구체적인 분리 변경은 [third_party 설명](../third_party/kernarium/README.md)에 있다.

추출한 각 파일은 SPDX `GPL-2.0-or-later`를 명시한다. 동일한 라이선스의 저장소에 원본 헤더와 출처를 보존했다. 이 추출에는 upstream의 Apache/MIT/BSD 예외 파일을 포함하지 않았다. 향후 별도 파일을 추가할 때는 그 파일의 라이선스와 원 저작권 표시를 다시 확인한다. 원본 앱의 배포 권한과 iOS 배포 조건 검토는 이 소스 라이선스와 별개이며, 최종 앱 배포 전에 남아 있는 작업이다.
