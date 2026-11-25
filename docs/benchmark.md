# 벤치마크와 블록 캐시

## 하네스

`atlan-bench`(`bench/bench.cpp`)는 합성 ARM32 워크로드를 실행해 처리량(MIPS)을 잰다. 프로그램은 `bench/workloads.hpp`에 손으로 인코딩한 공개 코드이며 원본 앱 데이터를 쓰지 않는다. 워크로드: `alu`(정수 연산 루프), `memory`(레지스터 오프셋 LDR/STR), `callret`(BL/BX LR), `vfp`(스칼라 VFP 루틴 경로), `thumb`(T16 정수 루프).

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-rel
./build-rel/atlan-bench --iterations 3000000 --repeats 5   # --workload, --mode interp|cached|both, --csv
```

두 모드는 같은 프로그램을 돌린 뒤 레지스터·PC·플래그·벡터·데이터 메모리 digest가 같은지 확인한다. 다르면 종료 코드 1이다. 원본 라이브러리에 대한 측정은 `atlan-elf`로 하고 결과는 무시되는 `private/`에만 둔다.

## 측정값 (실측)

x86-64 클라우드 VM(4 vCPU), GCC 13.3 Release, 반복 3회 중 최고값, 합성 워크로드 1,800만 명령 기준. 값은 이 기기에서만 의미가 있고 iOS 기기 값이 아니다. 원본 라이브러리와 지도 데이터로는 `ATLAN_PERF=1 atlan-session …`으로 잰다. 서버(Neoverse-N1 VM)에서 첫 실측은 createGLSF 6.59억 명령에 4.45초(148 MIPS)였고 부하에 따라 13.6초까지 흔들렸다. 그 전 경로는 같은 호출에 약 3분이 걸렸다.

| 워크로드 | `Runtime::step()` | 블록 캐시 | 배율 |
|---|---|---|---|
| alu | 11.7 | 465 | 40× |
| memory | 12.3 | 368 | 30× |
| callret | 11.8 | 407 | 34× |
| vfp | 18.8 | 247 | 13× |
| thumb | 14.6 | 372 | 25× |

단계별(alu, 같은 기기): `step()` 11 → 명령마다 블록 조회(이전 구현) 80 → 블록이 명령 열 전체를 담고 다음 블록을 연결 107 → 전용 핸들러 465 MIPS. 같은 기기에서 Clang 18은 alu 515, thumb 427 MIPS다.

VM 시간 측정은 ±30% 흔들려서 작은 변경은 callgrind의 명령 수(게스트 명령 하나당 호스트 명령 수, `--mode cached`)로 비교한다. 2026-11-25 핫 패스 정리(플래그 계산, PC를 블록 끝에서만 기록, LSL 즉치 특수화, 루프 정리) 전후: alu 43.5 → 31.6, memory 66.1 → 43.5, callret 51.8 → 47.0, vfp 62.4 → 53.2, thumb 58.3 → 47.6.

## 구조

`BlockRunner`(`block_cache.hpp`)는 PC 키와 ITSTATE로 블록을 찾고, 없으면 최대 64개 명령을 한 번 디코드한다. 런타임 JIT나 실행 가능 메모리는 쓰지 않는다. 블록은 데이터이고 호출 경로는 정적 핸들러 함수뿐이다.

- 전용 핸들러는 PC와 ITSTATE를 쓰지 않는다. 블록 루프가 연속 실행이 끝나는 곳(블록 끝, 느린 경로 명령 앞)에서 `next_key`/`next_it`로 한 번 기록하고, 제어를 옮기는 핸들러만 직접 쓴다.
- 명령마다 48바이트 `FastOp` 하나를 둔다(`semantics_fast.cpp`, `prepare_fast`). 데이터 처리(즉치·레지스터·즉치 시프트·레지스터 시프트, PC 읽기와 PC 쓰기 포함), MOVW/MOVT/ADR, 바이트·하프워드·워드 LDR/STR(리터럴·PC 기준·PC로 로드 포함), LDM/STM/PUSH/POP, B/BL/BX/BLX/CBZ/CBNZ, SXT/UXT, MUL/MLA/MLS, IT/NOP은 연산·피연산자 형태별로 특수화한 핸들러가 실행한다. 조건은 16비트 표 한 번으로 판정하고, 분기 대상과 리터럴 주소는 블록을 만들 때 상수로 계산한다.
- 스칼라 VFP(A32)도 `vfp::prepare_fast`가 같은 `FastOp`로 미리 해석한다: 산술, VMOV/VABS/VNEG/VSQRT, VCMP(E), 정수·정밀도 변환, 코어 레지스터 전송, VMRS/VMSR, VLDR/VSTR/VLDM/VSTM. 산술은 `execute_a32`와 같은 함수를 호출한다(NaN payload가 피연산자 순서에 따라 달라져서 식을 복사하면 컴파일러에 따라 결과가 갈린다).
- 전용 핸들러는 빠른 경로로 끝낼 수 없으면(폴트, 페이지를 넘는 접근) 상태를 바꾸기 전에 물러나고, 그 명령은 `Runtime::step()`이 실행하거나 이벤트를 보고한다. 폴트 순서와 보고 내용은 기존 경로 그대로다.
- 전용 핸들러가 없는 명령(VFP, 배타 접근, 레지스터 시프트 등)만 전체 디코드 결과(`Instruction`, 352바이트)와 `prepare_a32` 핸들러를 따로 보관한다.
- 형태 특성(`alu_pc`, `load_pc`, `branch`)은 "PC를 쓸 수 있는 형태"라는 뜻이라 블록을 끊는 기준으로 쓰지 않는다. 실행 중 PC가 다음 명령이 아니면 블록을 떠나고, 디코드는 예외·무조건 분기·실제 PC 쓰기에서 멈춘다. 조건 분기 뒤로는 계속 이어진다.
- 블록은 마지막으로 이어진 다음 블록 두 개를 기억해 표 조회를 건너뛴다(체이닝). 캐시는 명령 131,072개를 넘으면 전부 버린다.
- `Memory::code_generation()`은 실행 가능 페이지가 관련된 `map()`/`unmap()`/`protect()`에서만 바뀌고, 바뀌면 캐시 전체를 버린다. 데이터 페이지만 매핑·해제할 때는 캐시가 유지된다. 값은 프로세스 전체에서 유일하므로 같은 주소에 다시 만든 `Runtime`도 구별된다. 쓰기 가능 + 실행 가능 상태가 없어 게스트 저장이 코드를 바꿀 수 없다.
- `Memory::translate`는 `std::map` 대신 2단계 페이지 테이블(10 + 10비트)을 읽는 인라인 함수다.
- `ImportDispatcher`는 기본으로 `BlockRunner::run()`을 쓴다. 반환 주소·스레드 종료 sentinel·import 트랩은 실행 불가 주소라 블록 실행이 그 PC에서 실행 폴트로 멈추고, 디스패처가 기존과 같은 순서로 검사한다. 남은 예산과 스레드 quantum은 실행 한도로 넘겨 명령 단위까지 같다. `set_block_cache(false)`(도구에서는 `ATLAN_NO_BLOCK_CACHE=1`)로 `step()` 경로를 고를 수 있다.
- 대화형 단계는 10초마다 `perf frames:` 한 줄을 로그에 남긴다: fps, 그린 프레임·요청·깨어난 횟수, 그린 프레임당 렌더 시간(ms)·명령 수·MIPS, 렌더·게스트 스레드·오디오·대기가 차지한 시간 비율(%). iOS 앱은 블록 통계와 import 상위 목록, import별 호스트 시간(ms/호출 수)도 함께 남긴다. 프레임 사이에는 게스트 타이머(usleep, 시한 대기)와 입력에 맞춰 깨어난다(2~8 ms).
- `ATLAN_PERF=1 atlan-session …`은 surface/render 호출마다 명령 수·시간·MIPS와 블록 통계, 전용 핸들러가 없어 느린 경로로 간 명령 형태 상위 목록을 로그에 남긴다. 다음 전용 핸들러 후보는 이 목록으로 고른다.

## 게스트 함수별 프로파일

`GuestProfiler`(`include/atlan/profiler.hpp`)는 게스트 실행을 함수 단위로 나눠 호출 수, 명령 수, 호스트 시간을 센다. 각 값은 함수 자기 코드만(self)과 반환할 때까지(inclusive, 피호출 함수와 호스트 서비스 포함) 두 가지이고, 호출 관계(누가 누구를 몇 번)도 남긴다.

- 함수 시작 주소는 export 심볼과 import 트랩, 그리고 실행 중 호출 대상으로 관찰된 주소다. 코드는 바로 아래의 가장 가까운 함수 시작에 귀속된다.
- 호출은 "블록이 끝난 뒤 LR이 그 블록 바로 다음 주소"로, 반환은 "PC가 프레임의 반환 주소"로 판단한다. 호출자의 반환 주소와 스택이 복원된 채 함수 밖으로 나가면 꼬리 호출로 본다. self 값은 함수 시작 목록에만 의존하고, inclusive와 호출 수는 longjmp 같은 비정상 이동 근처에서 근사다.
- 디스패처가 블록 하나씩 실행하게 되어 체이닝이 꺼진다. 명령 수는 정확하지만 시간에는 측정 비용이 섞이므로 함수끼리 비교하는 용도로만 쓴다. 실행 결과는 바뀌지 않는다.
- `ATLAN_PROFILE=접두사 atlan-session …`은 단계별로 `접두사.<단계>.tsv`를 쓴다: `init`(생성자·`JNI_OnLoad`·`nativeInit`), surface 호출별, `render`(처음 `ATLAN_FRAMES` 프레임), `frames`(대화형 10초 구간마다, 반복되면 번호가 붙음). iOS 앱은 Documents에 `profile` 폴더가 있으면 그 안에 같은 파일을 쓴다.
- `tools/profile_report.py 파일.tsv [--frames N] [--top N]`이 self 명령 수, inclusive 명령 수, self 시간 순의 표와 함수별 주요 호출자를 출력한다. 프로파일 파일에는 원본 라이브러리의 심볼과 주소가 들어 있으므로 저장소에 넣지 않는다.
- 프로파일로 고른 함수를 호스트 구현으로 바꾸는 방법은 [네이티브 대체](native.md)에 있다.
- `guest-profiler`(`tests/profiler_tests.cpp`)가 합성 게스트(루프 안 직접 호출, 레지스터를 통한 import 호출, 꼬리 호출)로 호출 수·self·inclusive·호출 관계를 확인한다.

## 차분 테스트

`block-cache-differential`(`tests/block_cache_tests.cpp`)이 `Runtime::step/run`과 `BlockRunner`를 같은 초기 상태에서 돌려 결과 이벤트·주소·신드롬·레지스터·플래그·VFP 상태·ITSTATE·배타 모니터·메모리를 비교한다. 합성 워크로드를 임의 예산으로 쪼개 실행하고, 임의 A32/Thumb 단어 단일 명령, 포인터를 페이지 경계와 매핑 끝에 몰아 둔 단일 명령(전용 핸들러가 물러나는 경로), 임의 단어 프로그램, import를 부르는 프로그램을 디스패처의 두 경로로 임의 예산만큼 실행한 결과, 코드 덮어쓰기 후 무효화, 실행 권한 제거, 페이지 경계, 예산 0 경계를 다룬다. `exclusive_key`는 호스트 포인터라 비교에서 존재 여부만 본다. 시계를 읽는 MRRC(CNTVCT)는 두 실행의 시각이 달라 값이 다를 수 있다.

## 남은 병목과 한계 (추정은 따로 표시)

- 원본 라이브러리의 명령 분포로 잰 값이 없다. 합성 루프는 전용 핸들러만 타므로 실제 배율은 이보다 낮을 수 있다(추정).
- 서버 프레임 실측(800x480, x86-64): renderGLSF 한 번에 440만~990만 명령, 64~108ms(평균 약 80 MIPS). 전용 핸들러 없이 돈 명령은 단정밀도 VFP가 대부분이었고, 프레임당 import 호출이 약 5만 번(memset·memcmp·memcpy·뮤텍스)이었다. 그 뒤 VFP 핸들러를 넣고 메모리 서비스를 페이지 단위로 바꿨다. 바꾼 뒤의 프레임 값은 아직 없다.
- 전용 핸들러가 없는 것: LDRD/STRD, TBB/TBH, 비트 필드, 긴 곱셈, 배타 접근, Thumb의 VFP. 첫 실측에서 전용 핸들러 없이 실행된 명령은 6.59억 중 2,900만이었고, 그 대부분(레지스터 시프트 MOV, PC 상대 ADD/LDR, BX pc, MLA)에는 이후 핸들러를 넣었다.
- import 호출은 실행 불가 PC에서 블록 실행이 바로 fetch 폴트를 돌려주는 것으로 시작한다(`step()`을 거치지 않는다). 트랩 주소에서 서비스와 카운터를 찾는 조회는 주소별로 캐시한다. `Memory::fill/move/page_span`으로 memset·memcpy·memmove·memcmp·strcmp·strlen이 호스트 버퍼 없이 페이지 단위로 동작한다.
- 바꾼 함수(UDF 트랩) 호출은 블록 루프가 바로 보고하고, 나눗셈은 루프 안에서 끝낸다(`docs/native.md`). 기기 모의주행 8배속에서 `__aeabi_idivmod`가 10초에 300만~530만 번 불린 것이 계기다.
- 추정(미측정): 실제 빈도로 고른 명령 융합(CMP+Bcc 등), `musttail` 꼬리 호출 체인, 빌드 시점 AOT가 다음 후보다.
- iOS 기기 값, 발열·배터리에 대한 값은 없다.
