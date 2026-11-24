# 게스트 스레드

원본 라이브러리가 만드는 작업 스레드를 위한 모델이다. 구현은 `GuestThreads`(`include/atlan/threads.hpp`)와 `ImportDispatcher`의 실행 루프다.

## 선택: 호스트 스레드 하나 위의 협력 스케줄링

게스트 스레드마다 CPU 컨텍스트(레지스터, TLS 포인터, 스택)를 따로 두지만 전부 `Runtime`을 소유한 호스트 스레드 하나에서 번갈아 실행한다. 게스트 스레드마다 호스트 스레드를 만들지 않는 이유는 다음과 같다.

- `Memory`(소프트웨어 페이지 맵)와 힙·파일·stdio·OpenSL ES 같은 호스트 서비스가 모두 단일 스레드 전제다. 호스트 스레드를 늘리면 전부에 락과 정밀한 exclusive 모델이 필요하고, 그 상태에서 생기는 경쟁은 재현하기 어렵다.
- 한 번에 컨텍스트 하나만 실행하므로 게스트의 LDREX/STREX 원자 연산이 그대로 맞다. 전환할 때는 exclusive monitor를 해제한다(실제 컨텍스트 스위치와 같다).
- 같은 클록이면 스케줄이 결정적이라 회귀 검사와 디버깅이 쉽다. iOS 백그라운드 스레드 제약과도 무관하다.
- 비용은 원본이 한 코어 이상을 쓰지 못한다는 점이다. 병렬 실행이 성능 병목으로 확인되면 그때 다시 본다.

## 전환 시점

1. 게스트가 import에서 막힐 때: `pthread_mutex_lock`(다른 스레드가 보유), `pthread_cond_wait`/`timedwait`, `sem_wait`/`timedwait`, `pthread_join`, `sleep`/`usleep`/`nanosleep`, `sched_yield`.
2. 한 스레드가 `quantum` 명령(기본 20000)을 쓰는 동안 다른 스레드가 준비 상태일 때. 플래그를 돌며 기다리는 스핀 루프가 있어도 값을 쓰는 스레드가 굶지 않는다.

막는 서비스는 재시도 방식이다(`ImportCall::block`). 조건을 확인하고, 거짓이면 레지스터를 건드리지 않고 `block(key, deadline)`을 호출한다. 깨어난 스레드는 같은 import를 다시 실행한다. 그래서 깨우기 쪽(`unlock`, `sem_post` 등)은 `GuestThreads::wake(key)`만 부르면 되고, 깨움이 겹치거나 불필요해도 안전하다. 조건 변수는 대기자 큐와 스레드별 단계 상태(대기 → 신호 → 뮤텍스 재획득)를 가져서 `signal`은 정확히 한 스레드만 깨운다.

## 서비스

| 영역 | import |
| --- | --- |
| 스레드 | `pthread_create` `join` `detach` `self` `equal` `exit`, `pthread_attr_*`(init/destroy, detachstate, stacksize, guardsize, schedpolicy, schedparam, scope), `pthread_setname_np`, `pthread_setschedparam`(무시), `gettid` |
| 시간 | `sched_yield` `usleep` `sleep` `nanosleep` |
| 뮤텍스 | `pthread_mutex_*`(init/destroy/lock/trylock/timedlock/unlock), `pthread_mutexattr_*`, `pthread_once` |
| 조건 변수 | `pthread_cond_*`(init/destroy/signal/broadcast/wait/timedwait/`timedwait_monotonic_np`), `pthread_condattr_*` |
| 세마포어 | `sem_*`(init/destroy/post/wait/trywait/timedwait/getvalue) |
| TLS | `pthread_key_*`, `get/setspecific`: 값은 스레드별이고 스레드 종료 때 destructor를 최대 4회전 실행 |

- 스레드마다 스택(기본 1 MiB, attr로 지정하면 최대 8 MiB)과 TLS 페이지를 `0x71000000` 영역에서 받는다. 스택 아래는 매핑하지 않은 보호 페이지다. `pthread_t`는 TLS 페이지 주소이고, 슬롯 0은 자기 자신, 슬롯 1은 스레드 ID다. `a32_tpidruro`가 그 주소를 가리킨다.
- `errno` 워드는 컨텍스트 전환마다 스레드별 값으로 바꿔 넣으므로 게스트에는 스레드별로 보인다(`Config::errno_address`).
- 종료한 스레드는 `join`/`detach` 시점에 스택을 해제한다.
- 일반 뮤텍스는 Bionic처럼 소유자를 검사하지 않고 해제를 허용한다. 재귀·오류 검사 뮤텍스는 EPERM을 돌려준다.

## 한계와 진단

- 호스트 서비스가 게스트를 다시 부르는 콜백(`pthread_once` 초기화 함수)은 호출한 스레드 위에서 중첩 실행되며 선점되지도 막히지도 않는다. 그 안에서 막는 import가 나오면 이유와 함께 멈춘다.
- 모든 스레드가 막혔고 깨울 방법이 없으면 `Stop::deadlock`과 막힌 스레드·import·주소 목록을 돌려준다. 스레드가 하나뿐일 때도 같다.
- 모델링하지 않은 pthread import(rwlock, barrier, `pthread_kill` 등)는 기존처럼 이름과 함께 `unresolved_import`로 멈춘다.
- 대기 시간은 기본적으로 호스트 시계(`steady_clock`)로 실제로 잔다. 테스트는 가상 시계를 주입한다.
- 대기 중인 스레드는 게스트 호출(`call`)이 실행되는 동안에만 진행한다. 호출 밖에서도 진행시키려면 임베더가 `ImportDispatcher::pump(runtime, budget)`을 주기적으로 부른다. 준비된 스레드가 없으면 `returned`로 즉시 끝나고, 시간 대기만 남았으면 `GuestThreads::next_wake_ns()`가 가장 이른 마감을 알려준다.
- 한 스레드가 오류로 멈추면 그 컨텍스트가 `Runtime`에 남고 `CallOutcome::thread`가 번호를 알려준다.

## 확인

`tests/thread_tests.cpp`가 `tests/guest/threads.s`의 합성 ARM32 프로그램으로 join, 세마포어 핸드셰이크, 스핀 선점, 조건 변수, 스레드별 TLS와 종료 destructor, `pthread_exit`, 가상 시계 sleep·timedwait, 교착 진단, `pump`, `sched_yield` 순서를 확인한다. `tests/guest/generate.sh`가 어셈블리에서 `tests/thread_guest.inc`를 다시 만든다(llvm-mc 필요). 원본 라이브러리로의 실행 확인은 로컬에서 `atlan-elf --pump N`으로 한다.
