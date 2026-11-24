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

## 3. 지도 넣기

- 파일 앱 → **내 iPhone > (앱 이름)** 폴더 = 앱의 Documents 폴더야. 여기에 지도 파일/폴더를 직접 복사하거나 옮겨 넣어.
- 앱은 복사하지 않고 Documents 안에서 가장 큰 일반 파일을 그 자리에서 읽어. 앱을 켤 때와 실행할 때 상단에 파일 이름과 크기가 보여.
- 지도를 바꾸려면 같은 폴더의 파일을 바꿔 넣으면 돼.
- 실행하면 로그에 `map ok` (또는 `map rejected: …` / `map missing`) 가 찍혀. 지금 단계에서는 `map missing` 이어도 다음 단계로 진행되고, 지도는 아직 게스트 코드에 연결되어 있지 않아.

## 4. 실행

두 번째 버튼(▶︎)을 누른다. 게스트는 전용 스레드에서 돌고 로그는 화면 아래 텍스트 영역에 쌓여.

## 지금 기대하는 로그

앱을 켜면 먼저 `selftest <숫자>` 가 나와. 실행하면 대략 이런 순서야 (숫자는 기기마다 같아야 해).

```
loaded mapped_bytes=16486400 exports=25963 imports_trapped=263 initializers=415
initializers_completed=415
android_log 4 ATLAN30_LOG: JNI_OnLoad called
android_log 4 ATLAN30_LOG: JNI_OnLoad called 2
android_log 4 ATLAN30_LOG: JNI_OnLoad: registerNatives function is success
jni class ... (com/atlan/… 클래스 여러 줄)
jni native ... (등록된 네이티브 메서드 여러 줄)
JNI_OnLoad returned 0x10004
pump stop=0 retired=0 live_threads=1
audio callbacks=0 players=0
```

- 정상 종료 지점은 `audio callbacks=0 players=0` 줄이야. 여기까지 나오고 아무것도 더 안 뜨는 게 **현재 예상 결과**야.
- `JNI_OnLoad returned 0x10004` 는 JNI 버전 1.4 를 돌려준다는 뜻이고, 이게 이번 단계의 핵심 확인 포인트야.
- 앱 셸은 아직 `nativeInit` 을 부르지 않아. (서버에서 `nativeInit` 까지 돌리면 지도 폴더의 `ATLAN/MAP/WAVE/*.idx` 를 열려 하고, 지금은 그다음 `srand48` 서비스가 없어서 멈춰. 앱에서 보이는 단계는 아니야.)
- 오디오는 연결만 되어 있어서 지금은 소리도 안 나고 플레이어도 만들어지지 않아 (`players=0`).

## 실패했을 때 로그의 의미

| 로그 | 의미 |
| --- | --- |
| `library …` 없이 `import failed: …` | 파일 가져오기 실패 |
| `load failed: …` | ELF 로드 실패 (잘못된 파일, ARM32 가 아님) |
| `initializer[N] …` 로 시작하는 줄 | N 번째 생성자에서 멈춤 (뒤에 이유가 붙어) |
| `JNI_OnLoad …` 로 시작하는 실패 줄 | JNI 초기화 중 멈춤 |
| `pump stopped at missing service …` | 게스트 스레드가 아직 없는 서비스를 호출 |
| `audio callback: …` | 오디오 콜백 실행 중 오류 |

## 리포트할 때

- 로그 영역 **전체 스크린샷** (길면 위쪽과 아래쪽을 나눠서). 마지막 줄이 보이게 해줘.
- 가장 중요한 건 마지막 몇 줄과 `JNI_OnLoad returned …` 줄이야.
- 기기 모델, iOS 버전, 설치에 쓴 도구.
- 앱이 죽었다면(튕김) 어느 시점에서 죽었는지, 그리고 설정 → 개인정보 보호 및 보안 → 분석 및 향상 → 분석 데이터에서 `AtlanRuntime` 크래시 로그가 있으면 같이 보내줘.
- 멈췄다면 얼마나 기다렸는지, 기기가 뜨거워졌는지.
