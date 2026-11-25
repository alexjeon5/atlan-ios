# 엔진 / 어댑터 경계와 분리 계획

지금은 특정 앱 하나를 돌리는 데 집중하지만, 구조는 나중에 런타임만 범용 엔진으로 떼어낼 수 있게 유지한다. 이 문서는 어느 모듈이 엔진이고 어느 부분이 앱별 어댑터인지, 그리고 진행 중인 작업과 충돌하지 않는 단계별 리팩터 순서를 정한다. 코드는 이 문서로 바뀌지 않는다.

## 용어

- **엔진**: ARM32 CPU 의미론, ELF 로더, import 디스패치, libc·pthread 서비스, JNI 기반 구조, 호스트 플랫폼 서비스(파일·그래픽·오디오·입력). 특정 앱을 전제하지 않는다.
- **어댑터**: 특정 앱 하나를 구동하기 위한 값과 순서. 엔진이 제공하는 인터페이스를 채우는 쪽이다.

판단 기준은 한 가지다. 다른 앱을 돌린다고 가정했을 때 바뀌어야 하는 것은 어댑터, 그대로 쓰는 것은 엔진이다.

## 현재 모듈 분류

| 위치 | 분류 | 비고 |
| --- | --- | --- |
| `third_party/kernarium` | 엔진 | CPU·디코더. GPL-2.0-or-later 출처 유지 |
| `src/memory.cpp`, `runtime.cpp`, `vfp.cpp`, `include/atlan/cpu_state.hpp` | 엔진 | 게스트 주소 공간과 실행 |
| `src/elf.cpp` | 엔진 | ELF32 ARM 로더 |
| `src/imports.cpp`, `lifetime.cpp` | 엔진 | import 디스패치, atexit·생성자 |
| `src/libc_services.cpp`, `heap.cpp`, `threads.cpp` | 엔진 | libc·힙·스레드 서비스 |
| `src/native_functions.cpp`, `native_runtime.cpp`, `inflate.cpp`, `profiler.cpp` | 엔진 | 게스트 함수의 네이티브 대체(표준 런타임 라이브러리 이름만)와 프로파일러. 앱 고유 대체 집합은 어댑터가 넘김. [네이티브 대체](native.md) |
| `src/files.cpp` | 엔진 | 가상 파일 디스크립터. 파일 이름·배치는 어댑터가 정함 |
| `src/abi.cpp`, `src/android_log.cpp` | 엔진 | 인자 읽기·printf 전개·로그 |
| `src/jni.cpp` (`JavaBootstrap`) | **경계** | 테이블·트랩·핸들 구조는 엔진, 어떤 클래스·메서드에 어떻게 답하는지는 어댑터 |
| `src/demo.cpp`, `tools/`, `tests/` | 엔진 | 합성 입력만 사용 |
| `shell/session.*` | 엔진 | 스레드·예산·중지·프레임 루프. 앱 고유 값과 시작 순서는 `AppAdapter`로만 받음 |
| `shell/app_adapter.hpp` | 엔진 | 엔진이 앱에 묻는 인터페이스(`AppAdapter`, `AppHost`, `AppStartup`) |
| `shell/input.*`, `http_client.*`, `log_file.*` | 엔진 | 입력 큐·바인딩 해석, HTTP, 로그 파일. 바인딩 값은 어댑터 쪽 파일 |
| `adapters/atlan/` | 어댑터 | 패키지 위치, 설정 파일·폴더 별칭, Java 쪽 응답, 시작·프레임 진입점, 프레임 요청 규약 |
| `shell/java_services.*`, `shell/app_package.*`, `shell/guest_ini.*` | 어댑터 | `atlan_adapter` 라이브러리로 빌드. 파일 위치는 단계 5에서 `adapters/atlan/`로 옮김. APK 읽기 자체는 범용이라 그때 엔진 쪽으로 나눔 |
| `shell/data_file.*` | **경계** | 부분 읽기·교체·거부 메커니즘은 엔진, 헤더 형식 검증은 어댑터 |
| `shell/render_config.hpp` | 확인 필요 | 앱 고유 값이 있으면 어댑터로 |
| `platform/ios`, `platform/macos` | 엔진 쪽 호스트 계층 | 앱 고유 UI·번들 설정은 어댑터. iOS 앱은 지금 `AtlanAdapter`를 직접 만들고, 데이터 폴더 이름과 로그 분류도 아직 앱 기준 |

앱별 어댑터가 가져야 하는 것:

- JNI 응답: 어떤 클래스·메서드·필드가 존재하고 어떤 값을 돌려주는지, 어떤 네이티브 메서드를 어떤 순서로 부르는지.
- 시작 순서: 이미지 로드, `JNI_OnLoad`, 초기화 호출 순서.
- 입력 연결: 터치·위치(GPS) 같은 호스트 이벤트를 게스트 호출로 바꾸는 규칙.
- 데이터 파일 규약: 사용자가 가져온 파일의 이름·형식 검증.
- 렌더 설정과 UI 문구.

앱 이름, 바이너리, 심볼 목록, 관찰된 호출 기록 같은 분석 결과는 어댑터에 두더라도 공개하지 않는다. 공개 어댑터 코드는 합성 예제와 인터페이스 구현만 담고, 실제 앱용 값은 로컬 `private/`에서 주입한다.

## 목표 구조

```text
engine/            CPU, 메모리, ELF, import, libc, JNI 구조, 서비스 인터페이스
adapters/
  synthetic/       공개 테스트용 합성 어댑터(엔진 단독 검증)
  atlan/           앱별 시작 순서·JNI 응답·입력 연결(값은 로컬 주입)
platform/          ios, macos 호스트 서비스
```

의존은 한 방향이다. `adapters → engine`, `platform → engine`이고 엔진은 어댑터를 모른다. 엔진 코드에 앱 이름이나 앱 고유 상수가 보이면 경계 위반이다.

## 필요한 인터페이스

지금 코드가 이미 "관찰된 호출만 구현하고 나머지는 명시적으로 정지"하는 구조라서 틈이 작다. 필요한 것은 다음 둘이다.

1. **`JavaHost`(가칭)**: `JavaBootstrap`이 클래스 조회, 멤버 조회, 객체 생성, 메서드 호출 결과를 어댑터에 묻는 추상 인터페이스. 기본 구현은 지금과 같이 "알 수 없으면 정지"다.
2. **`AppAdapter`** (`shell/app_adapter.hpp`, 계획 때 이름은 `AppProfile`): `SessionConfig::adapter`로 넘기고 `GuestSession`이 이것만 보고 구동한다. 세션 하나에 어댑터 하나다.
   - 라이브러리: `find_library`(데이터 폴더에서 찾기), `missing_library`, `unpack_library`(APK·앱 고유 포장 풀기)
   - 파일: `configure_files`(별칭, 읽기 필터)
   - Java: `install_java`/`uninstall_java`(Java 메서드 등록과 정리), `deliver`(Java 쪽이 나중에 보내는 응답을 게스트 호출로 전달)
   - 시작 순서: `startup()`이 돌려주는 `AppStartup` 값(receiver 클래스, init, 표면 호출 목록, 프레임 호출, 종료 폴링)
   - 프레임 요청: `frames_on_request`, `frame_requested`, `frame_drawn`, `frame_ready`, `take_frame_request_count`
   - 어댑터는 `AppHost`로 세션 기능을 쓴다: export 주소, 네이티브 메서드 조회, 세션의 정지 보고를 따르는 게스트 호출, 중첩 호출, 표면 크기, 로그.

   어댑터가 없으면 세션은 ELF 이미지를 그대로 읽어 생성자와 `JNI_OnLoad`까지만 실행한다. 입력 바인딩은 지금처럼 `SessionConfig::input_bindings`(파일에서 읽는 어댑터 값)로 따로 받는다.

## 단계별 계획

진행 중인 브랜치(오디오, 게스트 스레딩, ABI/JNI)가 건드리는 파일과 겹치지 않게 순서를 잡았다.

| 단계 | 내용 | 충돌 위험 | 시점 |
| --- | --- | --- | --- |
| 0 | 이 문서와 로드맵 항목 | 없음(문서만) | 완료 |
| 1 | 규칙 확정: 새 코드는 엔진/어댑터 중 어디인지 PR 설명에 적는다. 엔진 코드에 앱 고유 이름·상수 금지를 `AGENTS.md`에 반영 | 없음 | 완료(`AGENTS.md`). PR을 쓰지 않으므로 커밋 메시지에 적음 |
| 2 | `JavaHost` 인터페이스 추가와 `JavaBootstrap`이 이를 쓰도록 변경. 기본 구현은 현재 동작 유지 | `src/jni.cpp`·`jni.hpp`: ABI/JNI 브랜치와 겹침 | 해당 브랜치가 main에 들어간 뒤 |
| 3 | `AppProfile` 도입과 `GuestSession`이 시작 순서를 프로필에서 읽도록 변경 | `shell/session.*`: 스레딩 브랜치와 겹칠 수 있음 | 완료(`AppAdapter`, `adapters/atlan/`). 동작 변화 없음 |
| 4 | 합성 어댑터 추가: 엔진 단독으로 JNI·시작·입력 경로를 합성 이미지로 검증하는 테스트 | 새 파일 위주 | 시작 경로는 `tests/shell_tests.cpp`의 합성 어댑터로 검증 중. JNI·입력 경로는 단계 2 뒤 |
| 5 | 디렉터리 이동(`git mv`만, 내용 변경 없음)과 CMake 타깃 정리. 기존 `atlan_runtime`은 별칭으로 유지 | 모든 브랜치와 충돌 가능 | 진행 중 브랜치가 모두 정리된 시점에 한 번에 |
| 6 | 엔진 공개 API 정리, 누락 함수 감지(M10)를 엔진 기능으로 고정, 별도 저장소로 분리할지 판단 | 낮음 | 구동 안정 이후 |

오디오·그래픽 같은 신규 호스트 서비스는 처음부터 "엔진 서비스 + 어댑터가 채우는 설정"으로 나눠 만든다. 그러면 단계 5의 이동 때 손댈 것이 줄어든다.

## 분리할 때의 주의

- **범위**: 지금은 한 앱이 부르는 함수만 구현한다. 다른 앱은 빠진 함수를 채워야 하고, 누락 함수 감지(M10)가 그 목록을 보여 준다. "범용"은 구조가 앱에 묶이지 않는다는 뜻이지 호환 범위를 보장한다는 뜻이 아니다.
- **라이선스**: CPU 코어가 GPL-2.0-or-later 출처이므로 엔진을 따로 올리면 같은 조건을 이어받는다. 분리 전에 파일별 SPDX와 출처를 다시 확인한다.
- **공개 범위**: 엔진만 공개한다. 원본 앱의 바이너리, 분석 자료, 다운로드 주소와 취득 방법은 지금처럼 `original/`·`private/`에만 둔다. 어댑터의 앱별 값도 같다.
- **되돌리기**: 단계 5 이전에는 디렉터리 이동이 없으므로 언제든 멈춰도 현재 구조가 그대로 남는다.
