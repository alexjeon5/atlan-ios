# 기기 테스트 가이드

CI 가 만든 서명 없는 IPA 를 iPhone/iPad 에 설치해서 앱 셸이 어디까지 가는지 확인하는 방법이야. 원본 앱·라이브러리·지도 파일은 저장소에 없고, 각자 가진 파일을 직접 넣어야 해.

## 준비물

- iOS/iPadOS 15 이상 기기, 가로 모드 전용 앱
- iloader 같은 사이드로딩 도구 (IPA 서명·설치용)
- 본인이 가진 ARM32 네이티브 라이브러리 `libatlan3dv5.so`
- 본인이 가진 지도 데이터 폴더/파일

## 1. IPA 받기

1. GitHub 저장소 → Actions → 가장 최근 `main` 의 `Runtime` 워크플로 run 을 연다 (초록 체크인지 확인).
2. 아래 Artifacts 에서 `atlan-runtime-ios-unsigned` 를 받는다. zip 안에 `.ipa` 가 들어 있어.
3. iloader 로 서명하고 기기에 설치한다. 신뢰 설정(설정 → 일반 → VPN 및 기기 관리)이 필요할 수 있어.

## 2. 라이브러리 가져오기

1. 기기의 파일 앱에 `libatlan3dv5.so` 를 넣어 둔다 (iCloud Drive 든 "내 iPhone" 이든 상관없어).
2. 앱을 열고 왼쪽 위 첫 번째 버튼(아래 화살표 아이콘)을 눌러 파일 선택기에서 그 `.so` 를 고른다.
3. 로그에 `library libatlan3dv5.so` 가 뜨면 성공. 앱이 `Application Support/guest.so` 로 복사해 두니까 다시 안 골라도 돼.

## 3. 지도·데이터 넣기

- 파일 앱 → **내 iPhone > (앱 이름)** 폴더 = 앱의 Documents 폴더야. 게스트는 이 폴더를 루트로 보고 `ATLAN/MAP/WAVE/…`, `ATLAN/APP/…`, `ATLAN/UI/…` 같은 상대 경로를 열어. 그래서 `ATLAN` 폴더 구조를 그대로 이 폴더 안에 복사해 넣어야 해.
- 앱이 `ATLAN/User/…` 아래에 설정 파일을 직접 만들어 쓰기도 해.
- Documents 안에서 가장 큰 일반 파일을 상단 지도 표시와 `map ok` 검사에 쓰는데, 지금 단계에서는 참고용이야. 데이터가 없어도 실행은 끝까지 가고, 없는 파일은 `fopen … failed` 로그로 남아.
- 데이터를 바꾸려면 같은 폴더의 파일을 바꿔 넣으면 돼.

## 4. 실행

두 번째 버튼(▶︎)을 누른다. 게스트는 전용 스레드에서 돌고 로그는 화면 아래 텍스트 영역에 쌓여.

## 지금 기대하는 로그

앱을 켜면 먼저 `selftest <숫자>` 가 나와. 실행하면 대략 이런 순서야 (서버에서 같은 코드를 돌린 결과 기준이야).

```
loaded mapped_bytes=16486400 exports=25963 imports_trapped=262 initializers=415
initializers_completed=415
android_log 4 ATLAN30_LOG: JNI_OnLoad called
android_log 4 ATLAN30_LOG: JNI_OnLoad called 2
android_log 4 ATLAN30_LOG: JNI_OnLoad: registerNatives function is success
jni class ... / jni native ... (여러 줄)
JNI_OnLoad returned 0x10004
android_log 3 atlan MainActivity: nativeInit()
opensles ... (오디오 엔진 생성 관련 여러 줄)
fopen ATLAN/... mode=... ok|failed (여러 줄)
java MainActivity.setSurfaces(0,0)
nativeInit returned 0x0
pump stop=0 retired=... live_threads=4
audio callbacks=0 players=0
```

- 정상 종료 지점은 `audio callbacks=0 players=0` 줄이야. 이 줄까지 나오면 이번 단계는 성공이고, 그 뒤에 아무것도 안 뜨는 게 **현재 예상 결과**야.
- 핵심 확인 줄은 `JNI_OnLoad returned 0x10004` 와 `nativeInit returned 0x0` 이야.
- 지도·데이터를 안 넣었으면 `fopen ATLAN/MAP/WAVE/….idx … failed` 가 찍혀. 그래도 `nativeInit returned 0x0` 까지는 가. 데이터를 넣으면 `ok` 로 바뀌는지, 그 뒤에 새로 멈추는 곳이 있는지 보는 게 이번 테스트의 목적이야.
- `setSurfaces` 는 화면 연결이 아직 없어서 호출만 기록하고 아무 일도 안 해. 화면에는 아직 아무것도 그려지지 않아.
- 소리도 안 나고 오디오 플레이어도 만들어지지 않아 (`players=0`).

## 실패했을 때 로그의 의미

| 로그 | 의미 |
| --- | --- |
| `library …` 없이 `import failed: …` | 파일 가져오기 실패 |
| `load failed: …` | ELF 로드 실패 (잘못된 파일, ARM32 가 아님) |
| `initializer[N] …` 로 시작하는 줄 | N 번째 생성자에서 멈춤 (뒤에 이유가 붙어) |
| `JNI_OnLoad …` 로 시작하는 실패 줄 | JNI 초기화 중 멈춤 |
| `nativeInit …` 로 시작하는 실패 줄 | `nativeInit` 중 멈춤 (없는 서비스 이름이나 예산 초과가 붙어) |
| `pump stopped at missing service …` | 게스트 스레드가 아직 없는 서비스를 호출 |
| `audio callback: …` | 오디오 콜백 실행 중 오류 |

## 리포트할 때

- 로그 영역 **전체 스크린샷** (길면 위쪽과 아래쪽을 나눠서). 마지막 줄이 보이게 해줘.
- 가장 중요한 건 마지막 몇 줄과 `nativeInit …` 줄이야. `fopen … failed` 로 시작하는 줄이 많으면 그 경로 목록도 같이 보여줘.
- 기기 모델, iOS 버전, 설치에 쓴 도구.
- 앱이 죽었다면(튕김) 어느 시점에서 죽었는지, 그리고 설정 → 개인정보 보호 및 보안 → 분석 및 향상 → 분석 데이터에서 `AtlanRuntime` 크래시 로그가 있으면 같이 보내줘.
- 멈췄다면 얼마나 기다렸는지, 기기가 뜨거워졌는지.
