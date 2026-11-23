# ELF 로더와 함수 호출

`atlan::elf::load`는 little-endian ELF32 ARM EABI5 공유 라이브러리를 독립 게스트 주소 공간에 로드한다. 프로그램 헤더와 동적 테이블만 사용하므로 섹션 헤더가 없어도 동작한다. `PT_LOAD`의 파일 내용·BSS, SysV 동적 심볼, `DT_NEEDED`, `DT_INIT`·`DT_INIT_ARRAY`와 RELRO를 처리한다.

지원하는 재배치는 `R_ARM_RELATIVE`, `R_ARM_ABS32`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`이다. ARM/Thumb 함수 주소의 하위 비트를 보존한다. GLOB_DAT와 JUMP_SLOT은 기존 GOT/PLT word를 해석된 심볼 주소로 대체한다. [Arm ELF ABI의 동적 재배치 규약](https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst#dynamic-relocations)을 따른다.

로더는 별도 이미지에서 검증과 재배치를 끝낸 뒤 결과를 반환한다. 파일·가상 주소 범위, 세그먼트 정렬, 메모리 예산, 심볼·문자열·재배치 테이블을 확인하고 실행·쓰기 동시 권한은 거부한다. 게스트 코드는 호스트 실행 메모리에 매핑하지 않는다.

`Options::resolve`는 외부 심볼을 **게스트 주소**로 연결하는 확장점이다. 의존성 이름을 수집하지만 다른 라이브러리를 자동으로 로드하지는 않는다. 기본 설정에서는 해석되지 않은 strong import가 있으면 로드에 실패한다. 명시적인 진단 모드에서는 import마다 미매핑 주소를 지정하므로 호출·데이터 접근이 오류로 멈춘다. 가짜 성공 반환이나 가짜 데이터 객체를 제공하지 않는다. 미해결 weak 심볼은 ELF 규약에 따라 0이 된다.

호스트 도구 `atlan-elf --help`에서 입력·호출 옵션을 확인할 수 있다. 생성자는 `--initialize`를 명시할 때만 실행하며, 초기화나 함수 호출이 실패하면 다음 호출로 진행하지 않는다. 초기화 이전 호출은 해당 함수의 의존성을 이해한 경우에만 유효하다. 생성자 실행 중 변경한 메모리는 실패 시 되돌리지 않으므로 초기화를 재시도하려면 이미지를 다시 로드한다.

`Runtime::call`은 최대 네 개의 32비트 정수·포인터 인자, 8바이트 정렬 스택과 LR 반환 주소를 설정한다. 반환 sentinel에 도달하면 `function_return`을 반환하며, 그렇지 않으면 최초 이벤트나 실행 예산 초과를 보고한다. 부동소수점·64비트·구조체 인자, 스택으로 전달하는 추가 인자와 Java 객체 변환은 아직 제공하지 않는다. 게스트 메모리를 호스트에서 수정한 경우 실행을 재개하기 전에 exclusive monitor를 해제해야 한다.

GNU hash만 있는 이미지, 심볼 버전, TLS, IFUNC, RELA·Android packed relocations, 텍스트 재배치와 겹치는 세그먼트 내용은 지원하지 않는다. 지원하지 않는 형식은 오류로 보고한다. 합성 ELF 테스트는 두 load bias, ARM·Thumb 반환, BSS, 네 가지 재배치, RELRO, import trap과 잘못된 입력을 검증한다.

## Import dispatch와 객체 수명

`ImportDispatcher`는 진단 모드에서 만든 import trap 주소의 실행 fault를 심볼 이름으로 찾아 등록된 호스트 서비스로 보낸다. 서비스는 r0-r3 인자만 읽고 r0 결과를 설정하며, 반환 후 LR(bit 0은 Thumb 선택)로 복귀한다. 등록되지 않은 import는 `Stop::unresolved_import`로, 서비스 실패는 `Stop::service_error`로 멈추며 성공 반환을 만들지 않는다. 데이터 import(`STT_OBJECT`)는 디스패치하지 않는다. 실행 예산은 서비스가 게스트 콜백을 호출해도 하나로 공유한다.

`ObjectLifetime`은 `__aeabi_atexit(obj, dtor, dso)`, `__cxa_atexit(dtor, obj, dso)`, `__cxa_finalize(dso)`를 구현한다. 소멸자 주소·인자·DSO 핸들은 게스트 값으로만 저장하고 호스트 `atexit`에 넘기지 않는다. finalize는 최신 등록부터 실행하며 항목을 실행 전에 제거하므로 두 번 실행되지 않는다. 소멸자는 `call_back`으로 호출자의 CPU 상태를 보존한 채 제한된 예산 안에서 실행한다. null 소멸자, 레지스트리 초과, 소멸자의 예산 초과·fault는 오류로 보고한다.

`install_libc_services`는 관찰된 import만 타입 있는 서비스로 구현한다: `memset`(게스트 메모리 쓰기, 크기 상한, 주소 래핑 거부)과 `clock_gettime`(32비트 `timespec`, CLOCK_REALTIME·CLOCK_MONOTONIC만 지원하며 다른 clock id는 오류).
