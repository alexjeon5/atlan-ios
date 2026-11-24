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

Neoverse-N1 2.0 GHz(2 vCPU VM), GCC 13.3 Release, 반복 5회 중 최고값, 합성 워크로드 1,800만 명령 기준. 값은 이 기기에서만 의미가 있고 iOS 기기 값이 아니다.

| 워크로드 | 인터프리터 | 블록 캐시 | 배율 |
|---|---|---|---|
| alu | 8.35 | 35.41 | 4.2× |
| memory | 8.37 | 29.94 | 3.6× |
| callret | 8.60 | 37.05 | 4.3× |
| vfp | 12.76 | 40.48 | 3.2× |
| thumb | 10.33 | 33.40 | 3.2× |

단계별(alu, 같은 기기): 인터프리터 8.3 → 디코드 결과만 캐시 29.1 → 핸들러까지 미리 해석 35.4 MIPS.

## 구조

`BlockRunner`(`block_cache.hpp`)는 PC 키와 ITSTATE로 블록을 찾고, 없으면 최대 64개 명령을 한 번 디코드해 `Instruction`과 해석된 핸들러 포인터(`prepare_a32`)를 저장한다. 실행 중에는 디코더와 `op_info`·적용 범위 검사·연산 switch가 없다. 런타임 JIT나 실행 가능 메모리는 쓰지 않는다. 블록은 데이터이고 호출 경로는 정적 핸들러 함수뿐이다.

- 블록은 분기, 예외, PC 쓰기 특성의 명령에서 끝나고, 실행 중에도 PC가 순차 위치와 다르면 즉시 끝난다.
- A32에서 VFP로 라우팅될 수 있는 단어(`fp_state` 특성, 정수 코어가 다루지 않는 형식, 코프로세서 검사)는 `Runtime::step()`과 같은 순서로 VFP 실행기를 먼저 거친다.
- `Memory::generation()`이 `map()`/`protect()`마다 오르고 바뀌면 캐시 전체를 버린다. 쓰기 가능 + 실행 가능 상태가 없어 게스트 저장이 코드를 바꿀 수 없다.
- 블록을 만들 수 없는 PC(매핑 없음, 실행 권한 없음, 정렬 오류)는 `Runtime::step()`이 같은 이벤트를 보고한다.
- 용어: 지금 구현은 핸들러 포인터를 호출하는 call-threaded 방식이다. 레이블 점프 방식(computed goto)이나 tail-call 체인은 아니다.

## 차분 테스트

`block-cache-differential`(`tests/block_cache_tests.cpp`)이 `Runtime::step/run`과 `BlockRunner`를 같은 초기 상태에서 돌려 결과 이벤트·주소·신드롬·레지스터·플래그·VFP 상태·ITSTATE·배타 모니터·메모리를 비교한다. 합성 워크로드를 임의 예산으로 쪼개 실행하고, 임의 A32/Thumb 단어 단일 명령, 임의 단어 프로그램, 코드 덮어쓰기 후 무효화, 실행 권한 제거, 페이지 경계, 예산 0 경계를 다룬다. `exclusive_key`는 호스트 포인터라 비교에서 존재 여부만 본다. VFP 라우팅을 일부러 끄면 이 테스트가 실패하는 것을 확인했다.

## 남은 병목과 한계 (추정은 따로 표시)

- 실측: 캐시 후에도 `Memory::translate`가 `std::map` 조회라 memory 워크로드가 가장 느리다(29.9 vs 35.4 MIPS).
- 실측: `Instruction`은 352바이트라 블록당 메모리가 크다. 호출마다 `Exec`를 만든다.
- 추정(미측정): 주소 변환 TLB, 플래그 지연 계산, 명령 융합(CMP+Bcc), 블록 체이닝이 다음 이득 후보다.
- 아직 연결하지 않음: `ImportDispatcher`/`Runtime::call`은 여전히 `step()`을 쓴다. `step_block(budget)`로 교체할 수 있지만 다른 세션의 파일이라 건드리지 않았다.
- iOS 기기 값, 발열·배터리, 실제 앱 코드 분포에 대한 값은 없다.
